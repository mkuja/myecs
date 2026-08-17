/* The asset registry: handles, deduplication, refcounting and staleness.
 *
 * Loading is synchronous -- assets are loaded at scene boundaries, so there
 * is no pipeline to test, only the registry's bookkeeping. Headless, so it
 * runs everywhere. See plan/06-assets.md. */
#include "asset/asset.h"
#include "core/engine.h"
#include "mye_test.h"

#include <raylib.h>

#include <stdio.h>

#define TEST_DIR "/tmp/mye_assets_test"

/* A real PNG on disk, so the test exercises actual file I/O rather than a
 * stub that could hide a decode failure. */
static bool write_test_png(const char *path, int width, int height, Color fill)
{
    if (!DirectoryExists(TEST_DIR)) {
        MakeDirectory(TEST_DIR);
    }
    Image image = GenImageColor(width, height, fill);
    bool ok = ExportImage(image, path);
    UnloadImage(image);
    return ok;
}

static void make_path(const char *name, char *out, size_t out_size)
{
    if (!DirectoryExists(TEST_DIR)) {
        MakeDirectory(TEST_DIR);
    }
    snprintf(out, out_size, "%s/%s", TEST_DIR, name);
}

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){ .headless = true });
}

TEST(a_loaded_texture_is_valid_and_resolvable)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    char path[600];
    make_path("one.png", path, sizeof path);
    ASSERT_TRUE(write_test_png(path, 16, 24, RED));

    mye_texture tex = mye_texture_load(world, path);
    ASSERT_TRUE(mye_texture_valid(world, tex));

    /* Headless records the dimensions without a GPU upload, which is what
     * makes registry behaviour testable without a window. */
    const Texture2D *resolved = mye_texture_get(world, tex);
    ASSERT_TRUE(resolved != NULL);
    ASSERT_EQ_INT(16, resolved->width);
    ASSERT_EQ_INT(24, resolved->height);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The same file asked for twice must occupy one slot: without this, a scene
 * that references a shared texture from ten entities uploads it ten times. */
TEST(loading_the_same_path_twice_shares_one_slot)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    char path[600];
    make_path("shared.png", path, sizeof path);
    ASSERT_TRUE(write_test_png(path, 8, 8, BLUE));

    mye_texture a = mye_texture_load(world, path);
    mye_texture b = mye_texture_load(world, path);
    mye_texture c = mye_texture_load(world, path);

    ASSERT_TRUE(mye_texture_valid(world, a));
    ASSERT_EQ_INT((int)a.index, (int)b.index);
    ASSERT_EQ_INT((int)a.index, (int)c.index);
    ASSERT_EQ_INT((int)a.generation, (int)c.generation);

    mye_asset_stats stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(1, (int)stats.textures_live);

    /* Three loads, three references: the slot survives the first two
     * releases and goes away on the third. */
    mye_texture_release(world, a);
    mye_texture_release(world, b);
    ASSERT_TRUE(mye_texture_valid(world, c));
    mye_texture_release(world, c);
    ASSERT_TRUE(!mye_texture_valid(world, c));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_missing_file_fails_without_a_valid_handle)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    char path[600];
    make_path("does_not_exist.png", path, sizeof path);

    mye_texture tex = mye_texture_load(world, path);
    ASSERT_TRUE(!mye_texture_valid(world, tex));
    ASSERT_TRUE(mye_texture_get(world, tex) == NULL);

    /* The magenta placeholder is a GPU texture, so a headless world has
     * none to hand back. In a windowed build this is what makes a missing
     * asset show up on screen instead of dereferencing nothing. */
    ASSERT_TRUE(mye_texture_get_or_placeholder(world, tex) == NULL);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A handle kept across a release must not resolve to whatever lands in that
 * slot next -- that is the whole reason handles carry a generation. */
TEST(a_stale_handle_does_not_resolve_to_the_next_tenant)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    char first[600];
    char second[600];
    make_path("first.png", first, sizeof first);
    make_path("second.png", second, sizeof second);
    ASSERT_TRUE(write_test_png(first, 8, 8, GREEN));
    ASSERT_TRUE(write_test_png(second, 32, 32, YELLOW));

    mye_texture old = mye_texture_load(world, first);
    ASSERT_TRUE(mye_texture_valid(world, old));
    mye_texture_release(world, old);

    mye_texture fresh = mye_texture_load(world, second);
    ASSERT_TRUE(mye_texture_valid(world, fresh));
    ASSERT_TRUE(!mye_texture_valid(world, old));
    ASSERT_TRUE(mye_texture_get(world, old) == NULL);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_generated_texture_needs_no_file)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    Image image = GenImageColor(12, 12, PURPLE);
    mye_texture tex = mye_texture_from_image(world, "gen:test", image);
    ASSERT_TRUE(mye_texture_valid(world, tex));

    /* Named, so asking again shares the slot rather than uploading twice. */
    Image other = GenImageColor(12, 12, PURPLE);
    mye_texture again = mye_texture_from_image(world, "gen:test", other);
    ASSERT_EQ_INT((int)tex.index, (int)again.index);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(a_loaded_texture_is_valid_and_resolvable),
          TEST_CASE(loading_the_same_path_twice_shares_one_slot),
          TEST_CASE(a_missing_file_fails_without_a_valid_handle),
          TEST_CASE(a_stale_handle_does_not_resolve_to_the_next_tenant),
          TEST_CASE(a_generated_texture_needs_no_file))
