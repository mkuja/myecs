/* M4 integration test: the async asset pipeline end to end.
 *
 *   main thread  mye_texture_load_async  -> handle, status LOADING
 *   worker       read file + decode      -> upload_msg on the channel
 *   main thread  MyeAssetUpload (frame)  -> GPU upload, status READY
 *
 * Headless, so it runs everywhere. See plan/06-assets.md. */
#include "asset/asset.h"
#include "core/engine.h"
#include "mye_test.h"

#include <raylib.h>

#include <stdio.h>

#define FIXED_DT (1.0f / 60.0f)
#define MAX_FRAMES_TO_SETTLE 240

static char g_dir[512];

/* Writes a real PNG to disk so the worker has something genuine to decode --
 * the test exercises actual file I/O, not a stub. */
static bool write_test_png(const char *path, int width, int height, Color fill)
{
    Image image = GenImageColor(width, height, fill);
    bool ok = ExportImage(image, path);
    UnloadImage(image);
    return ok;
}

static void make_paths(const char *name, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", g_dir, name);
}

static ecs_world_t *make_world(int workers)
{
    return mye_init(&(mye_config){
        .headless = true,
        .asset_workers = workers,
    });
}

/* Runs frames until nothing is in flight, so the test never sleeps or races. */
static int settle(ecs_world_t *world)
{
    int frames = 0;
    while (!mye_assets_ready(world) && frames < MAX_FRAMES_TO_SETTLE) {
        mye_progress(world, FIXED_DT);
        ++frames;
    }
    /* One more frame so the final upload batch is drained. */
    mye_progress(world, FIXED_DT);
    return frames;
}

TEST(async_load_goes_loading_then_ready)
{
    char path[640];
    make_paths("async_one.png", path, sizeof path);
    ASSERT_TRUE(write_test_png(path, 16, 24, RED));

    ecs_world_t *world = make_world(2);
    ASSERT_NOT_NULL(world);

    mye_texture tex = mye_texture_load_async(world, path);
    ASSERT_TRUE(tex.generation != 0); /* usable handle straight away */

    /* Before the upload runs, the texture is not yet resolvable -- callers
     * get the placeholder, never a half-initialised Texture2D. */
    mye_asset_status early = mye_texture_status(world, tex);
    ASSERT_TRUE(early == MYE_ASSET_LOADING || early == MYE_ASSET_READY);

    settle(world);

    ASSERT_EQ_INT(MYE_ASSET_READY, mye_texture_status(world, tex));
    ASSERT_TRUE(mye_assets_ready(world));
    ASSERT_EQ_U64(0, mye_assets_pending(world));

    const Texture2D *t = mye_texture_get(world, tex);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(16, t->width); /* the real file's dimensions */
    ASSERT_EQ_INT(24, t->height);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(many_concurrent_loads_all_arrive)
{
    /* Several files decoding at once across two workers: every one must land
     * exactly once, with its own dimensions intact. */
    enum { COUNT = 12 };
    char paths[COUNT][640];
    for (int i = 0; i < COUNT; ++i) {
        char name[64];
        snprintf(name, sizeof name, "async_many_%d.png", i);
        make_paths(name, paths[i], sizeof paths[i]);
        ASSERT_TRUE(write_test_png(paths[i], 8 + i, 4 + i, BLUE));
    }

    ecs_world_t *world = make_world(4);
    ASSERT_NOT_NULL(world);

    mye_texture handles[COUNT];
    for (int i = 0; i < COUNT; ++i) {
        handles[i] = mye_texture_load_async(world, paths[i]);
        ASSERT_TRUE(handles[i].generation != 0);
    }
    ASSERT_TRUE(mye_assets_pending(world) > 0);

    settle(world);

    for (int i = 0; i < COUNT; ++i) {
        ASSERT_EQ_INT(MYE_ASSET_READY, mye_texture_status(world, handles[i]));
        const Texture2D *t = mye_texture_get(world, handles[i]);
        ASSERT_NOT_NULL(t);
        /* Right pixels with the right handle: nothing got crossed over. */
        ASSERT_EQ_INT(8 + i, t->width);
        ASSERT_EQ_INT(4 + i, t->height);
    }

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(async_load_dedupes_by_path)
{
    char path[640];
    make_paths("async_dedupe.png", path, sizeof path);
    ASSERT_TRUE(write_test_png(path, 32, 32, GREEN));

    ecs_world_t *world = make_world(2);
    ASSERT_NOT_NULL(world);

    mye_texture a = mye_texture_load_async(world, path);
    mye_texture b = mye_texture_load_async(world, path);
    mye_texture c = mye_texture_load_async(world, path);

    /* One slot, one decode, three references. */
    ASSERT_EQ_U64(a.index, b.index);
    ASSERT_EQ_U64(a.index, c.index);
    ASSERT_EQ_U64(a.generation, b.generation);

    settle(world);
    ASSERT_EQ_INT(MYE_ASSET_READY, mye_texture_status(world, a));

    mye_asset_stats stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(1, (int)stats.textures_live);

    /* Refcounted: it survives until the last holder lets go. */
    mye_texture_release(world, a);
    ASSERT_EQ_INT(MYE_ASSET_READY, mye_texture_status(world, b));
    mye_texture_release(world, b);
    ASSERT_EQ_INT(MYE_ASSET_READY, mye_texture_status(world, c));
    mye_texture_release(world, c);
    ASSERT_EQ_INT(MYE_ASSET_MISSING, mye_texture_status(world, c));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(missing_file_fails_without_hanging)
{
    char path[640];
    make_paths("does_not_exist_anywhere.png", path, sizeof path);

    ecs_world_t *world = make_world(2);
    ASSERT_NOT_NULL(world);

    mye_texture tex = mye_texture_load_async(world, path);
    settle(world);

    /* The pipeline must resolve it, not leave it pending forever. */
    ASSERT_TRUE(mye_assets_ready(world));
    ASSERT_EQ_INT(MYE_ASSET_FAILED, mye_texture_status(world, tex));
    ASSERT_NULL(mye_texture_get(world, tex));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(shutdown_mid_flight_leaks_nothing)
{
    /* The hard case: tear the world down while decodes are still running.
     * Workers must be stopped and their decoded images released, or ASan and
     * the tracking allocator will both complain. */
    enum { COUNT = 16 };
    char paths[COUNT][640];
    for (int i = 0; i < COUNT; ++i) {
        char name[64];
        snprintf(name, sizeof name, "async_abort_%d.png", i);
        make_paths(name, paths[i], sizeof paths[i]);
        ASSERT_TRUE(write_test_png(paths[i], 64, 64, ORANGE));
    }

    ecs_world_t *world = make_world(4);
    ASSERT_NOT_NULL(world);

    for (int i = 0; i < COUNT; ++i) {
        mye_texture_load_async(world, paths[i]);
    }

    /* No settle(): shut down immediately, with work in flight. */
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(disabling_workers_falls_back_to_synchronous)
{
    char path[640];
    make_paths("async_sync.png", path, sizeof path);
    ASSERT_TRUE(write_test_png(path, 20, 10, PURPLE));

    /* Negative worker count disables the pool entirely. */
    ecs_world_t *world = make_world(-1);
    ASSERT_NOT_NULL(world);

    mye_texture tex = mye_texture_load_async(world, path);
    /* Ready immediately: no frame needed, same call site. */
    ASSERT_EQ_INT(MYE_ASSET_READY, mye_texture_status(world, tex));
    ASSERT_TRUE(mye_assets_ready(world));

    const Texture2D *t = mye_texture_get(world, tex);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(20, t->width);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(release_while_loading_is_safe)
{
    /* Releasing a handle whose decode is still in flight must not corrupt the
     * slot when the worker's result finally arrives -- the generation check
     * is what makes the late message harmless. */
    char path[640];
    make_paths("async_release.png", path, sizeof path);
    ASSERT_TRUE(write_test_png(path, 48, 48, YELLOW));

    ecs_world_t *world = make_world(2);
    ASSERT_NOT_NULL(world);

    mye_texture tex = mye_texture_load_async(world, path);
    mye_texture_release(world, tex);

    settle(world);

    /* Either it never landed, or it landed and was discarded. Never READY. */
    ASSERT_TRUE(mye_texture_status(world, tex) != MYE_ASSET_READY);
    ASSERT_TRUE(mye_assets_ready(world));

    /* And the registry is still usable afterwards. */
    mye_texture again = mye_texture_load_async(world, path);
    settle(world);
    ASSERT_EQ_INT(MYE_ASSET_READY, mye_texture_status(world, again));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

int main(void)
{
    /* Scratch directory for the PNGs these tests generate. */
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/mye_async_assets",
             tmp != NULL ? tmp : "/tmp");
    if (!DirectoryExists(g_dir)) {
        MakeDirectory(g_dir);
    }

    static const mye_test_entry tests[] = {
        TEST_CASE(async_load_goes_loading_then_ready),
        TEST_CASE(many_concurrent_loads_all_arrive),
        TEST_CASE(async_load_dedupes_by_path),
        TEST_CASE(missing_file_fails_without_hanging),
        TEST_CASE(shutdown_mid_flight_leaks_nothing),
        TEST_CASE(disabling_workers_falls_back_to_synchronous),
        TEST_CASE(release_while_loading_is_safe),
    };
    return mye_test_run_all(tests, sizeof tests / sizeof tests[0]);
}
