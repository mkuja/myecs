/* Canvases: the entity, its texture handle, and how cameras attach to it.
 *
 * Headless -- LoadRenderTexture needs a GL context, so a headless canvas has
 * no GPU object and a zero texture handle, and everything ELSE about it must
 * still work. What a canvas actually renders is checked with real pixels in
 * tests/integration/test_int_render_cameras.c. See plan/14-canvases.md. */
#include "mye_test.h"

#include "core/engine.h"
#include "core/log.h"
#include "render/camera.h"
#include "render/canvas.h"
#include "render/render2d.h"
#include "render/render3d.h"

#include <raylib.h>

#define FIXED_DT (1.0f / 60.0f)

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){ .headless = true,
                                   .width = 1280, .height = 720 });
}

TEST(a_canvas_is_an_entity_with_a_size_and_a_source_rect)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t canvas = mye_canvas_create(world, "test:canvas", 256, 128,
                                            BLACK);
    ASSERT_TRUE(canvas != 0);

    const MyeCanvas *c = ecs_get(world, canvas, MyeCanvas);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ_INT(256, c->width);
    ASSERT_EQ_INT(128, c->height);
    ASSERT_TRUE(c->active);

    /* Negative height: render textures are stored bottom-up, and drawing one
     * without this shows it mirrored. */
    Rectangle src = mye_canvas_source_rect(world, canvas);
    ASSERT_NEAR(0.0f, src.x, 0.001f);
    ASSERT_NEAR(256.0f, src.width, 0.001f);
    ASSERT_NEAR(-128.0f, src.height, 0.001f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_camera_targeting_a_canvas_is_collected_under_it_not_the_window)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t canvas = mye_canvas_create(world, "test:canvas", 256, 256,
                                            BLACK);
    ecs_entity_t window_cam = mye_camera3d_spawn(world, (Vector3){ 0, 0, 5 },
                                                 (Vector3){ 0, 0, 0 }, 60.0f);
    ecs_entity_t canvas_cam = mye_camera3d_spawn(world, (Vector3){ 0, 9, 0 },
                                                 (Vector3){ 0, 0, 0 }, 60.0f);
    MyeCamera3D *c = ecs_get_mut(world, canvas_cam, MyeCamera3D);
    c->target = canvas;
    ecs_modified(world, canvas_cam, MyeCamera3D);
    mye_progress(world, FIXED_DT);

    ecs_entity_t got[MYE_MAX_DRAWN_CAMERAS];

    ASSERT_EQ_INT(1, mye_camera3d_collect_for(world, canvas, got,
                                              MYE_MAX_DRAWN_CAMERAS));
    ASSERT_TRUE(got[0] == canvas_cam);

    ASSERT_EQ_INT(1, mye_camera3d_collect_for(world, 0, got,
                                              MYE_MAX_DRAWN_CAMERAS));
    ASSERT_TRUE(got[0] == window_cam);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A camera with no viewport of its own fills its target -- and its target is
 * the canvas, not the window. Getting this wrong scales the view by the
 * window/canvas ratio, which looks like a broken projection. */
TEST(a_canvas_cameras_default_viewport_is_the_canvas_not_the_window)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t canvas = mye_canvas_create(world, "test:canvas", 256, 128,
                                            BLACK);
    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0, 0, 5 },
                                          (Vector3){ 0, 0, 0 }, 60.0f);
    MyeCamera3D *c = ecs_get_mut(world, cam, MyeCamera3D);
    c->target = canvas;
    ecs_modified(world, cam, MyeCamera3D);
    mye_progress(world, FIXED_DT);

    Rectangle vp = mye_camera_viewport(world, cam);
    ASSERT_NEAR(256.0f, vp.width, 0.001f);
    ASSERT_NEAR(128.0f, vp.height, 0.001f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Picking must never resolve against a canvas camera, however low its order:
 * the answer would be in the canvas's space and silently wrong. */
TEST(the_main_view_for_picking_ignores_canvas_cameras)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t canvas = mye_canvas_create(world, "test:canvas", 256, 256,
                                            BLACK);
    ecs_entity_t window_cam = mye_camera3d_spawn(world, (Vector3){ 0, 0, 5 },
                                                 (Vector3){ 0, 0, 0 }, 60.0f);
    ecs_entity_t canvas_cam = mye_camera3d_spawn(world, (Vector3){ 0, 99, 0 },
                                                 (Vector3){ 0, 0, 0 }, 60.0f);
    MyeCamera3D *c = ecs_get_mut(world, canvas_cam, MyeCamera3D);
    c->target = canvas;
    c->order = -100; /* lowest by far, and still not the main view */
    ecs_modified(world, canvas_cam, MyeCamera3D);
    mye_progress(world, FIXED_DT);

    ecs_entity_t who = 0;
    Camera3D cam;
    ASSERT_TRUE(mye_camera3d_active(world, &cam, &who));
    ASSERT_TRUE(who == window_cam);
    ASSERT_NEAR(5.0f, cam.position.z, 0.001f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The adopted-texture guard: the registry hands out a handle to the canvas's
 * colour attachment but must never unload it -- UnloadRenderTexture does.
 * Unloading twice corrupts an unrelated GL texture much later, so this
 * asserts a clean shutdown under the sanitizers. */
TEST(destroying_a_canvas_frees_its_texture_exactly_once)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t canvas = mye_canvas_create(world, "test:canvas", 64, 64,
                                            BLACK);
    ASSERT_TRUE(canvas != 0);
    mye_texture handle = mye_canvas_texture(world, canvas);

    mye_canvas_destroy(world, canvas);
    mye_progress(world, FIXED_DT);

    ASSERT_TRUE(!ecs_is_alive(world, canvas));
    ASSERT_TRUE(!mye_texture_valid(world, handle));

    /* Another texture after the destroy must be unaffected. */
    Image image = GenImageColor(8, 8, RED);
    mye_texture after = mye_texture_from_image(world, "test:after", image);
    ASSERT_TRUE(mye_texture_valid(world, after));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A dangling target is malformed data, not a choice: fall back to the window
 * so the camera keeps working, and say so once rather than every frame. */
TEST(a_camera_whose_canvas_died_falls_back_to_the_window_and_warns_once)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t canvas = mye_canvas_create(world, "test:canvas", 64, 64,
                                            BLACK);
    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0, 0, 5 },
                                          (Vector3){ 0, 0, 0 }, 60.0f);
    MyeCamera3D *c = ecs_get_mut(world, cam, MyeCamera3D);
    c->target = canvas;
    ecs_modified(world, cam, MyeCamera3D);
    mye_progress(world, FIXED_DT);

    mye_canvas_destroy(world, canvas);
    mye_progress(world, FIXED_DT);

    mye_log_counts before = mye_log_get_counts();
    ecs_entity_t got[MYE_MAX_DRAWN_CAMERAS];
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ_INT(1, mye_camera3d_collect_for(world, 0, got,
                                                  MYE_MAX_DRAWN_CAMERAS));
        ASSERT_TRUE(got[0] == cam);
    }
    ASSERT_EQ_U64(before.warn + 1, mye_log_get_counts().warn);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(an_inactive_canvas_keeps_its_texture_and_its_camera)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t canvas = mye_canvas_create(world, "test:canvas", 64, 64,
                                            BLACK);
    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0, 0, 5 },
                                          (Vector3){ 0, 0, 0 }, 60.0f);
    MyeCamera3D *c = ecs_get_mut(world, cam, MyeCamera3D);
    c->target = canvas;
    ecs_modified(world, cam, MyeCamera3D);

    MyeCanvas *cv = ecs_get_mut(world, canvas, MyeCanvas);
    cv->active = false;
    ecs_modified(world, canvas, MyeCanvas);
    mye_progress(world, FIXED_DT);

    /* Not rendered, but still a canvas: its camera belongs to it, and its
     * texture still resolves to whatever it last held. */
    ecs_entity_t got[MYE_MAX_DRAWN_CAMERAS];
    ASSERT_EQ_INT(1, mye_camera3d_collect_for(world, canvas, got,
                                              MYE_MAX_DRAWN_CAMERAS));
    Camera3D resolved;
    ASSERT_TRUE(mye_camera3d_resolve(world, cam, &resolved));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(a_canvas_is_an_entity_with_a_size_and_a_source_rect),
          TEST_CASE(a_camera_targeting_a_canvas_is_collected_under_it_not_the_window),
          TEST_CASE(a_canvas_cameras_default_viewport_is_the_canvas_not_the_window),
          TEST_CASE(the_main_view_for_picking_ignores_canvas_cameras),
          TEST_CASE(destroying_a_canvas_frees_its_texture_exactly_once),
          TEST_CASE(a_camera_whose_canvas_died_falls_back_to_the_window_and_warns_once),
          TEST_CASE(an_inactive_canvas_keeps_its_texture_and_its_camera))
