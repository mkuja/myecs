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
#include "render/canvas.h"
#include "render/render2d.h"
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

/* Brightest pixel in a rect. Used to tell "this surface is lit" from "this
 * surface has been multiplied into the floor". */
static int brightest(const Image *img, Rectangle rect)
{
    int best = 0;
    for (int y = (int)rect.y; y < (int)(rect.y + rect.height); ++y) {
        for (int x = (int)rect.x; x < (int)(rect.x + rect.width); ++x) {
            if (x < 0 || y < 0 || x >= img->width || y >= img->height) {
                continue;
            }
            Color c = GetImageColor(*img, x, y);
            int v = (c.r + c.g + c.b) / 3;
            if (v > best) {
                best = v;
            }
        }
    }
    return best;
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


/* C2, through the 3D pass and a real framebuffer. Two cameras in the same
 * place, looking at the same ball, differing in nothing but their layer mask:
 * the one that shares the ball's layer draws it and the one that does not,
 * does not. Drop the test and the ball floods the whole window; invert it and
 * the corner is empty. */
TEST(a_masked_camera_draws_only_the_meshes_that_share_its_layer)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs camera layers test",
        .max_frames = 3 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    mye_model ball = mye_model_from_mesh(world, "test:ball",
                                         GenMeshSphere(1.0f, 16, 16), RED);
    ecs_entity_t blip = mye_mesh_spawn(world, ball, (Vector3){ 0.0f, 0.0f, 0.0f },
                                       RED);
    /* On layer 1 (bit 1) -- the minimap-shows-blips case. */
    ecs_set(world, blip, MyeVisibilityLayers, { .mask = 2 });

    /* The main view watches layer 0 only, and so must not draw it... */
    ecs_entity_t main_view = mye_camera3d_spawn(world,
                                                (Vector3){ 0.0f, 0.0f, 5.0f },
                                                (Vector3){ 0.0f, 0.0f, 0.0f },
                                                60.0f);
    MyeCamera3D *m = ecs_get_mut(world, main_view, MyeCamera3D);
    m->layers = 1;
    ecs_modified(world, main_view, MyeCamera3D);

    /* ...while the corner camera, from the same place, watches layer 1. */
    Rectangle corner = { W - 160.0f, 0.0f, 160.0f, 120.0f };
    ecs_entity_t minimap = mye_camera3d_spawn(world,
                                              (Vector3){ 0.0f, 0.0f, 5.0f },
                                              (Vector3){ 0.0f, 0.0f, 0.0f },
                                              60.0f);
    MyeCamera3D *c = ecs_get_mut(world, minimap, MyeCamera3D);
    c->order = 1;
    c->viewport = corner;
    c->layers = 2;
    ecs_modified(world, minimap, MyeCamera3D);

    MyeRender3dConfig *cfg = ecs_singleton_ensure(world, MyeRender3dConfig);
    cfg->ambient = (Color){ 255, 255, 255, 255 };
    cfg->draw_grid = false;
    ecs_singleton_modified(world, MyeRender3dConfig);

    char path[256];
    snprintf(path, sizeof path, "%s/mye_layers3d_%d.png",
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

    ASSERT_TRUE(red_in_corner > 50);
    ASSERT_EQ_INT(0, red_elsewhere);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The same rule in the sprite pass, which applies it to a draw list built
 * once and drawn by every camera -- a different piece of code, and the one
 * where a mask stored per item can drift from the entity it came from. */
TEST(a_masked_2d_camera_draws_only_the_sprites_that_share_its_layer)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs sprite layers test",
        .max_frames = 3 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    /* from_image takes the Image; do not unload it here. */
    mye_texture red_tex = mye_texture_from_image(world, "test:red",
                                                 GenImageColor(48, 48, RED));
    ecs_entity_t blip = mye_entity_new(world);
    ecs_set(world, blip, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, blip, MyeSprite,
            { .texture = red_tex, .origin = { 24.0f, 24.0f }, .tint = WHITE });
    ecs_set(world, blip, MyeVisibilityLayers, { .mask = 2 });

    /* Split screen. Each camera centres its own half on the world origin, so
     * the sprite would land in the middle of both halves -- only the mask
     * decides which one actually shows it. */
    ecs_entity_t left = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    MyeCamera2D *l = ecs_get_mut(world, left, MyeCamera2D);
    l->viewport = (Rectangle){ 0.0f, 0.0f, W / 2.0f, (float)H };
    l->offset = (Vector2){ W / 4.0f, H / 2.0f };
    l->layers = 1;
    ecs_modified(world, left, MyeCamera2D);

    ecs_entity_t right = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f },
                                            1.0f);
    MyeCamera2D *r = ecs_get_mut(world, right, MyeCamera2D);
    r->viewport = (Rectangle){ W / 2.0f, 0.0f, W / 2.0f, (float)H };
    r->offset = (Vector2){ W / 4.0f, H / 2.0f };
    r->layers = 2;
    ecs_modified(world, right, MyeCamera2D);

    char path[256];
    snprintf(path, sizeof path, "%s/mye_layers2d_%d.png",
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

    Rectangle left_half = { 0.0f, 0.0f, W / 2.0f, (float)H };
    Rectangle right_half = { W / 2.0f, 0.0f, W / 2.0f, (float)H };
    int red_left = count_red(&shot, left_half);
    int red_right = count_red(&shot, right_half);
    UnloadImage(shot);

    /* The whole 48x48 sprite in the half that shares its layer... */
    ASSERT_TRUE(red_right > 2000);
    /* ...and not one pixel of it in the half that does not. */
    ASSERT_EQ_INT(0, red_left);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* C3: the clipping planes are per camera, not per process. Two cameras in the
 * same place see the same two balls -- one close, one far -- and only the
 * right-hand one has planes that exclude both. Each ball has its own third of
 * the picture, so a near plane that was ignored and a far plane that was
 * ignored fail different assertions. */
TEST(per_camera_near_and_far_planes_clip_only_that_camera_s_view)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs camera planes test",
        .max_frames = 3 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    /* Camera at z = 10 looking down -Z, fov 60, in a 320x360 viewport: the
     * half-width at distance d is 0.5132*d. */
    mye_model small = mye_model_from_mesh(world, "test:small",
                                          GenMeshSphere(0.5f, 16, 16), RED);
    mye_model large = mye_model_from_mesh(world, "test:large",
                                          GenMeshSphere(1.5f, 16, 16), RED);
    /* Four units away, on the left of the view. */
    mye_mesh_spawn(world, small, (Vector3){ -1.5f, 0.0f, 6.0f }, RED);
    /* Twenty-two away, on the right of it. */
    mye_mesh_spawn(world, large, (Vector3){ 5.0f, 0.0f, -12.0f }, RED);

    /* Left half: raylib's defaults, so both balls are inside the frustum. */
    ecs_entity_t open = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 10.0f },
                                           (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    MyeCamera3D *o = ecs_get_mut(world, open, MyeCamera3D);
    o->viewport = (Rectangle){ 0.0f, 0.0f, W / 2.0f, (float)H };
    ecs_modified(world, open, MyeCamera3D);

    /* Right half: a slab from 5 to 15 units, which contains neither ball. */
    ecs_entity_t slab = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 10.0f },
                                           (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    MyeCamera3D *s = ecs_get_mut(world, slab, MyeCamera3D);
    s->viewport = (Rectangle){ W / 2.0f, 0.0f, W / 2.0f, (float)H };
    s->order = 1;
    s->near_plane = 5.0f;
    s->far_plane = 15.0f;
    ecs_modified(world, slab, MyeCamera3D);

    MyeRender3dConfig *cfg = ecs_singleton_ensure(world, MyeRender3dConfig);
    cfg->ambient = (Color){ 255, 255, 255, 255 };
    cfg->draw_grid = false;
    ecs_singleton_modified(world, MyeRender3dConfig);

    char path[256];
    snprintf(path, sizeof path, "%s/mye_planes_%d.png",
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

    /* Where each ball lands, in each half. */
    Rectangle open_near = { 0.0f, 100.0f, 106.0f, 160.0f };
    Rectangle open_far = { 200.0f, 100.0f, 120.0f, 160.0f };
    Rectangle slab_near = { 320.0f, 100.0f, 106.0f, 160.0f };
    Rectangle slab_far = { 520.0f, 100.0f, 120.0f, 160.0f };
    int open_near_red = count_red(&shot, open_near);
    int open_far_red = count_red(&shot, open_far);
    int slab_near_red = count_red(&shot, slab_near);
    int slab_far_red = count_red(&shot, slab_far);
    UnloadImage(shot);

    /* The camera that set no planes sees both -- so the geometry is where the
     * arithmetic above says, and the two assertions below mean something. */
    ASSERT_TRUE(open_near_red > 50);
    ASSERT_TRUE(open_far_red > 50);
    /* Its twin, differing only in near_plane and far_plane, sees neither: the
     * close ball is in front of 5, the distant one beyond 15. */
    ASSERT_EQ_INT(0, slab_near_red);
    ASSERT_EQ_INT(0, slab_far_red);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A HUD drawn in MyeOnDrawUI after a corner camera. rlViewport and the
 * scissor are global state: leave them set to the last camera's rect and the
 * HUD is squeezed into that corner and clipped to it -- which reads as a
 * layout bug, in a game whose layout is fine. */
static void DrawHudBar(ecs_iter_t *it)
{
    (void)ecs_field(it, MyeRenderConfig, 0);
    /* Full width, along the bottom: as far from the camera's corner as the
     * window allows. */
    DrawRectangle(0, H - 40, W, 40, RED);
}

TEST(the_viewport_and_scissor_are_restored_before_the_hud_draws)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs viewport restore test",
        /* One frame: nothing else gets a chance to put the viewport back. */
        .max_frames = 1 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    /* A camera confined to the top-right corner, with something to draw so
     * the pass really runs. */
    mye_model ball = mye_model_from_mesh(world, "test:ball",
                                         GenMeshSphere(1.0f, 12, 12), WHITE);
    mye_mesh_spawn(world, ball, (Vector3){ 0.0f, 0.0f, 0.0f }, WHITE);
    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 5.0f },
                                          (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    MyeCamera3D *c = ecs_get_mut(world, cam, MyeCamera3D);
    c->viewport = (Rectangle){ W - 160.0f, 0.0f, 160.0f, 120.0f };
    ecs_modified(world, cam, MyeCamera3D);

    ECS_SYSTEM(world, DrawHudBar, MyeOnDrawUI, MyeRenderConfig);

    char path[256];
    snprintf(path, sizeof path, "%s/mye_restore_%d.png",
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

    /* The bar, at both ends of the window and nowhere near the camera's
     * rect. Clipped to that rect, either count is zero. */
    Rectangle bar_left = { 0.0f, H - 40.0f, 200.0f, 40.0f };
    Rectangle bar_right = { W - 200.0f, H - 40.0f, 200.0f, 40.0f };
    int red_left = count_red(&shot, bar_left);
    int red_right = count_red(&shot, bar_right);
    UnloadImage(shot);

    ASSERT_TRUE(red_left > 5000);  /* 200x40 = 8000 px of bar */
    ASSERT_TRUE(red_right > 5000);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The screenshot is of the WINDOW. Canvases draw first, each into its own
 * framebuffer, and the frame's pixels are read at the end of the window's
 * passes -- so a canvas left bound, or a read taken from the wrong
 * framebuffer, would hand back a small square of the canvas instead of the
 * picture the player saw. Both halves are asserted: the size, and that the
 * canvas's red is only where the window put it. */
TEST(the_screenshot_is_of_the_window_not_the_last_canvas)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs screenshot identity test",
        .max_frames = 1 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    /* An all-red canvas: its clear colour is enough, no camera needed. */
    ecs_entity_t canvas = mye_canvas_create(world, "test:red", 128, 128, RED);
    ASSERT_TRUE(canvas != 0);

    /* Shown in the middle of the window, so red is present but confined. */
    mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    Rectangle shown = { (float)W / 2.0f - 64.0f, (float)H / 2.0f - 64.0f,
                        128.0f, 128.0f };
    ecs_entity_t display = mye_entity_new(world);
    ecs_set(world, display, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, display, MyeSprite,
            { .texture = mye_canvas_texture(world, canvas),
              .source = mye_canvas_source_rect(world, canvas),
              .origin = { 64.0f, 64.0f },
              .tint = WHITE });

    char path[256];
    snprintf(path, sizeof path, "%s/mye_shot_identity_%d.png",
             getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp",
             (int)getpid());
    mye_engine *engine = mye_engine_get(world);
    engine->screenshot_path = path;

    while (mye_running(world)) {
        mye_progress(world, 1.0f / 60.0f);
    }

    /* Read while the window is still up: this is what the shot must match. */
    int render_width = GetRenderWidth();
    int render_height = GetRenderHeight();

    Image shot = LoadImage(path);
    remove(path);
    ASSERT_TRUE(shot.data != NULL);

    /* The window's size, not the canvas's 128x128. The live render size is
     * asked for rather than assumed from the config, because HiDPI makes them
     * differ -- but raylib reports the CURRENT framebuffer's size there, so
     * with a canvas left bound it would agree with a shot of that canvas.
     * Hence the absolute floor as well: it is what catches that. */
    ASSERT_EQ_INT(render_width, shot.width);
    ASSERT_EQ_INT(render_height, shot.height);
    ASSERT_TRUE(shot.width > 128 && shot.height > 128);

    /* And the canvas's pixels are where the window drew them -- not filling
     * the frame, which is what a shot OF the canvas would be. */
    int red_shown = count_red(&shot, shown);
    int red_total = count_red(&shot, (Rectangle){ 0.0f, 0.0f,
                                                  (float)shot.width,
                                                  (float)shot.height });
    UnloadImage(shot);

    ASSERT_TRUE(red_shown > 10000); /* most of a 128x128 square */
    ASSERT_EQ_INT(red_shown, red_total);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A canvas renders what only its camera can see, and the window shows the
 * canvas. This is the whole feature in one picture: red must appear inside
 * the sprite that displays the canvas, and nowhere else.
 *
 * Mutations this catches, all of which produce a plausible-looking picture:
 *  - canvases rendered AFTER the window: the sprite shows the previous
 *    frame, so with a short run the rect is empty;
 *  - EndTextureMode dropped: the window's passes draw into the canvas and
 *    the window keeps the clear colour;
 *  - the vertical flip dropped: red lands in the wrong half of the rect. */
TEST(a_canvas_shows_its_own_camera_and_the_window_shows_the_canvas)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs canvas render test",
        /* ONE frame. The canvas must be drawn and displayed within the same
         * frame; with two, a canvas rendered too late still shows last
         * frame's content and the ordering bug hides. */
        .max_frames = 1 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    /* The ball sits high in the canvas camera's view, so the flip is
     * detectable: it must land in the TOP half of the displayed rect. */
    mye_model ball = mye_model_from_mesh(world, "test:ball",
                                         GenMeshSphere(1.0f, 16, 16), RED);
    mye_mesh_spawn(world, ball, (Vector3){ 100.0f, 1.2f, 0.0f }, RED);

    ecs_entity_t canvas = mye_canvas_create(world, "test:canvas", 128, 128,
                                            BLACK);
    ASSERT_TRUE(canvas != 0);

    /* Only this camera can see the ball, and it renders into the canvas. */
    ecs_entity_t canvas_cam = mye_camera3d_spawn(
        world, (Vector3){ 100.0f, 0.0f, 6.0f }, (Vector3){ 100.0f, 0.0f, 0.0f },
        60.0f);
    MyeCamera3D *cc = ecs_get_mut(world, canvas_cam, MyeCamera3D);
    cc->target = canvas;
    ecs_modified(world, canvas_cam, MyeCamera3D);

    /* The window's camera looks at empty space... */
    mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);

    /* ...and a sprite at the world origin displays the canvas. The camera
     * is at the origin too, so it lands dead centre: no arithmetic of mine
     * between the feature and the assertion. */
    Rectangle shown = { (float)W / 2.0f - 64.0f, (float)H / 2.0f - 64.0f,
                        128.0f, 128.0f };
    ecs_entity_t display = mye_entity_new(world);
    ecs_set(world, display, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, display, MyeSprite,
            { .texture = mye_canvas_texture(world, canvas),
              .source = mye_canvas_source_rect(world, canvas),
              .origin = { 64.0f, 64.0f },
              .tint = WHITE });

    MyeRender3dConfig *cfg = ecs_singleton_ensure(world, MyeRender3dConfig);
    cfg->ambient = (Color){ 255, 255, 255, 255 };
    cfg->draw_grid = false;
    ecs_singleton_modified(world, MyeRender3dConfig);

    char path[256];
    snprintf(path, sizeof path, "%s/mye_canvas_%d.png",
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

    int red_in_display = count_red(&shot, shown);
    /* Everything left of the displayed sprite must be untouched. */
    Rectangle elsewhere = { 0.0f, 0.0f, shown.x - 4.0f, (float)H };
    int red_elsewhere = count_red(&shot, elsewhere);

    /* Top half of the displayed rect: the ball is above the canvas camera's
     * centre, so a dropped flip puts it in the bottom half instead. */
    Rectangle top_half = { shown.x, shown.y, shown.width, shown.height / 2.0f };
    Rectangle bottom_half = { shown.x, shown.y + shown.height / 2.0f,
                              shown.width, shown.height / 2.0f };
    int red_top = count_red(&shot, top_half);
    int red_bottom = count_red(&shot, bottom_half);
    UnloadImage(shot);

    ASSERT_TRUE(red_in_display > 50);
    ASSERT_EQ_INT(0, red_elsewhere);
    ASSERT_TRUE(red_top > red_bottom);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The other way to display a canvas: as the texture of a mesh, so the screen
 * is IN the world. Same canvas machinery, different consumer -- and the one
 * that would break if the per-instance override were written into the shared
 * model material or ignored entirely. */
TEST(a_canvas_can_be_displayed_on_a_mesh)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs canvas-on-mesh test",
        .max_frames = 1 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    /* A red ball only the canvas camera can see, filling most of its view. */
    mye_model ball = mye_model_from_mesh(world, "test:ball",
                                         GenMeshSphere(1.0f, 16, 16), RED);
    mye_mesh_spawn(world, ball, (Vector3){ 100.0f, 0.0f, 0.0f }, RED);

    ecs_entity_t canvas = mye_canvas_create(world, "test:canvas", 128, 128,
                                            BLACK);
    ecs_entity_t canvas_cam = mye_camera3d_spawn(
        world, (Vector3){ 100.0f, 0.0f, 2.6f }, (Vector3){ 100.0f, 0.0f, 0.0f },
        60.0f);
    MyeCamera3D *cc = ecs_get_mut(world, canvas_cam, MyeCamera3D);
    cc->target = canvas;
    ecs_modified(world, canvas_cam, MyeCamera3D);

    /* Two identical cubes from the SAME model, side by side. Only the left
     * one gets the canvas. If the override leaked into the shared material,
     * the right one would show red too -- which is the point of testing two. */
    mye_model panel = mye_model_from_mesh(world, "test:panel",
                                          GenMeshCube(3.0f, 3.0f, 0.2f), WHITE);
    ecs_entity_t screen = mye_mesh_spawn(world, panel,
                                         (Vector3){ -2.0f, 0.0f, 0.0f }, WHITE);
    mye_mesh_spawn(world, panel, (Vector3){ 2.0f, 0.0f, 0.0f }, WHITE);
    MyeMeshInstance *m = ecs_get_mut(world, screen, MyeMeshInstance);
    m->texture = mye_canvas_texture(world, canvas);
    ecs_modified(world, screen, MyeMeshInstance);

    /* The window camera sees both panels and not the ball. */
    mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 6.0f },
                       (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);

    MyeRender3dConfig *cfg = ecs_singleton_ensure(world, MyeRender3dConfig);
    cfg->ambient = (Color){ 255, 255, 255, 255 };
    cfg->draw_grid = false;
    ecs_singleton_modified(world, MyeRender3dConfig);

    char path[256];
    snprintf(path, sizeof path, "%s/mye_mesh_canvas_%d.png",
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

    Rectangle left = { 0.0f, 0.0f, (float)W / 2.0f, (float)H };
    Rectangle right = { (float)W / 2.0f, 0.0f, (float)W / 2.0f, (float)H };
    int red_left = count_red(&shot, left);
    int red_right = count_red(&shot, right);
    UnloadImage(shot);

    /* The textured panel shows the canvas... */
    ASSERT_TRUE(red_left > 50);
    /* ...and the untextured one, from the same model, does not. */
    ASSERT_EQ_INT(0, red_right);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* MyeMeshInstance.tint is per-instance, and must stay that way over time.
 * raylib's Material aliases the model's map array, so tinting by writing into
 * it both compounds frame over frame and bleeds onto every other entity using
 * the same model. Two panels from one model, one tinted, thirty frames: the
 * untinted one must still be bright and the tinted one must not have decayed
 * toward black. */
TEST(a_tint_stays_per_instance_and_does_not_compound_over_frames)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs tint test",
        .max_frames = 30 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    mye_model panel = mye_model_from_mesh(world, "test:panel",
                                          GenMeshCube(3.0f, 3.0f, 0.2f), WHITE);
    /* Left: tinted grey. Right: untinted, same model. */
    mye_mesh_spawn(world, panel, (Vector3){ -2.0f, 0.0f, 0.0f },
                   (Color){ 128, 128, 128, 255 });
    mye_mesh_spawn(world, panel, (Vector3){ 2.0f, 0.0f, 0.0f }, WHITE);

    mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 6.0f },
                       (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);

    MyeRender3dConfig *cfg = ecs_singleton_ensure(world, MyeRender3dConfig);
    cfg->ambient = (Color){ 255, 255, 255, 255 };
    cfg->draw_grid = false;
    ecs_singleton_modified(world, MyeRender3dConfig);

    char path[256];
    snprintf(path, sizeof path, "%s/mye_tint_%d.png",
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

    Rectangle left = { 0.0f, 0.0f, (float)W / 2.0f, (float)H };
    Rectangle right = { (float)W / 2.0f, 0.0f, (float)W / 2.0f, (float)H };
    int bright_left = brightest(&shot, left);
    int bright_right = brightest(&shot, right);
    UnloadImage(shot);

    /* The untinted panel is still lit after thirty frames -- it would be
     * black if its neighbour's tint had been written into the shared
     * material. */
    ASSERT_TRUE(bright_right > 150);
    /* The tinted one is dimmer, but has not decayed to nothing. */
    ASSERT_TRUE(bright_left > 40);
    ASSERT_TRUE(bright_left < bright_right);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A canvas that displays ITSELF. Sampling a texture while it is the bound
 * framebuffer is a GL feedback loop: the contents are undefined by
 * specification, and that is fine -- but it is a mistake a game can make by
 * accident, so it must not crash, corrupt the rest of the frame, or leak.
 * Nothing is asserted about the pixels, because nothing may be. */
TEST(a_canvas_that_displays_itself_does_not_take_the_program_with_it)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs canvas self-reference test",
        .max_frames = 10 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    ecs_entity_t canvas = mye_canvas_create(world, "test:loop", 64, 64, BLUE);
    ASSERT_TRUE(canvas != 0);

    ecs_entity_t cam = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    MyeCamera2D *c = ecs_get_mut(world, cam, MyeCamera2D);
    c->target = canvas;
    ecs_modified(world, cam, MyeCamera2D);

    ecs_entity_t s = mye_entity_new(world);
    ecs_set(world, s, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, s, MyeSprite,
            { .texture = mye_canvas_texture(world, canvas),
              .source = mye_canvas_source_rect(world, canvas),
              .tint = WHITE });

    /* The rule that makes it defined rather than undefined: while rendering
     * INTO the canvas, a surface textured with that canvas is skipped. */
    mye_texture own = mye_canvas_texture(world, canvas);
    ASSERT_TRUE(mye_canvas_is_own_texture(world, canvas, own));
    /* ...but only for that canvas, and never for the window. */
    ASSERT_TRUE(!mye_canvas_is_own_texture(world, 0, own));
    ecs_entity_t other = mye_canvas_create(world, "test:other", 32, 32, RED);
    ASSERT_TRUE(!mye_canvas_is_own_texture(world, other, own));
    ASSERT_TRUE(!mye_canvas_is_own_texture(world, canvas,
                                           mye_canvas_texture(world, other)));

    int frames = 0;
    while (mye_running(world)) {
        mye_progress(world, 1.0f / 60.0f);
        ++frames;
    }
    ASSERT_EQ_INT(10, frames);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Destroying a canvas that is still in use, mid-run, with the passes
 * running. A camera keeps a dead entity in its `target` (the engine does not
 * rewrite the game's components), and flecs aborts on ecs_has for a dead
 * entity in debug and dereferences NULL in release -- so the documented
 * "cameras fall back to the window" path used to take the process with it on
 * the very next frame. */
TEST(destroying_a_canvas_in_use_does_not_crash_the_passes)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs canvas destroy test",
        .max_frames = 8 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    mye_model ball = mye_model_from_mesh(world, "test:ball",
                                         GenMeshSphere(1.0f, 12, 12), RED);
    mye_mesh_spawn(world, ball, (Vector3){ 0.0f, 0.0f, 0.0f }, RED);

    ecs_entity_t canvas = mye_canvas_create(world, "test:doomed", 96, 96,
                                            BLACK);
    ASSERT_TRUE(canvas != 0);

    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 5.0f },
                                          (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    MyeCamera3D *cc = ecs_get_mut(world, cam, MyeCamera3D);
    cc->target = canvas;
    ecs_modified(world, cam, MyeCamera3D);

    /* A sprite displaying it, and a window camera, so both passes touch the
     * dead target afterwards. */
    mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    ecs_entity_t display = mye_entity_new(world);
    ecs_set(world, display, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, display, MyeSprite,
            { .texture = mye_canvas_texture(world, canvas),
              .source = mye_canvas_source_rect(world, canvas),
              .tint = WHITE });

    int frames = 0;
    while (mye_running(world)) {
        mye_progress(world, 1.0f / 60.0f);
        if (++frames == 3) {
            mye_canvas_destroy(world, canvas);
            /* The camera still points at it, and the sprite still holds its
             * handle. Both are things a game does. */
        }
    }
    ASSERT_EQ_INT(8, frames);

    /* Reached from the other direction too: these are documented to return
     * empty rather than abort. */
    ASSERT_TRUE(mye_canvas_texture(world, canvas).generation == 0);
    ASSERT_TRUE(mye_canvas_source_rect(world, canvas).width == 0.0f);
    (void)mye_camera_viewport(world, cam);
    (void)mye_camera_at_screen(world, (Vector2){ 10.0f, 10.0f });

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A canvas with clear = false accumulates: this frame is drawn OVER what is
 * already there. The trap is that the first 3D camera used to clear colour
 * and depth together, so the documented "trails and paint effects" quietly
 * did nothing on any canvas with a 3D camera. Depth must still be reset, or
 * last frame's depth values reject this frame's meshes. */
TEST(an_accumulating_canvas_keeps_what_earlier_frames_drew)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs canvas accumulate test",
        .max_frames = 8 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    ecs_entity_t canvas = mye_canvas_create(world, "test:accumulate", 128, 128,
                                            BLACK);
    ASSERT_TRUE(canvas != 0);
    MyeCanvas *cv = ecs_get_mut(world, canvas, MyeCanvas);
    cv->clear = false;
    ecs_modified(world, canvas, MyeCanvas);

    mye_model ball = mye_model_from_mesh(world, "test:ball",
                                         GenMeshSphere(1.0f, 16, 16), RED);
    ecs_entity_t red = mye_mesh_spawn(world, ball,
                                      (Vector3){ 100.0f, 0.0f, 0.0f }, RED);

    ecs_entity_t canvas_cam = mye_camera3d_spawn(
        world, (Vector3){ 100.0f, 0.0f, 2.6f }, (Vector3){ 100.0f, 0.0f, 0.0f },
        60.0f);
    MyeCamera3D *cc = ecs_get_mut(world, canvas_cam, MyeCamera3D);
    cc->target = canvas;
    ecs_modified(world, canvas_cam, MyeCamera3D);

    mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    Rectangle shown = { (float)W / 2.0f - 64.0f, (float)H / 2.0f - 64.0f,
                        128.0f, 128.0f };
    ecs_entity_t display = mye_entity_new(world);
    ecs_set(world, display, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, display, MyeSprite,
            { .texture = mye_canvas_texture(world, canvas),
              .source = mye_canvas_source_rect(world, canvas),
              .origin = { 64.0f, 64.0f },
              .tint = WHITE });

    MyeRender3dConfig *cfg = ecs_singleton_ensure(world, MyeRender3dConfig);
    cfg->ambient = (Color){ 255, 255, 255, 255 };
    cfg->draw_grid = false;
    ecs_singleton_modified(world, MyeRender3dConfig);

    char path[256];
    snprintf(path, sizeof path, "%s/mye_accum_%d.png",
             getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp",
             (int)getpid());
    mye_engine *engine = mye_engine_get(world);
    engine->screenshot_path = path;

    int frames = 0;
    while (mye_running(world)) {
        mye_progress(world, 1.0f / 60.0f);
        /* Once the ball has been painted into the canvas, take it away. What
         * it drew must still be there at the end of the run. */
        if (++frames == 3) {
            ecs_delete(world, red);
        }
    }

    Image shot = LoadImage(path);
    remove(path);
    ASSERT_TRUE(shot.data != NULL);
    int red_left = count_red(&shot, shown);
    UnloadImage(shot);

    /* Five frames after the ball was deleted, its paint is still on the
     * canvas. With colour cleared alongside depth, this is 0. */
    ASSERT_TRUE(red_left > 50);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Two canvases cannot share a name. Sharing the registry slot would hand the
 * second canvas the first one's pixels -- a plausible-looking wrong picture --
 * and tangle two GPU lifetimes behind one refcount. */
TEST(a_canvas_will_not_take_a_name_that_is_already_a_texture)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs canvas name test",
        .max_frames = 1 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    ecs_entity_t first = mye_canvas_create(world, "test:taken", 64, 64, BLACK);
    ASSERT_TRUE(first != 0);
    mye_texture a = mye_canvas_texture(world, first);

    ecs_entity_t second = mye_canvas_create(world, "test:taken", 64, 64, BLACK);
    ASSERT_EQ_INT(0, (int)second);

    /* And the first canvas is untouched: no refcount was borrowed from it. */
    mye_texture b = mye_canvas_texture(world, first);
    ASSERT_TRUE(a.index == b.index && a.generation == b.generation);
    ASSERT_TRUE(mye_texture_valid(world, a));

    while (mye_running(world)) {
        mye_progress(world, 1.0f / 60.0f);
    }
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Resizing the window must not displace what the cameras draw.
 *
 * The viewport has to be flipped against the CURRENT surface height, and the
 * obvious source for that -- rlgl's framebuffer height -- is written by
 * raylib only in BeginTextureMode: not on resize, not in EndTextureMode. A
 * camera flipped against a stale height slides everything the game draws by
 * the difference, in a game that need not contain a single canvas. */
TEST(resizing_the_window_does_not_displace_what_cameras_draw)
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN | FLAG_WINDOW_RESIZABLE);
    ecs_world_t *world = mye_init(&(mye_config){
        .width = W, .height = H, .title = "myecs resize test",
        .max_frames = 6 });
    if (world == NULL) {
        SKIP("no window could be created");
    }

    /* A 2D camera at the origin: its offset is the window centre AT SPAWN,
     * so the sprite stays at (W/2, H/2) whatever the window does later. */
    mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);

    /* from_image takes the Image; do not unload it here. */
    mye_texture red_tex = mye_texture_from_image(world, "test:red",
                                                 GenImageColor(64, 64, RED));

    ecs_entity_t sprite = mye_entity_new(world);
    ecs_set(world, sprite, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, sprite, MyeSprite,
            { .texture = red_tex, .origin = { 32.0f, 32.0f }, .tint = WHITE });

    char path[256];
    snprintf(path, sizeof path, "%s/mye_resize_%d.png",
             getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp",
             (int)getpid());
    mye_engine *engine = mye_engine_get(world);
    engine->screenshot_path = path;

    int frames = 0;
    while (mye_running(world)) {
        mye_progress(world, 1.0f / 60.0f);
        if (++frames == 2) {
            SetWindowSize(W, H + 140);
        }
    }

    Image shot = LoadImage(path);
    remove(path);
    ASSERT_TRUE(shot.data != NULL);

    /* Still centred on the camera's offset, which the resize did not move. */
    Rectangle where = { (float)W / 2.0f - 32.0f, (float)H / 2.0f - 32.0f,
                        64.0f, 64.0f };
    int red_here = count_red(&shot, where);
    /* And nowhere below it: a stale flip slides the picture down by the
     * height difference. */
    Rectangle below = { 0.0f, (float)H / 2.0f + 40.0f, (float)shot.width,
                        (float)shot.height - ((float)H / 2.0f + 40.0f) };
    int red_below = count_red(&shot, below);
    UnloadImage(shot);

    ASSERT_TRUE(red_here > 1000);
    ASSERT_EQ_INT(0, red_below);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(a_second_camera_draws_what_only_it_can_see_into_its_viewport),
          TEST_CASE(a_masked_camera_draws_only_the_meshes_that_share_its_layer),
          TEST_CASE(a_masked_2d_camera_draws_only_the_sprites_that_share_its_layer),
          TEST_CASE(per_camera_near_and_far_planes_clip_only_that_camera_s_view),
          TEST_CASE(the_viewport_and_scissor_are_restored_before_the_hud_draws),
          TEST_CASE(the_screenshot_is_of_the_window_not_the_last_canvas),
          TEST_CASE(a_canvas_shows_its_own_camera_and_the_window_shows_the_canvas),
          TEST_CASE(a_canvas_can_be_displayed_on_a_mesh),
          TEST_CASE(a_tint_stays_per_instance_and_does_not_compound_over_frames),
          TEST_CASE(a_canvas_that_displays_itself_does_not_take_the_program_with_it),
          TEST_CASE(destroying_a_canvas_in_use_does_not_crash_the_passes),
          TEST_CASE(an_accumulating_canvas_keeps_what_earlier_frames_drew),
          TEST_CASE(a_canvas_will_not_take_a_name_that_is_already_a_texture),
          TEST_CASE(resizing_the_window_does_not_displace_what_cameras_draw))
