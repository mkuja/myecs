/* Sound playback. See plan/00-overview.md (Tier 1: audio).
 *
 * Requests are queued, not played immediately, and the queue is flushed once
 * per frame. That matters because fixed-timestep systems can run several
 * times in a single frame: firing a weapon during a catch-up would otherwise
 * stack three identical shots into one audible blast. Identical sounds
 * requested in the same frame collapse into one.
 *
 *   mye_sound_play(world, sfx_fire);              // from any system
 *   mye_sound_play_ex(world, sfx_hit, 0.8f, 1.2f); // volume, pitch
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

typedef struct MyeAudio {
    mye_sound_request queue[MYE_MAX_QUEUED_SOUNDS];
    int queued;
    float master_volume; /* scales everything, 0..1 */
    bool muted;
    /* Requests dropped because the queue was full, for debugging. */
    uint32_t dropped;
} MyeAudio;

extern ECS_COMPONENT_DECLARE(MyeAudio);

void MyeAudioModuleImport(ecs_world_t *world);

/* Queue a sound for this frame. Safe to call from fixed-step systems. */
void mye_sound_play(ecs_world_t *world, mye_sound sound);
void mye_sound_play_ex(ecs_world_t *world, mye_sound sound, float volume,
                       float pitch);

void mye_audio_set_master_volume(ecs_world_t *world, float volume);
void mye_audio_set_muted(ecs_world_t *world, bool muted);

/* The queue logic, exposed for tests: pure, no device required. Returns false
 * if the request was dropped (queue full). Collapses duplicates. */
bool mye_audio_enqueue(MyeAudio *audio, mye_sound sound, float volume,
                       float pitch);

#endif /* MYE_AUDIO_AUDIO_H */
