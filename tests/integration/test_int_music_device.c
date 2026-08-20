/* Music against a real audio device: the half of streaming playback that a
 * headless world cannot reach.
 *
 * Everything in test_int_music.c is bookkeeping -- handles, scopes, the state
 * machine -- and passes with no device at all. This file covers what only a
 * device can show: that LoadMusicStream really opens the file, that raylib's
 * default looping is off before the first frame, that asking for the playing
 * track again does not rewind it, that a non-looping track ends by itself,
 * and that the streams are freed. The last one is not hypothetical -- the WAV
 * decoder context raylib forgets to free (see unload_music_fully in
 * asset/asset.c) leaks on exactly this path and nowhere else.
 *
 * Labeled "render" in CTest, like the other tests that need a window:
 *     ctest -LE render
 * A machine with a display but no sound card skips instead of failing.
 * See plan/09-testing.md. */
#include "asset/asset.h"
#include "audio/audio.h"
#include "core/engine.h"
#include "core/rl_alloc.h"
#include "mye_test.h"

#include <math.h>
#include <raylib.h>

#include <stdio.h>
#include <stdlib.h>

#define FIXED_DT (1.0f / 60.0f)
#define TRACK_SECONDS 0.4f

static void make_path(const char *name, char *out, size_t out_size)
{
    const char *base = getenv("TMPDIR");
    char dir[512];
    snprintf(dir, sizeof dir, "%s/mye_music_test",
             base != NULL && base[0] != '\0' ? base : "/tmp");
    if (!DirectoryExists(dir)) {
        MakeDirectory(dir);
    }
    snprintf(out, out_size, "%s/%s", dir, name);
}

/* mye_rl_malloc, not malloc: UnloadWave frees the samples with raylib's
 * allocator, which is the engine's tracking one (core/rl_alloc.h). */
static bool write_test_wav(const char *path, float seconds)
{
    unsigned int rate = 22050;
    unsigned int frames = (unsigned int)(seconds * (float)rate);
    short *samples = (short *)mye_rl_malloc(frames * sizeof(short));
    if (samples == NULL) {
        return false;
    }
    for (unsigned int i = 0; i < frames; ++i) {
        float t = (float)i / (float)rate;
        samples[i] = (short)(sinf(2.0f * PI * 440.0f * t) * 6000.0f);
    }
    Wave wave = { .frameCount = frames, .sampleRate = rate, .sampleSize = 16,
                  .channels = 1, .data = samples };
    bool ok = ExportWave(wave, path);
    UnloadWave(wave);
    return ok;
}

TEST(a_real_stream_plays_through_and_leaves_nothing_behind)
{
    /* Must precede InitWindow, which mye_init performs. */
    SetConfigFlags(FLAG_WINDOW_HIDDEN);

    ecs_world_t *world = mye_init(&(mye_config){
        .width = 320, .height = 200, .title = "myecs music device" });
    if (world == NULL) {
        SKIP("no window could be created");
    }
    if (!IsAudioDeviceReady()) {
        /* A display but no sound card: the registry took the deviceless path,
         * which test_int_music.c already covers in full. */
        ASSERT_EQ_INT(0, mye_shutdown(world));
        SKIP("no audio device");
    }

    char path[600];
    make_path("device.wav", path, sizeof path);
    ASSERT_TRUE(write_test_wav(path, TRACK_SECONDS));

    mye_music track = mye_music_load(world, path);
    ASSERT_TRUE(mye_music_valid(world, track));

    const Music *stream = mye_music_get(world, track);
    ASSERT_NOT_NULL(stream);
    /* A real decoder behind a real device buffer -- not the empty slot a
     * deviceless world records. */
    ASSERT_NOT_NULL(stream->stream.buffer);
    ASSERT_TRUE(stream->frameCount > 0);
    /* raylib's loaders turn looping ON. The engine does not loop behind the
     * user's back, so the loaded track must arrive with it off. */
    ASSERT_FALSE(stream->looping);

    /* Loading is not playing, on the device path either. */
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);
    ASSERT_FALSE(IsMusicStreamPlaying(*stream));

    mye_music_play_ex(world, track, 0.5f, false);
    ASSERT_TRUE(IsMusicStreamPlaying(*mye_music_get(world, track)));

    /* The stream is fed from the once-per-frame flush, so the play position
     * has to advance across frames -- if the pump were dropped, the buffers
     * would never be refilled. */
    for (int i = 0; i < 12; ++i) {
        mye_progress(world, FIXED_DT);
    }
    float played = GetMusicTimePlayed(*mye_music_get(world, track));
    ASSERT_TRUE(played > 0.0f);

    /* Asking again for the track already playing is not a restart: the
     * position must not fall back to the beginning. This is the property that
     * lets music do without the sound queue. */
    mye_music_play_ex(world, track, 0.5f, false);
    ASSERT_TRUE(GetMusicTimePlayed(*mye_music_get(world, track)) >= played);

    /* A non-looping track runs out and clears itself. Watched rather than
     * timed: how many frames 0.4 s takes depends on the machine. */
    bool ended = false;
    for (int i = 0; i < 900 && !ended; ++i) {
        mye_progress(world, FIXED_DT);
        ended = mye_music_current(world).generation == 0;
    }
    ASSERT_TRUE(ended);

    /* Looping, by contrast, keeps going past the end of the file. */
    mye_music_play_ex(world, track, 0.3f, true);
    for (int i = 0; i < 120; ++i) {
        mye_progress(world, FIXED_DT);
    }
    ASSERT_EQ_INT((int)track.generation,
                  (int)mye_music_current(world).generation);

    /* Left playing on purpose and never released by hand: the registry's own
     * teardown has to stop and unload the stream, and mye_shutdown's zero is
     * the assertion that it freed every last byte of it. */
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(a_real_stream_plays_through_and_leaves_nothing_behind))
