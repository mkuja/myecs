/* Sound and music playback. See plan/00-overview.md (Tier 1: audio).
 *
 * Sound requests are queued, not played immediately, and the queue is flushed
 * once per frame. That matters because fixed-timestep systems can run several
 * times in a single frame: firing a weapon during a catch-up would otherwise
 * stack three identical shots into one audible blast. Identical sounds
 * requested in the same frame collapse into one.
 *
 *   mye_sound_play(world, sfx_fire);               // from any system
 *   mye_sound_play_ex(world, sfx_hit, 0.8f, 1.2f); // volume, pitch
 *
 *   mye_music_play(world, theme);                  // once, at full volume
 *   mye_music_play_ex(world, theme, 0.6f, true);   // volume, looping
 *
 * MUSIC IS NOT QUEUED, and the asymmetry is deliberate. A sound is an event,
 * so N requests in a frame are N events to collapse; music is a *state* --
 * which one track is playing, and how loud. mye_music_play asks for a track
 * to be playing, and asking for the track that already is does nothing at
 * all: no restart, no second stream. Several fixed steps in one frame
 * therefore produce exactly one stream without any queue to arbitrate them,
 * and so does a system that calls it every frame. A queue would only add a
 * frame of latency to a decision the last caller in the frame already wins.
 * (To rewind deliberately: mye_music_stop then mye_music_play.)
 *
 * One track plays at a time -- starting another stops the first. Nothing
 * autoplays and nothing loops unless asked; loading a track only opens it.
 *
 * The stream is fed once per frame from the same EcsPreStore flush that
 * drains the sound queue: raylib decodes the next buffer's worth in
 * UpdateMusicStream, and a frame that skips it is a frame the music stutters.
 *
 * The audio device belongs to the main thread, like the window. Never call
 * these from a job (see plan/05-concurrency.md).
 */
#ifndef MYE_AUDIO_AUDIO_H
#define MYE_AUDIO_AUDIO_H

#include "asset/asset.h"
#include "core/engine.h"

#define MYE_MAX_QUEUED_SOUNDS 32

typedef struct mye_sound_request {
    mye_sound sound;
    float volume; /* 0..1 */
    float pitch;  /* 1.0 = unchanged */
} mye_sound_request;

/* The one music track, if any. Plain data, so a test can read the whole of
 * playback state without a sound card. */
typedef struct mye_music_track {
    mye_music track;  /* zero handle when nothing is playing */
    float volume;     /* 0..1, before master_volume */
    bool loop;
    bool paused;
} mye_music_track;

typedef struct MyeAudio {
    mye_sound_request queue[MYE_MAX_QUEUED_SOUNDS];
    int queued;
    float master_volume; /* scales everything, 0..1 */
    bool muted;
    /* Requests dropped because the queue was full, for debugging. */
    uint32_t dropped;
    mye_music_track music;
} MyeAudio;

extern ECS_COMPONENT_DECLARE(MyeAudio);

void MyeAudioModuleImport(ecs_world_t *world);

/* Queue a sound for this frame. Safe to call from fixed-step systems. */
void mye_sound_play(ecs_world_t *world, mye_sound sound);
void mye_sound_play_ex(ecs_world_t *world, mye_sound sound, float volume,
                       float pitch);

void mye_audio_set_master_volume(ecs_world_t *world, float volume);
void mye_audio_set_muted(ecs_world_t *world, bool muted);

/* ----------------------------------------------------------------- music -- */

/* Starts `music` from the beginning, once, at full volume. A no-op if the
 * handle is invalid or has been released -- a track that went away with its
 * scene must not take the game down with it. */
void mye_music_play(ecs_world_t *world, mye_music music);

/* volume is 0..1, before master volume; loop repeats until stopped. */
void mye_music_play_ex(ecs_world_t *world, mye_music music, float volume,
                       bool loop);

/* Pause keeps the position; stop forgets it and leaves nothing current.
 * Resuming what was never paused, or stopping silence, is a no-op. */
void mye_music_pause(ecs_world_t *world);
void mye_music_resume(ecs_world_t *world);
void mye_music_stop(ecs_world_t *world);

/* The current track's own volume. Master volume and mute still apply on top;
 * muting silences music without pausing it, so a muted track keeps its
 * place. */
void mye_music_set_volume(ecs_world_t *world, float volume);

/* The track playing or paused right now, or a zero handle for neither. A
 * non-looping track that reaches its end clears itself on the next frame. */
mye_music mye_music_current(const ecs_world_t *world);
bool mye_music_paused(const ecs_world_t *world);

/* The queue logic, exposed for tests: pure, no device required. Returns false
 * if the request was dropped (queue full). Collapses duplicates. */
bool mye_audio_enqueue(MyeAudio *audio, mye_sound sound, float volume,
                       float pitch);

#endif /* MYE_AUDIO_AUDIO_H */
