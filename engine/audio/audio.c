#include "audio/audio.h"

#include <raylib.h>

ECS_COMPONENT_DECLARE(MyeAudio);

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

bool mye_audio_enqueue(MyeAudio *audio, mye_sound sound, float volume,
                       float pitch)
{
    if (audio == NULL || sound.generation == 0) {
        return false;
    }

    /* Collapse duplicates: several fixed steps in one frame must not stack
     * the same sound into a single loud blast. The loudest request wins. */
    for (int i = 0; i < audio->queued; ++i) {
        if (audio->queue[i].sound.index == sound.index &&
            audio->queue[i].sound.generation == sound.generation) {
            if (volume > audio->queue[i].volume) {
                audio->queue[i].volume = volume;
            }
            return true;
        }
    }

    if (audio->queued >= MYE_MAX_QUEUED_SOUNDS) {
        ++audio->dropped;
        return false;
    }

    audio->queue[audio->queued++] = (mye_sound_request){
        .sound = sound,
        .volume = clamp01(volume),
        .pitch = pitch > 0.0f ? pitch : 1.0f,
    };
    return true;
}

void mye_sound_play_ex(ecs_world_t *world, mye_sound sound, float volume,
                       float pitch)
{
    MyeAudio *audio = ecs_singleton_ensure(world, MyeAudio);
    if (audio == NULL) {
        return;
    }
    mye_audio_enqueue(audio, sound, volume, pitch);
    ecs_singleton_modified(world, MyeAudio);
}

void mye_sound_play(ecs_world_t *world, mye_sound sound)
{
    mye_sound_play_ex(world, sound, 1.0f, 1.0f);
}

void mye_audio_set_master_volume(ecs_world_t *world, float volume)
{
    MyeAudio *audio = ecs_singleton_ensure(world, MyeAudio);
    if (audio != NULL) {
        audio->master_volume = clamp01(volume);
        ecs_singleton_modified(world, MyeAudio);
    }
}

void mye_audio_set_muted(ecs_world_t *world, bool muted)
{
    MyeAudio *audio = ecs_singleton_ensure(world, MyeAudio);
    if (audio != NULL) {
        audio->muted = muted;
        ecs_singleton_modified(world, MyeAudio);
    }
}

/* ----------------------------------------------------------------- music -- */

/* A device-backed stream for the current track, or NULL.
 *
 * Two different NULLs are folded together here on purpose -- no track, and a
 * track with no stream behind it (headless, or a build that came up without
 * an audio device). Either way there is nothing for raylib to do. Callers
 * that need to tell "released" from "silent" ask mye_music_get themselves. */
static const Music *current_stream(const ecs_world_t *world,
                                   const MyeAudio *audio)
{
    const Music *music = mye_music_get(world, audio->music.track);
    return (music != NULL && music->stream.buffer != NULL) ? music : NULL;
}

/* Pushed to the device on every change and once per frame, so master volume
 * and mute reach a stream that is already playing. */
static void music_apply_volume(const MyeAudio *audio, const Music *stream)
{
    float volume = audio->muted
                       ? 0.0f
                       : clamp01(audio->music.volume) * audio->master_volume;
    SetMusicVolume(*stream, volume);
}

/* Forgets the current track without touching the device. Volume and loop are
 * left alone: they are settings, not part of "something is playing". */
static void music_forget(MyeAudio *audio)
{
    audio->music.track = (mye_music){ 0 };
    audio->music.paused = false;
}

static void music_stop_now(ecs_world_t *world, MyeAudio *audio)
{
    const Music *stream = current_stream(world, audio);
    if (stream != NULL) {
        StopMusicStream(*stream);
    }
    music_forget(audio);
}

void mye_music_play_ex(ecs_world_t *world, mye_music music, float volume,
                       bool loop)
{
    MyeAudio *audio = ecs_singleton_ensure(world, MyeAudio);
    if (audio == NULL) {
        return;
    }

    /* The stale-handle guard: a released track resolves to NULL, and asking
     * for it does nothing rather than starting a freed stream. */
    const Music *stream = mye_music_get(world, music);
    if (stream == NULL) {
        return;
    }

    bool same = audio->music.track.index == music.index &&
                audio->music.track.generation == music.generation;
    if (!same) {
        music_stop_now(world, audio); /* one track at a time */
    }

    audio->music.track = music;
    audio->music.volume = clamp01(volume);
    audio->music.loop = loop;

    if (stream->stream.buffer != NULL) {
        if (!same) {
            PlayMusicStream(*stream);
        } else if (audio->music.paused) {
            /* Resume where it was: PlayMusicStream would rewind the device
             * buffer under a decoder that is still mid-track. */
            ResumeMusicStream(*stream);
        }
        music_apply_volume(audio, stream);
    }
    /* `same` and not paused: already playing this track, so nothing to do --
     * see the header on why music needs no queue. */
    audio->music.paused = false;

    ecs_singleton_modified(world, MyeAudio);
}

void mye_music_play(ecs_world_t *world, mye_music music)
{
    mye_music_play_ex(world, music, 1.0f, false);
}

void mye_music_stop(ecs_world_t *world)
{
    MyeAudio *audio = ecs_singleton_ensure(world, MyeAudio);
    if (audio == NULL) {
        return;
    }
    music_stop_now(world, audio);
    ecs_singleton_modified(world, MyeAudio);
}

void mye_music_pause(ecs_world_t *world)
{
    MyeAudio *audio = ecs_singleton_ensure(world, MyeAudio);
    if (audio == NULL || audio->music.track.generation == 0 ||
        audio->music.paused) {
        return;
    }
    const Music *stream = current_stream(world, audio);
    if (stream != NULL) {
        PauseMusicStream(*stream);
    }
    audio->music.paused = true;
    ecs_singleton_modified(world, MyeAudio);
}

void mye_music_resume(ecs_world_t *world)
{
    MyeAudio *audio = ecs_singleton_ensure(world, MyeAudio);
    if (audio == NULL || !audio->music.paused) {
        return;
    }
    const Music *stream = current_stream(world, audio);
    if (stream != NULL) {
        ResumeMusicStream(*stream);
    }
    audio->music.paused = false;
    ecs_singleton_modified(world, MyeAudio);
}

void mye_music_set_volume(ecs_world_t *world, float volume)
{
    MyeAudio *audio = ecs_singleton_ensure(world, MyeAudio);
    if (audio == NULL) {
        return;
    }
    audio->music.volume = clamp01(volume);
    const Music *stream = current_stream(world, audio);
    if (stream != NULL) {
        music_apply_volume(audio, stream);
    }
    ecs_singleton_modified(world, MyeAudio);
}

mye_music mye_music_current(const ecs_world_t *world)
{
    const MyeAudio *audio = ecs_singleton_get(world, MyeAudio);
    return audio != NULL ? audio->music.track : (mye_music){ 0 };
}

bool mye_music_paused(const ecs_world_t *world)
{
    const MyeAudio *audio = ecs_singleton_get(world, MyeAudio);
    return audio != NULL && audio->music.paused;
}

/* Feeds the music stream its next buffer and notices when a track ends or
 * disappears. Must run every frame: raylib decodes on demand here, and a
 * frame that skips it is a frame the music runs dry. */
static void music_pump(ecs_world_t *world, MyeAudio *audio)
{
    if (audio->music.track.generation == 0) {
        return; /* nothing playing */
    }

    const Music *stream = mye_music_get(world, audio->music.track);
    if (stream == NULL) {
        /* Released while it was playing -- typically with the scene that
         * loaded it. Unloading already stopped the stream, so all that is
         * left here is stale bookkeeping, and touching the freed stream is
         * exactly what must not happen. */
        music_forget(audio);
        return;
    }
    if (stream->stream.buffer == NULL) {
        /* Headless: the state machine above is the whole of the behaviour,
         * and with no device there is no clock to end the track either -- it
         * "plays" until something stops it. */
        return;
    }

    /* By value, with looping applied: the registry stores the track, the
     * audio module owns the decision to repeat it, and raylib reads `looping`
     * from the copy it is handed. */
    Music playing = *stream;
    playing.looping = audio->music.loop;

    music_apply_volume(audio, &playing);
    UpdateMusicStream(playing);

    if (!audio->music.paused && !IsMusicStreamPlaying(playing)) {
        /* Ran to its end (a looping track never gets here): leave nothing
         * current, so mye_music_current tells the truth. */
        music_forget(audio);
    }
}

/* Plays everything queued this frame and clears the queue. EcsPreStore, so it
 * runs after all simulation has had its say and before rendering. */
static void MyeAudioFlush(ecs_iter_t *it)
{
    MyeAudio *audio = ecs_field(it, MyeAudio, 0);
    ecs_world_t *world = it->world;

    for (int i = 0; i < audio->queued; ++i) {
        const mye_sound_request *request = &audio->queue[i];
        const Sound *sound = mye_sound_get(world, request->sound);
        if (sound == NULL) {
            continue; /* released or never loaded */
        }

        if (!audio->muted && audio->master_volume > 0.0f) {
            SetSoundVolume(*sound, request->volume * audio->master_volume);
            SetSoundPitch(*sound, request->pitch);
            PlaySound(*sound);
        }
    }

    /* Cleared whether or not anything played, so a muted frame does not
     * leave a backlog to blast out when unmuted. */
    audio->queued = 0;

    music_pump(world, audio);
}

void MyeAudioModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeAudioModule);

    ECS_COMPONENT_DEFINE(world, MyeAudio);
    ecs_add_id(world, ecs_id(MyeAudio), EcsSingleton);
    ecs_singleton_set(world, MyeAudio,
                      { .master_volume = 1.0f, .music = { .volume = 1.0f } });

    /* Registered even when headless: the queue must still drain and the music
     * state machine must still run, so game logic behaves identically with
     * and without a device. mye_sound_get returns NULL there, so nothing is
     * played, and a headless music slot has no stream to feed. */
    ECS_SYSTEM(world, MyeAudioFlush, EcsPreStore, MyeAudio);
}
