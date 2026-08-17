#define _POSIX_C_SOURCE 200809L
/* Multi-camera rendering, checked by reading pixels back.
 *
 * Two cameras. The main one looks at empty space. The second draws into a
 * corner viewport and looks straight at a red ball that the main camera
 * cannot see. After a frame, the corner must contain red and the rest of the
 * window must not -- otherwise the second camera did not draw, or drew into
 * the wrong place, or bled outside its rect.
 *
 * Real GL through a hidden window, so it is labelled "render" like the smoke
 * test:  ctest -LE render  skips it on machines without a display. */
#include "asset/asset.h"
#include "core/engine.h"
#include "render/camera.h"
#include "render/render3d.h"
#include "scene/transform.h"
#include "mye_test.h"

#include <raylib.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define W 640
#define H 360

/* Counts pixels in a rect where red dominates. The ball is lit, so shades
 * vary; the test is "is anything red here", not a colour match. */
static int count_red(const Image *img, Rectangle rect)
{
    int red = 0;
    for (int y = (int)rect.y; y < (int)(rect.y + rect.height); ++y) {
        for (int x = (int)rect.x; x < (int)(rect.x + rect.width); ++x) {
            if (x < 0 || y < 0 || x >= img->width || y >= img->height) {
                continue;
            }
            Color c = GetImageColor(*img, x, y);
            /* "Red" means red DOMINATES, not that it is bright: under ambient
             * alone the ball is a dark (120,0,0), and the background is a
             * near-black grey with r == g == b. */
            if (c.r > 60 && c.r > c.g * 2 && c.r > c.b * 2) {
                ++red;
            }
        }
    }
    return red;
}

TEST(a_second_camera_draws_what_only_it_can_see_into_its_viewport)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs camera render test",
        .max_frames = 3 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    /* The red ball, far off to +X where the main camera is not looking. */
    mye_model ball = mye_model_from_mesh(world, "test:ball",
                                         GenMeshSphere(1.0f, 16, 16), RED);
    mye_mesh_spawn(world, ball, (Vector3){ 100.0f, 0.0f, 0.0f }, RED);

    /* Main camera: at the origin, looking down -Z. Sees nothing. */
    mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 5.0f },
                       (Vector3){ 0.0f, 0.0f, -10.0f }, 60.0f);

    /* Second camera: in front of the ball, drawn into the top-right corner. */
    Rectangle corner = { W - 160.0f, 0.0f, 160.0f, 120.0f };
    ecs_entity_t second = mye_camera3d_spawn(world, (Vector3){ 100.0f, 0.0f, 4.0f },
                                             (Vector3){ 100.0f, 0.0f, 0.0f },
                                             60.0f);
    MyeCamera3D *c = ecs_get_mut(world, second, MyeCamera3D);
    c->order = 1;
    c->viewport = corner;
    ecs_modified(world, second, MyeCamera3D);

    /* Ambient only, so the ball is red from any angle. */
    MyeRender3dConfig *cfg = ecs_singleton_ensure(world, MyeRender3dConfig);
    cfg->ambient = (Color){ 255, 255, 255, 255 };
    cfg->draw_grid = false;
    ecs_singleton_modified(world, MyeRender3dConfig);

    /* Pixels are read from INSIDE the frame, before the buffer swap: the
     * engine's screenshot hook does exactly that. Reading after
     * mye_progress returns gets the swapped-out back buffer -- the previous
     * frame on most drivers -- which is how a working renderer looked
     * broken for an afternoon. */
    char path[256];
    snprintf(path, sizeof path, "%s/mye_cameras_%d.png",
             getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp",
             (int)getpid());
    mye_engine *engine = mye_engine_get(world);
    engine->screenshot_path = path;

    while (mye_running(world)) {
        mye_progress(world, 1.0f / 60.0f);
    }

    Image shot = LoadImage(path);
    remove(path);
    ASSERT_TRUE(shot.data != NULL);

    int red_in_corner = count_red(&shot, corner);
    Rectangle rest = { 0.0f, 0.0f, W - 160.0f, (float)H };
    int red_elsewhere = count_red(&shot, rest);
    UnloadImage(shot);

    /* The whole point: the second camera drew, into its rect, and nowhere
     * else. */
    ASSERT_TRUE(red_in_corner > 50);
    ASSERT_EQ_INT(0, red_elsewhere);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(a_second_camera_draws_what_only_it_can_see_into_its_viewport))
