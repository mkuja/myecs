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
}

void MyeAudioModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeAudioModule);

    ECS_COMPONENT_DEFINE(world, MyeAudio);
    ecs_add_id(world, ecs_id(MyeAudio), EcsSingleton);
    ecs_singleton_set(world, MyeAudio, { .master_volume = 1.0f });

    /* Registered even when headless: the queue must still drain, so game
     * logic behaves identically with and without a device. mye_sound_get
     * returns NULL there, so nothing is played. */
    ECS_SYSTEM(world, MyeAudioFlush, EcsPreStore, MyeAudio);
}
