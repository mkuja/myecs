#define _POSIX_C_SOURCE 200809L
/* The sprite pass, checked by reading pixels back. This is 09-testing.md's
 * worked example, `int_render_sprites`.
 *
 * The 2D pipeline is where a game spends most of its frames, and until now
 * nothing looked at what it actually put on screen: the render smoke test
 * draws circles by hand and the camera tests are all 3D. Two things are
 * asserted here, and both are things a plausible-looking renderer can get
 * wrong -- that a sprite lands where the camera puts it, and that layer
 * decides which of two overlapping sprites the player sees.
 *
 * Real GL through a hidden window, so it carries the "render" label like the
 * other pixel tests:  ctest -LE render  skips it on a machine with no
 * display. See plan/09-testing.md. */
#include "asset/asset.h"
#include "core/engine.h"
#include "render/camera.h"
#include "render/render2d.h"
#include "scene/transform.h"
#include "mye_test.h"

#include <raylib.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define W 640
#define H 360

/* Counts pixels in a rect where red dominates -- the same test the camera
 * render tests use. "Dominates" rather than "equals RED": the clear colour is
 * a near-black grey with r == g == b, and a driver is free to round a channel
 * on its way through the framebuffer. */
static int count_red(const Image *img, Rectangle rect)
{
    int red = 0;
    for (int y = (int)rect.y; y < (int)(rect.y + rect.height); ++y) {
        for (int x = (int)rect.x; x < (int)(rect.x + rect.width); ++x) {
            if (x < 0 || y < 0 || x >= img->width || y >= img->height) {
                continue;
            }
            Color c = GetImageColor(*img, x, y);
            if (c.r > 60 && c.r > c.g * 2 && c.r > c.b * 2) {
                ++red;
            }
        }
    }
    return red;
}

/* The same, for the other sprite in each overlapping pair. */
static int count_green(const Image *img, Rectangle rect)
{
    int green = 0;
    for (int y = (int)rect.y; y < (int)(rect.y + rect.height); ++y) {
        for (int x = (int)rect.x; x < (int)(rect.x + rect.width); ++x) {
            if (x < 0 || y < 0 || x >= img->width || y >= img->height) {
                continue;
            }
            Color c = GetImageColor(*img, x, y);
            if (c.g > 60 && c.g > c.r * 2 && c.g > c.b * 2) {
                ++green;
            }
        }
    }
    return green;
}

/* Pixels are read from INSIDE the frame, before the buffer swap: the engine's
 * screenshot hook does exactly that. Reading after mye_progress returns gets
 * the swapped-out back buffer -- the previous frame on most drivers. */
static void arm_screenshot(ecs_world_t *world, char *path, size_t path_size,
                           const char *stem)
{
    snprintf(path, path_size, "%s/mye_%s_%d.png",
             getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp", stem,
             (int)getpid());
    mye_engine *engine = mye_engine_get(world);
    engine->screenshot_path = path;
}

/* A solid 64x64 sprite entity at a world position, on a given layer. */
static ecs_entity_t spawn_square(ecs_world_t *world, const char *name,
                                 Color color, float x, float y, int16_t layer)
{
    mye_texture tex = mye_texture_from_image(world, name,
                                             GenImageColor(64, 64, color));
    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, MyePosition2D, { x, y });
    ecs_set(world, e, MyeSprite,
            { .texture = tex, .origin = { 32.0f, 32.0f }, .tint = WHITE,
              .layer = layer });
    return e;
}

TEST(a_sprite_is_drawn_where_its_camera_puts_it)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs sprite render test",
        .max_frames = 1 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    /* A camera at the origin, so a sprite at the world origin lands dead
     * centre: no arithmetic of mine between the feature and the assertion. */
    mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    spawn_square(world, "test:red", RED, 0.0f, 0.0f, 0);

    char path[256];
    arm_screenshot(world, path, sizeof path, "sprite");

    while (mye_running(world)) {
        mye_progress(world, 1.0f / 60.0f);
    }

    Image shot = LoadImage(path);
    remove(path);
    ASSERT_TRUE(shot.data != NULL);

    Rectangle where = { (float)W / 2.0f - 32.0f, (float)H / 2.0f - 32.0f,
                        64.0f, 64.0f };
    int red_here = count_red(&shot, where);
    /* The left third of the window, which the sprite comes nowhere near. */
    Rectangle elsewhere = { 0.0f, 0.0f, (float)W / 3.0f, (float)H };
    int red_elsewhere = count_red(&shot, elsewhere);
    UnloadImage(shot);

    /* Nearly the whole 64x64 square, allowing for the edge pixels. */
    ASSERT_TRUE(red_here > 3800);
    ASSERT_EQ_INT(0, red_elsewhere);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Layer decides which of two overlapping sprites the player sees.
 *
 * Two pairs, crossed: on the left the green square is the higher layer, on
 * the right the red one is. That is what makes this a layer test rather than
 * a spawn-order test -- if the sort ignored layer entirely, both pairs would
 * be settled by the same tie-break (y, then texture address) and would come
 * out the SAME colour as each other. Crossed, they must come out different,
 * and each must be the higher-layered one. */
TEST(a_higher_layer_draws_over_a_lower_one)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs sprite layer test",
        .max_frames = 1 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);

    /* Left pair: red underneath, green on top. Spawned low-then-high. */
    spawn_square(world, "test:red_lo", RED, -120.0f, 0.0f, 0);
    spawn_square(world, "test:green_hi", GREEN, -120.0f, 0.0f, 5);

    /* Right pair: green underneath, red on top -- and spawned in the
     * opposite order, high first, so creation order argues the other way
     * from layer on this side. */
    spawn_square(world, "test:red_hi", RED, 120.0f, 0.0f, 5);
    spawn_square(world, "test:green_lo", GREEN, 120.0f, 0.0f, 0);

    char path[256];
    arm_screenshot(world, path, sizeof path, "sprite_layers");

    while (mye_running(world)) {
        mye_progress(world, 1.0f / 60.0f);
    }

    Image shot = LoadImage(path);
    remove(path);
    ASSERT_TRUE(shot.data != NULL);

    Rectangle left = { (float)W / 2.0f - 152.0f, (float)H / 2.0f - 32.0f,
                       64.0f, 64.0f };
    Rectangle right = { (float)W / 2.0f + 88.0f, (float)H / 2.0f - 32.0f,
                        64.0f, 64.0f };
    int green_left = count_green(&shot, left);
    int red_left = count_red(&shot, left);
    int red_right = count_red(&shot, right);
    int green_right = count_green(&shot, right);
    UnloadImage(shot);

    /* Left: green (layer 5) covers red (layer 0) completely. */
    ASSERT_TRUE(green_left > 3800);
    ASSERT_EQ_INT(0, red_left);
    /* Right: the same rule, with the colours and the spawn order swapped. */
    ASSERT_TRUE(red_right > 3800);
    ASSERT_EQ_INT(0, green_right);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(a_sprite_is_drawn_where_its_camera_puts_it),
          TEST_CASE(a_higher_layer_draws_over_a_lower_one))
