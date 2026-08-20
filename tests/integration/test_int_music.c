/* Streaming music: the registry side (handles, dedupe, staleness, scopes) and
 * the playback state machine.
 *
 * Headless, so there is no audio device and nothing is audible -- what is
 * asserted is that the bookkeeping behaves identically with and without one,
 * which is exactly what a game's logic depends on. The tracks are real files:
 * a WAV synthesised into TMPDIR, so the loader does actual file I/O rather
 * than trusting a stub. See plan/09-testing.md.
 *
 * Every case ends on mye_shutdown() == 0, which is the leak assertion: the
 * registry's slots, the exported wave and everything raylib allocated behind
 * them all go through the tracking allocator. */
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

/* Honours TMPDIR: a hardcoded /tmp path fails confusingly on a shared machine
 * where another user already owns that directory. */
static const char *test_dir(void)
{
    static char dir[512];
    if (dir[0] == '\0') {
        const char *base = getenv("TMPDIR");
        snprintf(dir, sizeof dir, "%s/mye_music_test",
                 base != NULL && base[0] != '\0' ? base : "/tmp");
    }
    return dir;
}

static void make_path(const char *name, char *out, size_t out_size)
{
    const char *dir = test_dir();
    if (!DirectoryExists(dir)) {
        MakeDirectory(dir);
    }
    snprintf(out, out_size, "%s/%s", dir, name);
}

/* A short sine sweep written out as a real WAV. mye_rl_malloc, not malloc:
 * UnloadWave frees the samples with raylib's allocator, which is the engine's
 * tracking one (see core/rl_alloc.h), and mixing the two would show up as a
 * leak or worse. */
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
        samples[i] = (short)(sinf(2.0f * PI * 440.0f * t) * 8000.0f);
    }

    Wave wave = { .frameCount = frames, .sampleRate = rate, .sampleSize = 16,
                  .channels = 1, .data = samples };
    bool ok = ExportWave(wave, path);
    UnloadWave(wave);
    return ok;
}

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){ .headless = true });
}

/* Writes the file and loads it, so each test starts from a known track. */
static mye_music load_track(ecs_world_t *world, const char *name)
{
    char path[600];
    make_path(name, path, sizeof path);
    if (!write_test_wav(path, 0.25f)) {
        return (mye_music){ 0 };
    }
    return mye_music_load(world, path);
}

/* ------------------------------------------------------------- registry -- */

TEST(a_loaded_track_is_valid_and_resolvable)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_music track = load_track(world, "theme.wav");
    ASSERT_TRUE(mye_music_valid(world, track));
    ASSERT_TRUE(track.generation != 0);

    /* Headless records the slot without opening a stream -- that is what
     * keeps the registry and the playback state machine testable without a
     * sound card. */
    const Music *resolved = mye_music_get(world, track);
    ASSERT_NOT_NULL(resolved);
    ASSERT_NULL(resolved->stream.buffer);

    mye_asset_stats stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(1, (int)stats.music_live);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(loading_the_same_path_twice_shares_one_slot)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    char path[600];
    make_path("shared.wav", path, sizeof path);
    ASSERT_TRUE(write_test_wav(path, 0.25f));

    mye_music a = mye_music_load(world, path);
    mye_music b = mye_music_load(world, path);
    mye_music c = mye_music_load(world, path);

    /* Generations too, not just indices: `a` is slot 0 in a fresh world, so a
     * failed second load returning the zero handle would match on index alone
     * and this test would pass with dedupe broken. */
    ASSERT_TRUE(mye_music_valid(world, a));
    ASSERT_TRUE(mye_music_valid(world, b));
    ASSERT_TRUE(mye_music_valid(world, c));
    ASSERT_EQ_INT((int)a.index, (int)b.index);
    ASSERT_EQ_INT((int)a.index, (int)c.index);
    ASSERT_EQ_INT((int)a.generation, (int)b.generation);
    ASSERT_EQ_INT((int)a.generation, (int)c.generation);

    mye_asset_stats stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(1, (int)stats.music_live);

    /* Three loads, three references: the slot survives the first two releases
     * and goes away on the third. */
    mye_music_release(world, a);
    mye_music_release(world, b);
    ASSERT_TRUE(mye_music_valid(world, c));
    mye_music_release(world, c);
    ASSERT_FALSE(mye_music_valid(world, c));

    stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(0, (int)stats.music_live);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_missing_file_fails_without_a_valid_handle)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    char path[600];
    make_path("no_such_track.ogg", path, sizeof path);

    mye_music track = mye_music_load(world, path);
    ASSERT_FALSE(mye_music_valid(world, track));
    ASSERT_NULL(mye_music_get(world, track));

    /* And a typo cannot be played, silently or otherwise. */
    mye_music_play(world, track);
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_stale_handle_does_not_resolve_to_the_next_tenant)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_music old = load_track(world, "first.wav");
    ASSERT_TRUE(mye_music_valid(world, old));
    mye_music_release(world, old);

    mye_music fresh = load_track(world, "second.wav");
    ASSERT_TRUE(mye_music_valid(world, fresh));
    /* The point of the test: `fresh` must be the same slot, so the two
     * handles differ only by generation. */
    ASSERT_EQ_INT((int)old.index, (int)fresh.index);
    ASSERT_TRUE(old.generation != fresh.generation);
    ASSERT_FALSE(mye_music_valid(world, old));
    ASSERT_NULL(mye_music_get(world, old));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* -------------------------------------------------------------- playback -- */

TEST(loading_a_track_does_not_start_it)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_music track = load_track(world, "quiet.wav");
    ASSERT_TRUE(mye_music_valid(world, track));

    /* The engine does nothing behind the user's back: a loaded track is an
     * open file, not a decision to play it. */
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(play_pause_resume_and_stop_move_the_state)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_music track = load_track(world, "state.wav");
    ASSERT_TRUE(mye_music_valid(world, track));

    mye_music_play(world, track);
    ASSERT_EQ_INT((int)track.index, (int)mye_music_current(world).index);
    ASSERT_EQ_INT((int)track.generation,
                  (int)mye_music_current(world).generation);
    ASSERT_FALSE(mye_music_paused(world));

    /* Plain play does not loop: the engine never repeats a track uninvited. */
    const MyeAudio *audio = ecs_singleton_get(world, MyeAudio);
    ASSERT_NOT_NULL(audio);
    ASSERT_FALSE(audio->music.loop);
    ASSERT_NEAR(1.0, audio->music.volume, 1e-6);

    mye_music_pause(world);
    ASSERT_TRUE(mye_music_paused(world));
    /* Paused is still current -- the position is kept. */
    ASSERT_EQ_INT((int)track.generation,
                  (int)mye_music_current(world).generation);

    mye_music_resume(world);
    ASSERT_FALSE(mye_music_paused(world));

    mye_music_stop(world);
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);
    ASSERT_FALSE(mye_music_paused(world));

    /* Stopping silence, and resuming what was never paused, are no-ops. */
    mye_music_stop(world);
    mye_music_resume(world);
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(asking_again_for_the_playing_track_is_not_a_restart)
{
    /* The reason music needs no queue: several fixed steps in one frame all
     * asking for the same track must come to exactly one stream, unpaused
     * only if it was already playing. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_music track = load_track(world, "idempotent.wav");
    mye_music_play_ex(world, track, 0.4f, true);

    const MyeAudio *audio = ecs_singleton_get(world, MyeAudio);
    ASSERT_TRUE(audio->music.loop);
    ASSERT_NEAR(0.4, audio->music.volume, 1e-6);

    /* Same track again, with different settings: the settings take, and the
     * stream is not restarted. */
    mye_music_play_ex(world, track, 0.9f, false);
    audio = ecs_singleton_get(world, MyeAudio);
    ASSERT_EQ_INT((int)track.generation,
                  (int)mye_music_current(world).generation);
    ASSERT_FALSE(audio->music.loop);
    ASSERT_NEAR(0.9, audio->music.volume, 1e-6);

    /* And a paused track that is asked for again resumes rather than
     * rewinding. */
    mye_music_pause(world);
    mye_music_play(world, track);
    ASSERT_FALSE(mye_music_paused(world));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(starting_another_track_replaces_the_first)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_music menu = load_track(world, "menu.wav");
    mye_music level = load_track(world, "level.wav");
    ASSERT_TRUE(mye_music_valid(world, menu));
    ASSERT_TRUE(mye_music_valid(world, level));
    ASSERT_TRUE(menu.index != level.index);

    mye_music_play(world, menu);
    mye_music_play_ex(world, level, 0.5f, true);

    /* One track at a time: the menu theme is not still going underneath. */
    ASSERT_EQ_INT((int)level.index, (int)mye_music_current(world).index);
    ASSERT_EQ_INT((int)level.generation,
                  (int)mye_music_current(world).generation);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(master_volume_and_mute_do_not_disturb_the_state)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_music track = load_track(world, "volume.wav");
    mye_music_play_ex(world, track, 0.8f, true);
    mye_music_set_volume(world, 0.25f);

    const MyeAudio *audio = ecs_singleton_get(world, MyeAudio);
    ASSERT_NEAR(0.25, audio->music.volume, 1e-6);

    /* Out of range is clamped rather than handed to the mixer. */
    mye_music_set_volume(world, 4.0f);
    audio = ecs_singleton_get(world, MyeAudio);
    ASSERT_NEAR(1.0, audio->music.volume, 1e-6);

    /* Muting silences music without pausing it: the track keeps its place. */
    mye_audio_set_muted(world, true);
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT((int)track.generation,
                  (int)mye_music_current(world).generation);
    ASSERT_FALSE(mye_music_paused(world));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* ------------------------------------------------------- stale handles -- */

TEST(playing_a_released_track_is_a_no_op)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_music track = load_track(world, "released.wav");
    ASSERT_TRUE(mye_music_valid(world, track));

    mye_music stale = track;
    mye_music_release(world, track);
    ASSERT_FALSE(mye_music_valid(world, stale));

    /* The handle outlived the asset, which is the whole point of generations:
     * playing it must do nothing at all, not start a freed stream. */
    mye_music_play(world, stale);
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);
    mye_music_play_ex(world, stale, 0.5f, true);
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);

    /* A zero handle is equally harmless. */
    mye_music_play(world, (mye_music){ 0 });
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);

    /* And the per-frame pump runs over all of it without touching anything
     * that was freed. */
    for (int i = 0; i < 5; ++i) {
        mye_progress(world, FIXED_DT);
    }
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(releasing_the_playing_track_clears_it_on_the_next_frame)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_music track = load_track(world, "yanked.wav");
    mye_music_play_ex(world, track, 1.0f, true);
    ASSERT_EQ_INT((int)track.generation,
                  (int)mye_music_current(world).generation);

    /* What a scene switch does to the music it loaded. */
    mye_music_release(world, track);
    mye_progress(world, FIXED_DT);

    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);
    ASSERT_FALSE(mye_music_paused(world));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* ---------------------------------------------------------------- scopes -- */

TEST(a_scope_releases_the_music_it_loaded)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    /* Scope 0 is "unscoped" and is never bulk-released, so the shared track
     * must survive a scene's exit while the scene's own track does not. */
    mye_music shared = load_track(world, "shared_scope.wav");
    ASSERT_TRUE(mye_music_valid(world, shared));

    mye_assets_set_scope(world, 7);
    mye_music scened = load_track(world, "scoped.wav");
    ASSERT_TRUE(mye_music_valid(world, scened));
    mye_assets_set_scope(world, 0);

    mye_asset_stats stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(2, (int)stats.music_live);

    mye_music_play(world, scened);
    mye_assets_release_scope(world, 7);

    ASSERT_FALSE(mye_music_valid(world, scened));
    ASSERT_TRUE(mye_music_valid(world, shared));
    stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(1, (int)stats.music_live);

    /* The track that was playing went with the scope; the pump notices. */
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(0, (int)mye_music_current(world).generation);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_track_two_scopes_asked_for_survives_the_first_release)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    char path[600];
    make_path("two_scopes.wav", path, sizeof path);
    ASSERT_TRUE(write_test_wav(path, 0.25f));

    mye_assets_set_scope(world, 1);
    mye_music a = mye_music_load(world, path);
    mye_assets_set_scope(world, 2);
    mye_music b = mye_music_load(world, path); /* dedupe: one slot, two refs */
    mye_assets_set_scope(world, 0);

    ASSERT_EQ_INT((int)a.index, (int)b.index);

    /* Release is refcounted, so the scope that leaves first does not unload a
     * track the other one is still playing. */
    mye_assets_release_scope(world, 1);
    ASSERT_TRUE(mye_music_valid(world, a));

    /* A sharp edge worth pinning down rather than pretending away: the slot
     * remembers only the scope that FIRST loaded it, so scope 2's bulk
     * release does not match it and the second reference has to be dropped by
     * hand. Same for every asset kind -- this is the registry's rule, not
     * something music does differently. */
    mye_assets_release_scope(world, 2);
    ASSERT_TRUE(mye_music_valid(world, a));
    mye_music_release(world, b);
    ASSERT_FALSE(mye_music_valid(world, a));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* ------------------------------------------------------------- shutdown -- */

TEST(a_track_still_playing_at_shutdown_leaks_nothing)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_music a = load_track(world, "outlives_a.wav");
    mye_music b = load_track(world, "outlives_b.wav");
    ASSERT_TRUE(mye_music_valid(world, a));
    ASSERT_TRUE(mye_music_valid(world, b));

    mye_music_play_ex(world, b, 0.7f, true);
    mye_progress(world, FIXED_DT);

    /* Nothing released by hand: the registry's own teardown has to close both
     * streams, and mye_shutdown's zero is the assertion that it did. */
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(a_loaded_track_is_valid_and_resolvable),
          TEST_CASE(loading_the_same_path_twice_shares_one_slot),
          TEST_CASE(a_missing_file_fails_without_a_valid_handle),
          TEST_CASE(a_stale_handle_does_not_resolve_to_the_next_tenant),
          TEST_CASE(loading_a_track_does_not_start_it),
          TEST_CASE(play_pause_resume_and_stop_move_the_state),
          TEST_CASE(asking_again_for_the_playing_track_is_not_a_restart),
          TEST_CASE(starting_another_track_replaces_the_first),
          TEST_CASE(master_volume_and_mute_do_not_disturb_the_state),
          TEST_CASE(playing_a_released_track_is_a_no_op),
          TEST_CASE(releasing_the_playing_track_clears_it_on_the_next_frame),
          TEST_CASE(a_scope_releases_the_music_it_loaded),
          TEST_CASE(a_track_two_scopes_asked_for_survives_the_first_release),
          TEST_CASE(a_track_still_playing_at_shutdown_leaks_nothing))
