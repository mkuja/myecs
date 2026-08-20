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
#include <stdlib.h>

/* Honours TMPDIR: a hardcoded /tmp path fails confusingly on a shared
 * machine where another user already owns that directory. */
static const char *test_dir(void)
{
    static char dir[512];
    if (dir[0] == '\0') {
        const char *base = getenv("TMPDIR");
        snprintf(dir, sizeof dir, "%s/mye_assets_test",
                 base != NULL && base[0] != '\0' ? base : "/tmp");
    }
    return dir;
}

/* A real PNG on disk, so the test exercises actual file I/O rather than a
 * stub that could hide a decode failure. */
static bool write_test_png(const char *path, int width, int height, Color fill)
{
    Image image = GenImageColor(width, height, fill);
    bool ok = ExportImage(image, path);
    UnloadImage(image);
    return ok;
}

static void make_path(const char *name, char *out, size_t out_size)
{
    const char *dir = test_dir();
    if (!DirectoryExists(dir)) {
        MakeDirectory(dir);
    }
    snprintf(out, out_size, "%s/%s", dir, name);
}

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){ .headless = true });
}

/* A real TTF on disk, which this repository deliberately does not carry:
 * binaries do not belong in git, and raylib can export an image or a wave but
 * has no way to write a font, so there is nothing to generate one from
 * either. The candidates are, in order, an explicit override, the project's
 * own asset directory, the raylib source the build fetched (which ships fonts
 * for its own examples, and sits a couple of directories above the test
 * binary), and two common system font paths. The font tests SKIP when none of
 * them is there, and say so. */
static const char *find_ttf(void)
{
    static char found[1024];
    static bool searched;

    if (searched) {
        return found[0] != '\0' ? found : NULL;
    }
    searched = true;

    const char *override = getenv("MYE_TEST_FONT");
    if (override != NULL && override[0] != '\0' && FileExists(override)) {
        snprintf(found, sizeof found, "%s", override);
        return found;
    }

    static const char *relative[] = {
        "assets/fonts/test.ttf",
        "_deps/raylib-src/examples/text/resources/anonymous_pro_bold.ttf",
    };
    for (size_t i = 0; i < sizeof relative / sizeof relative[0]; ++i) {
        char resolved[1024];
        if (mye_asset_path(relative[i], resolved, sizeof resolved)) {
            snprintf(found, sizeof found, "%s", resolved);
            return found;
        }
    }

    static const char *absolute[] = {
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof absolute / sizeof absolute[0]; ++i) {
        if (FileExists(absolute[i])) {
            snprintf(found, sizeof found, "%s", absolute[i]);
            return found;
        }
    }

    found[0] = '\0';
    return NULL;
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

    /* Generations too, not just indices: `a` is slot 0 in a fresh world, so
     * a failed second load returning the zero handle would match on index
     * alone and this test would pass with dedupe broken. */
    ASSERT_TRUE(mye_texture_valid(world, a));
    ASSERT_TRUE(mye_texture_valid(world, b));
    ASSERT_TRUE(mye_texture_valid(world, c));
    ASSERT_EQ_INT((int)a.index, (int)b.index);
    ASSERT_EQ_INT((int)a.index, (int)c.index);
    ASSERT_EQ_INT((int)a.generation, (int)b.generation);
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
    /* The point of the test: `fresh` must be the same slot, so the two
     * handles differ only by generation. Without this the test would still
     * pass if slots stopped being reused, while claiming to cover a
     * collision it no longer creates. */
    ASSERT_EQ_INT((int)old.index, (int)fresh.index);
    ASSERT_TRUE(old.generation != fresh.generation);
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
    ASSERT_TRUE(mye_texture_valid(world, again));
    ASSERT_EQ_INT((int)tex.index, (int)again.index);
    ASSERT_EQ_INT((int)tex.generation, (int)again.generation);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --- fonts ---------------------------------------------------------------- */

/* raylib rasterises a glyph atlas at load time, so the size is baked into
 * what the slot holds. Deduping on the path alone would hand a 16px atlas to
 * whoever asked for 48px -- text that comes out blurry with nothing on screen
 * to say why. */
TEST(a_font_is_keyed_by_path_and_size)
{
    const char *ttf = find_ttf();
    if (ttf == NULL) {
        SKIP("no TTF available; set MYE_TEST_FONT or see find_ttf()");
    }

    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    mye_font small = mye_font_load(world, ttf, 16);
    mye_font again = mye_font_load(world, ttf, 16);
    mye_font large = mye_font_load(world, ttf, 48);

    ASSERT_TRUE(mye_font_valid(world, small));
    ASSERT_TRUE(mye_font_valid(world, again));
    ASSERT_TRUE(mye_font_valid(world, large));

    /* Same file, same size: one slot, shared. */
    ASSERT_EQ_INT((int)small.index, (int)again.index);
    ASSERT_EQ_INT((int)small.generation, (int)again.generation);
    /* Same file, different size: a slot of its own. */
    ASSERT_TRUE(small.index != large.index);

    mye_asset_stats stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(2, (int)stats.fonts_live);

    /* Headless records the size it was asked for without uploading an atlas,
     * which is what makes registry behaviour testable without a window. */
    const Font *resolved = mye_font_get(world, small);
    ASSERT_NOT_NULL(resolved);
    ASSERT_EQ_INT(16, resolved->baseSize);
    ASSERT_EQ_INT(48, mye_font_get(world, large)->baseSize);

    /* Two loads at 16, so the slot survives the first release. */
    mye_font_release(world, small);
    ASSERT_TRUE(mye_font_valid(world, again));
    mye_font_release(world, again);
    ASSERT_TRUE(!mye_font_valid(world, again));
    ASSERT_TRUE(mye_font_get(world, again) == NULL);

    mye_font_release(world, large);
    ASSERT_EQ_INT(0, (int)mye_asset_stats_get(world).fonts_live);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --- failure records ------------------------------------------------------ */

TEST(a_missing_font_leaves_a_failure_record)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    char path[600];
    make_path("no_such_font.ttf", path, sizeof path);
    remove(path);

    mye_font missing = mye_font_load(world, path, 24);
    ASSERT_TRUE(!mye_font_valid(world, missing));
    ASSERT_TRUE(mye_font_get(world, missing) == NULL);
    /* The default font is a GPU atlas, so a headless world has none to fall
     * back to. In a windowed build this is what keeps missing text visible. */
    ASSERT_TRUE(mye_font_get_or_placeholder(world, missing) == NULL);

    ASSERT_EQ_INT(1, (int)mye_asset_stats_get(world).assets_failed);

    /* Asking again retries in the record's own slot rather than burning a
     * second one -- a game retrying every frame must not fill the registry. */
    mye_font_load(world, path, 24);
    mye_font_load(world, path, 24);
    ASSERT_EQ_INT(1, (int)mye_asset_stats_get(world).assets_failed);

    /* A different size is a different key, so a record of its own. */
    mye_font_load(world, path, 32);
    ASSERT_EQ_INT(2, (int)mye_asset_stats_get(world).assets_failed);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The point of keeping the key: the registry can answer "why did this not
 * resolve", and the answer stops being true the moment the file turns up. */
TEST(a_failure_record_becomes_the_asset_when_the_file_appears)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    char path[600];
    make_path("appears_later.png", path, sizeof path);
    remove(path);

    mye_texture missing = mye_texture_load(world, path);
    ASSERT_TRUE(!mye_texture_valid(world, missing));

    mye_asset_stats failed = mye_asset_stats_get(world);
    ASSERT_EQ_INT(1, (int)failed.assets_failed);
    ASSERT_EQ_INT(0, (int)failed.textures_live);

    ASSERT_TRUE(write_test_png(path, 6, 7, RED));

    mye_texture now = mye_texture_load(world, path);
    ASSERT_TRUE(mye_texture_valid(world, now));
    ASSERT_EQ_INT(6, mye_texture_get(world, now)->width);

    /* assets_failed back to zero is the assertion that matters: the retry
     * took over the record's own slot. Had it claimed a fresh one, the stale
     * record would still be standing and the same path would be counted both
     * loaded and failed. */
    mye_asset_stats loaded = mye_asset_stats_get(world);
    ASSERT_EQ_INT(0, (int)loaded.assets_failed);
    ASSERT_EQ_INT(1, (int)loaded.textures_live);

    mye_texture_release(world, now);
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --- the shutdown report -------------------------------------------------- */

/* plan/06-assets.md promises debug builds report any handle still live at
 * shutdown with the path that loaded it. One warning per asset, so the count
 * is what a test can hold on to; the key is in the message. */
TEST(shutdown_reports_every_asset_still_live)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    char first[600];
    char second[600];
    make_path("held_a.png", first, sizeof first);
    make_path("held_b.png", second, sizeof second);
    ASSERT_TRUE(write_test_png(first, 8, 8, RED));
    ASSERT_TRUE(write_test_png(second, 8, 8, BLUE));

    ASSERT_TRUE(mye_texture_valid(world, mye_texture_load(world, first)));
    ASSERT_TRUE(mye_texture_valid(world, mye_texture_load(world, second)));

    mye_log_counts before = mye_log_get_counts();
    ASSERT_EQ_INT(0, mye_shutdown(world)); /* a report, not an error */
    ASSERT_EQ_U64(before.warn + 2, mye_log_get_counts().warn);
}

TEST(shutdown_says_nothing_when_everything_was_released)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    char path[600];
    make_path("released.png", path, sizeof path);
    ASSERT_TRUE(write_test_png(path, 8, 8, GREEN));

    mye_texture tex = mye_texture_load(world, path);
    ASSERT_TRUE(mye_texture_valid(world, tex));
    mye_texture_release(world, tex);

    mye_log_counts before = mye_log_get_counts();
    ASSERT_EQ_INT(0, mye_shutdown(world));
    ASSERT_EQ_U64(before.warn, mye_log_get_counts().warn);
}

TEST_MAIN(TEST_CASE(a_loaded_texture_is_valid_and_resolvable),
          TEST_CASE(loading_the_same_path_twice_shares_one_slot),
          TEST_CASE(a_missing_file_fails_without_a_valid_handle),
          TEST_CASE(a_stale_handle_does_not_resolve_to_the_next_tenant),
          TEST_CASE(a_generated_texture_needs_no_file),
          TEST_CASE(a_font_is_keyed_by_path_and_size),
          TEST_CASE(a_missing_font_leaves_a_failure_record),
          TEST_CASE(a_failure_record_becomes_the_asset_when_the_file_appears),
          TEST_CASE(shutdown_reports_every_asset_still_live),
          TEST_CASE(shutdown_says_nothing_when_everything_was_released))
