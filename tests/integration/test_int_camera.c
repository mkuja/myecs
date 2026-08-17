/* Cameras: resolution, parenting, following, and screen/world conversion.
 *
 * Headless throughout. The screen helpers use raylib's *Ex forms, which take
 * the viewport size explicitly, so picking maths is testable without a
 * window. See engine/render/camera.h. */
#include "mye_test.h"

#include "core/engine.h"
#include "core/log.h"
#include "render/camera.h"
#include "render/render2d.h"
#include "render/render3d.h"
#include "scene/transform.h"

#include <raylib.h>
#include <raymath.h>

#define FIXED_DT (1.0f / 60.0f)

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){ .headless = true,
                                   .width = 1280, .height = 720 });
}

static void assert_near_v3(mye_test_ctx *T, Vector3 got, Vector3 want,
                           float eps, const char *what)
{
    (void)what;
    ASSERT_NEAR(want.x, got.x, eps);
    ASSERT_NEAR(want.y, got.y, eps);
    ASSERT_NEAR(want.z, got.z, eps);
}

static Vector3 forward_of(Camera3D c)
{
    return Vector3Normalize(Vector3Subtract(c.target, c.position));
}

/* --- a camera is placed by its transform, like everything else ----------- */

TEST(a_root_camera_resolves_to_its_own_position_and_fov)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 5.0f, 10.0f },
                                          (Vector3){ 0.0f, 0.0f, 0.0f }, 45.0f);
    mye_progress(world, FIXED_DT);

    Camera3D c;
    ASSERT_TRUE(mye_camera3d_resolve(world, cam, &c));
    assert_near_v3(T, c.position, (Vector3){ 0.0f, 5.0f, 10.0f }, 0.001f, "pos");
    ASSERT_NEAR(45.0f, c.fovy, 0.001f);

    /* Looking at the origin from (0,5,10): down and back along -Z. */
    Vector3 want = Vector3Normalize((Vector3){ 0.0f, -5.0f, -10.0f });
    assert_near_v3(T, forward_of(c), want, 0.001f, "forward");

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The reason orientation is a rotation rather than a stored target point: a
 * parented camera must be carried around by its parent's rotation. */
TEST(a_camera_child_of_a_rotating_parent_turns_with_it)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t pivot = mye_spawn_3d(world, (Vector3){ 0.0f, 0.0f, 0.0f });
    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 10.0f },
                                          (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    mye_set_parent(world, cam, pivot);
    mye_progress(world, FIXED_DT);

    Camera3D before;
    ASSERT_TRUE(mye_camera3d_resolve(world, cam, &before));
    assert_near_v3(T, before.position, (Vector3){ 0.0f, 0.0f, 10.0f }, 0.001f,
                   "pos");

    /* Yaw the pivot a quarter turn: the camera swings to +X and keeps
     * looking at the pivot. */
    ecs_set(world, pivot, MyeRotation3D,
            { QuaternionFromAxisAngle((Vector3){ 0.0f, 1.0f, 0.0f },
                                      PI / 2.0f) });
    mye_progress(world, FIXED_DT);

    Camera3D after;
    ASSERT_TRUE(mye_camera3d_resolve(world, cam, &after));
    assert_near_v3(T, after.position, (Vector3){ 10.0f, 0.0f, 0.0f }, 0.001f,
                   "rotated pos");
    assert_near_v3(T, forward_of(after), (Vector3){ -1.0f, 0.0f, 0.0f },
                   0.001f, "rotated forward");

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(look_at_points_the_camera_at_the_target)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 3.0f, 4.0f, 5.0f },
                                          (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    Vector3 target = { -2.0f, 1.0f, 7.0f };
    mye_camera_look_at(world, cam, target);
    mye_progress(world, FIXED_DT);

    Camera3D c;
    ASSERT_TRUE(mye_camera3d_resolve(world, cam, &c));
    Vector3 want = Vector3Normalize(Vector3Subtract(target, c.position));
    ASSERT_NEAR(1.0f, Vector3DotProduct(forward_of(c), want), 0.001f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* look_at takes a world point, but a child's rotation is in its parent's
 * space -- so this fails if the conversion is dropped. */
TEST(look_at_is_correct_for_a_parented_camera)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t pivot = mye_spawn_3d(world, (Vector3){ 100.0f, 0.0f, 0.0f });
    ecs_set(world, pivot, MyeRotation3D,
            { QuaternionFromAxisAngle((Vector3){ 0.0f, 1.0f, 0.0f },
                                      PI / 3.0f) });
    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 5.0f, 20.0f },
                                          (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    mye_set_parent(world, cam, pivot);
    mye_progress(world, FIXED_DT);

    Vector3 target = { 0.0f, 0.0f, 0.0f }; /* world origin, not the pivot */
    mye_camera_look_at(world, cam, target);
    mye_progress(world, FIXED_DT);

    Camera3D c;
    ASSERT_TRUE(mye_camera3d_resolve(world, cam, &c));
    Vector3 want = Vector3Normalize(Vector3Subtract(target, c.position));
    ASSERT_NEAR(1.0f, Vector3DotProduct(forward_of(c), want), 0.005f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(set_fov_changes_the_projection_and_nothing_else)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 10.0f },
                                          (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    mye_progress(world, FIXED_DT);
    Camera3D before;
    ASSERT_TRUE(mye_camera3d_resolve(world, cam, &before));

    mye_camera_set_fov(world, cam, 30.0f);
    ASSERT_NEAR(30.0f, mye_camera_get_fov(world, cam), 0.001f);

    Camera3D after;
    ASSERT_TRUE(mye_camera3d_resolve(world, cam, &after));
    ASSERT_NEAR(30.0f, after.fovy, 0.001f);
    assert_near_v3(T, after.position, before.position, 0.001f, "pos");
    assert_near_v3(T, forward_of(after), forward_of(before), 0.001f, "fwd");

    /* Clamped: a zero or 200-degree fov renders nothing or turns the
     * projection inside out, and both look like an engine fault. */
    mye_camera_set_fov(world, cam, 0.0f);
    ASSERT_TRUE(mye_camera_get_fov(world, cam) > 0.0f);
    mye_camera_set_fov(world, cam, 200.0f);
    ASSERT_TRUE(mye_camera_get_fov(world, cam) < 180.0f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --- following ----------------------------------------------------------- */

typedef struct Drift { float x, y; } Drift;
ECS_COMPONENT_DECLARE(Drift);

static void Move(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const Drift *vel = ecs_field(it, Drift, 1);
    for (int i = 0; i < it->count; ++i) {
        pos[i].x += vel[i].x * (float)it->delta_time;
        pos[i].y += vel[i].y * (float)it->delta_time;
    }
}

static ecs_world_t *make_moving_world(void)
{
    ecs_world_t *world = make_world();
    if (world == NULL) {
        return NULL;
    }
    ECS_COMPONENT_DEFINE(world, Drift);
    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "Move",
                                      .add = ecs_ids(ecs_dependson(
                                          MyeOnFixedUpdate)) }),
        .query.terms = {{ .id = ecs_id(MyePosition2D) },
                        { .id = ecs_id(Drift), .inout = EcsIn }},
        .callback = Move,
    });
    return world;
}

/* The guarantee the MyeOnCamera phase exists for: a follow camera reads the
 * target's DRAWN position, which mid-step differs from its simulated one.
 * Follow the simulated position instead and this fails. */
TEST(a_follow_camera_tracks_the_drawn_position_not_the_simulated_one)
{
    ecs_world_t *world = make_moving_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t player = mye_spawn_2d(world, (Vector2){ 0.0f, 0.0f });
    ecs_set(world, player, Drift, { 600.0f, 0.0f });
    ecs_set(world, player, MyeInterpolate, { 0 });

    ecs_entity_t cam = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    ecs_set(world, cam, MyeCameraFollow, { .target = player,
                                           .stiffness = 0.0f });

    /* 1.5 steps a frame alternates alpha between 0.5 and 0; stop on an odd
     * frame so the blend is live. */
    for (int i = 0; i < 9; ++i) {
        mye_progress(world, FIXED_DT * 1.5f);
    }
    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    ASSERT_NEAR(0.5f, time->alpha, 0.01f);

    Vector3 drawn = mye_render_position(world, player);
    Vector3 simulated = mye_world_position(world, player);
    ASSERT_TRUE(fabsf(drawn.x - simulated.x) > 1.0f); /* they really differ */

    const MyePosition2D *cam_pos = ecs_get(world, cam, MyePosition2D);
    ASSERT_NEAR(drawn.x, cam_pos->x, 0.01f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(follow_with_stiffness_converges_without_overshooting)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t target = mye_spawn_2d(world, (Vector2){ 100.0f, 0.0f });
    ecs_entity_t cam = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    ecs_set(world, cam, MyeCameraFollow, { .target = target,
                                           .stiffness = 8.0f });

    float previous = 0.0f;
    for (int i = 0; i < 120; ++i) {
        mye_progress(world, FIXED_DT);
        float x = ecs_get(world, cam, MyePosition2D)->x;
        ASSERT_TRUE(x >= previous - 0.001f); /* monotone */
        ASSERT_TRUE(x <= 100.0f + 0.001f);   /* never overshoots */
        previous = x;
    }
    ASSERT_NEAR(100.0f, previous, 0.5f); /* and it does arrive */

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The subtlety this whole design turns on: a follow system moves the camera
 * during MyeOnCamera, after propagation ran, so its stored world matrix is
 * a frame stale. Resolving must recompute from components. */
TEST(a_camera_moved_this_frame_resolves_to_where_it_was_moved)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t target = mye_spawn_2d(world, (Vector2){ 500.0f, 0.0f });
    ecs_entity_t cam = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    ecs_set(world, cam, MyeCameraFollow, { .target = target,
                                           .stiffness = 0.0f });
    mye_progress(world, FIXED_DT);

    /* The camera's own world matrix still holds the pre-move position... */
    Vector3 stale = mye_world_position(world, cam);
    ASSERT_TRUE(fabsf(stale.x - 500.0f) > 1.0f);

    /* ...but resolving reports where the follow system actually put it. */
    Camera2D c;
    ASSERT_TRUE(mye_camera2d_resolve(world, cam, &c));
    ASSERT_NEAR(500.0f, c.target.x, 0.01f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_dead_follow_target_leaves_the_camera_where_it_is)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t target = mye_spawn_2d(world, (Vector2){ 300.0f, 0.0f });
    ecs_entity_t cam = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    ecs_set(world, cam, MyeCameraFollow, { .target = target,
                                           .stiffness = 0.0f });
    mye_progress(world, FIXED_DT);
    ASSERT_NEAR(300.0f, ecs_get(world, cam, MyePosition2D)->x, 0.01f);

    ecs_delete(world, target);
    for (int i = 0; i < 5; ++i) {
        mye_progress(world, FIXED_DT);
    }
    /* Held, not snapped to the origin. */
    ASSERT_NEAR(300.0f, ecs_get(world, cam, MyePosition2D)->x, 0.01f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --- screen and world ---------------------------------------------------- */

TEST(a_world_point_projects_to_the_pixel_whose_ray_hits_it)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 10.0f },
                       (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    mye_progress(world, FIXED_DT);

    Vector3 point = { 2.0f, 1.0f, 0.0f };
    Vector2 screen = mye_world_to_screen(world, point);
    ASSERT_TRUE(screen.x > 0.0f && screen.x < 1280.0f);
    ASSERT_TRUE(screen.y > 0.0f && screen.y < 720.0f);

    /* Cast back: the ray under that pixel must pass through the point. */
    Ray ray = mye_screen_ray(world, screen);
    Vector3 to_point = Vector3Subtract(point, ray.position);
    float along = Vector3DotProduct(to_point, ray.direction);
    Vector3 closest = Vector3Add(ray.position,
                                 Vector3Scale(ray.direction, along));
    ASSERT_TRUE(Vector3Distance(closest, point) < 0.01f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(screen_and_world_round_trip_through_a_2d_camera)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    mye_camera2d_spawn(world, (Vector2){ 400.0f, 300.0f }, 2.0f);
    mye_progress(world, FIXED_DT);

    Vector2 point = { 450.0f, 260.0f };
    Vector2 screen = mye_world_to_screen_2d(world, point);
    Vector2 back = mye_screen_to_world_2d(world, screen);
    ASSERT_NEAR(point.x, back.x, 0.01f);
    ASSERT_NEAR(point.y, back.y, 0.01f);

    /* The camera's own position lands at the offset, which spawn put at the
     * centre of the viewport. */
    Vector2 centre = mye_world_to_screen_2d(world, (Vector2){ 400.0f, 300.0f });
    ASSERT_NEAR(640.0f, centre.x, 0.01f);
    ASSERT_NEAR(360.0f, centre.y, 0.01f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --- edge cases ---------------------------------------------------------- */

TEST(a_camera_with_no_transform_components_still_resolves)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    /* Hand-rolled rather than spawned: a camera component on a bare entity
     * must not fail, it just sits at the origin. */
    ecs_entity_t cam = mye_entity_new(world);
    ecs_set(world, cam, MyeCamera3D, { .fov = 70.0f,
                                       .projection = CAMERA_PERSPECTIVE,
                                       .active = true });
    mye_progress(world, FIXED_DT);

    Camera3D c;
    ASSERT_TRUE(mye_camera3d_resolve(world, cam, &c));
    assert_near_v3(T, c.position, (Vector3){ 0.0f, 0.0f, 0.0f }, 0.001f, "pos");
    ASSERT_NEAR(70.0f, c.fovy, 0.001f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(no_active_camera_is_reported_rather_than_guessed)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    Camera3D c3;
    Camera2D c2;
    ASSERT_FALSE(mye_camera3d_active(world, &c3, NULL));
    ASSERT_FALSE(mye_camera2d_active(world, &c2, NULL));

    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 5.0f },
                                          (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    MyeCamera3D *c = ecs_get_mut(world, cam, MyeCamera3D);
    c->active = false;
    ecs_modified(world, cam, MyeCamera3D);
    ASSERT_FALSE(mye_camera3d_active(world, &c3, NULL));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Which camera wins is otherwise arbitrary but stable, which looks
 * deliberate until the day it changes. */
TEST(more_than_one_active_camera_warns_once)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 5.0f },
                       (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 9.0f },
                       (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);

    mye_log_counts before = mye_log_get_counts();
    Camera3D c;
    ASSERT_TRUE(mye_camera3d_active(world, &c, NULL));
    mye_log_counts after_first = mye_log_get_counts();
    ASSERT_TRUE(after_first.warn > before.warn);

    /* Latched: it does not warn every frame for the rest of the run. */
    for (int i = 0; i < 5; ++i) {
        mye_camera3d_active(world, &c, NULL);
    }
    ASSERT_EQ_U64(after_first.warn, mye_log_get_counts().warn);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(a_root_camera_resolves_to_its_own_position_and_fov),
          TEST_CASE(a_camera_child_of_a_rotating_parent_turns_with_it),
          TEST_CASE(look_at_points_the_camera_at_the_target),
          TEST_CASE(look_at_is_correct_for_a_parented_camera),
          TEST_CASE(set_fov_changes_the_projection_and_nothing_else),
          TEST_CASE(a_follow_camera_tracks_the_drawn_position_not_the_simulated_one),
          TEST_CASE(follow_with_stiffness_converges_without_overshooting),
          TEST_CASE(a_camera_moved_this_frame_resolves_to_where_it_was_moved),
          TEST_CASE(a_dead_follow_target_leaves_the_camera_where_it_is),
          TEST_CASE(a_world_point_projects_to_the_pixel_whose_ray_hits_it),
          TEST_CASE(screen_and_world_round_trip_through_a_2d_camera),
          TEST_CASE(a_camera_with_no_transform_components_still_resolves),
          TEST_CASE(no_active_camera_is_reported_rather_than_guessed),
          TEST_CASE(more_than_one_active_camera_warns_once))
