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

TEST(a_dead_follow_target_changes_nothing_at_all)
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
    mye_log_counts before = mye_log_get_counts();
    for (int i = 0; i < 5; ++i) {
        mye_progress(world, FIXED_DT);
    }
    /* Held exactly where it was, and nothing said: what a dead target means
     * is the game's call, not the engine's. */
    ASSERT_NEAR(300.0f, ecs_get(world, cam, MyePosition2D)->x, 0.01f);
    ASSERT_EQ_U64(before.warn, mye_log_get_counts().warn);

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
    /* Right of and above centre -- a mirrored camera would still round-trip
     * with itself, so the direction is asserted, not just the consistency. */
    ASSERT_TRUE(screen.x > 640.0f && screen.x < 1280.0f);
    ASSERT_TRUE(screen.y > 0.0f && screen.y < 360.0f);

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

/* Several cameras is an ordinary thing to have -- a main view and a minimap.
 * The engine draws all of them and arbitrates nothing. */
TEST(every_active_camera_is_collected_in_order)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t main_view = mye_camera3d_spawn(
        world, (Vector3){ 0.0f, 2.0f, 8.0f }, (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    ecs_entity_t minimap = mye_camera3d_spawn(
        world, (Vector3){ 0.0f, 200.0f, 0.0f }, (Vector3){ 0.0f, 0.0f, 0.0f }, 30.0f);

    /* The minimap draws on top, and into a corner. */
    MyeCamera3D *m = ecs_get_mut(world, minimap, MyeCamera3D);
    m->order = 1;
    m->viewport = (Rectangle){ 1024.0f, 0.0f, 256.0f, 256.0f };
    ecs_modified(world, minimap, MyeCamera3D);
    mye_progress(world, FIXED_DT);

    mye_log_counts before = mye_log_get_counts();

    ecs_entity_t got[MYE_MAX_DRAWN_CAMERAS];
    ASSERT_EQ_INT(2, mye_camera3d_collect(world, got, MYE_MAX_DRAWN_CAMERAS));
    ASSERT_TRUE(got[0] == main_view); /* order 0 first */
    ASSERT_TRUE(got[1] == minimap);

    /* Swapping the order swaps who is on top. */
    m = ecs_get_mut(world, minimap, MyeCamera3D);
    m->order = -1;
    ecs_modified(world, minimap, MyeCamera3D);
    ASSERT_EQ_INT(2, mye_camera3d_collect(world, got, MYE_MAX_DRAWN_CAMERAS));
    ASSERT_TRUE(got[0] == minimap);

    /* Nothing to complain about: several cameras is not a mistake. */
    ASSERT_EQ_U64(before.warn, mye_log_get_counts().warn);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A camera that never set a viewport covers the window -- the single-camera
 * path, which must not require thinking about rects. */
TEST(a_zero_viewport_means_the_whole_window)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t cam = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    mye_progress(world, FIXED_DT);

    Rectangle vp = mye_camera_viewport(world, cam);
    ASSERT_NEAR(0.0f, vp.x, 0.001f);
    ASSERT_NEAR(0.0f, vp.y, 0.001f);
    ASSERT_NEAR(1280.0f, vp.width, 0.001f);
    ASSERT_NEAR(720.0f, vp.height, 0.001f);

    MyeCamera2D *c = ecs_get_mut(world, cam, MyeCamera2D);
    c->viewport = (Rectangle){ 10.0f, 20.0f, 300.0f, 200.0f };
    ecs_modified(world, cam, MyeCamera2D);
    vp = mye_camera_viewport(world, cam);
    ASSERT_NEAR(300.0f, vp.width, 0.001f);
    ASSERT_NEAR(20.0f, vp.y, 0.001f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Split-screen picking: a click must be read against the camera it landed
 * in, or player two's clicks are interpreted in player one's world. */
TEST(a_screen_point_resolves_to_the_camera_whose_viewport_contains_it)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t left = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    ecs_entity_t right = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    MyeCamera2D *l = ecs_get_mut(world, left, MyeCamera2D);
    l->viewport = (Rectangle){ 0.0f, 0.0f, 640.0f, 720.0f };
    ecs_modified(world, left, MyeCamera2D);
    MyeCamera2D *r = ecs_get_mut(world, right, MyeCamera2D);
    r->viewport = (Rectangle){ 640.0f, 0.0f, 640.0f, 720.0f };
    ecs_modified(world, right, MyeCamera2D);
    mye_progress(world, FIXED_DT);

    ASSERT_TRUE(mye_camera_at_screen(world, (Vector2){ 100.0f, 360.0f }) == left);
    ASSERT_TRUE(mye_camera_at_screen(world, (Vector2){ 900.0f, 360.0f }) == right);
    /* Outside every viewport: nothing, rather than a plausible wrong answer. */
    ASSERT_TRUE(mye_camera_at_screen(world, (Vector2){ 100.0f, 900.0f }) == 0);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --- layers -------------------------------------------------------------- */

/* Layer masks are pure data, so the rule is checked here rather than through
 * a window; that the passes actually apply it is a pixel test
 * (test_int_render_cameras.c). */
TEST(a_camera_draws_only_the_layers_it_shares)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    /* A blip on layer 1 (bit 1), the minimap-shows-blips case. */
    ecs_entity_t blip = mye_spawn_2d(world, (Vector2){ 0.0f, 0.0f });
    ecs_set(world, blip, MyeVisibilityLayers, { .mask = 2 });

    ecs_entity_t minimap = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f },
                                              1.0f);
    MyeCamera2D *m = ecs_get_mut(world, minimap, MyeCamera2D);
    m->layers = 2;
    ecs_modified(world, minimap, MyeCamera2D);

    ecs_entity_t main_view = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f },
                                                1.0f);
    MyeCamera2D *v = ecs_get_mut(world, main_view, MyeCamera2D);
    v->layers = 1; /* layer 0 only */
    ecs_modified(world, main_view, MyeCamera2D);
    mye_progress(world, FIXED_DT);

    ASSERT_TRUE(mye_camera_sees(world, minimap, blip));
    ASSERT_FALSE(mye_camera_sees(world, main_view, blip));

    /* Any bit matching is enough -- a camera watching several layers. */
    v = ecs_get_mut(world, main_view, MyeCamera2D);
    v->layers = 1 | 2;
    ecs_modified(world, main_view, MyeCamera2D);
    ASSERT_TRUE(mye_camera_sees(world, main_view, blip));

    /* A 3D camera applies the same rule; there is only one rule. */
    ecs_entity_t cam3d = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 5.0f },
                                            (Vector3){ 0.0f, 0.0f, 0.0f },
                                            60.0f);
    MyeCamera3D *c = ecs_get_mut(world, cam3d, MyeCamera3D);
    c->layers = 4;
    ecs_modified(world, cam3d, MyeCamera3D);
    ASSERT_FALSE(mye_camera_sees(world, cam3d, blip));
    c = ecs_get_mut(world, cam3d, MyeCamera3D);
    c->layers = 2;
    ecs_modified(world, cam3d, MyeCamera3D);
    ASSERT_TRUE(mye_camera_sees(world, cam3d, blip));

    /* Neither of these draws anything, so neither sees anything. */
    ASSERT_FALSE(mye_camera_sees(world, blip, blip));
    ASSERT_FALSE(mye_camera_sees(world, minimap, 0));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The default at both ends is deliberately generous, so that nothing existing
 * changes and no field left unset can make anything invisible. */
TEST(an_entity_with_no_visibility_layers_is_visible_to_every_camera)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t plain = mye_spawn_2d(world, (Vector2){ 0.0f, 0.0f });

    ecs_entity_t any = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f },
                                          1.0f); /* layers = 0 */
    ecs_entity_t masked = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f },
                                             1.0f);
    MyeCamera2D *c = ecs_get_mut(world, masked, MyeCamera2D);
    c->layers = 4;
    ecs_modified(world, masked, MyeCamera2D);
    mye_progress(world, FIXED_DT);

    /* Unlabelled: drawn by the camera that named no layers, and by the one
     * that named layer 2. An entity that says nothing is on every layer. */
    ASSERT_TRUE(mye_camera_sees(world, any, plain));
    ASSERT_TRUE(mye_camera_sees(world, masked, plain));

    /* And a camera that named no layers draws a labelled entity, whatever it
     * is labelled with: 0 means "do not filter", not "layer 0". */
    ecs_entity_t labelled = mye_spawn_2d(world, (Vector2){ 0.0f, 0.0f });
    ecs_set(world, labelled, MyeVisibilityLayers, { .mask = 8 });
    ASSERT_TRUE(mye_camera_sees(world, any, labelled));

    /* The way to hide something from every masked camera is to say so: an
     * empty mask is on no layer at all. */
    ecs_entity_t nowhere = mye_spawn_2d(world, (Vector2){ 0.0f, 0.0f });
    ecs_set(world, nowhere, MyeVisibilityLayers, { .mask = 0 });
    ASSERT_FALSE(mye_camera_sees(world, masked, nowhere));
    ASSERT_TRUE(mye_camera_sees(world, any, nowhere)); /* still not filtered */

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --- picking through a viewport ------------------------------------------ */

/* The plan's named case: "a ray cast from a minimap pixel must not be
 * interpreted as a main-view pixel". The screen point has to be brought into
 * the camera's rect, and the projection built with the RECT's aspect -- the
 * same two things mye_camera_begin_3d does when it draws. */
TEST(a_ray_from_a_viewport_restricted_camera_accounts_for_the_offset)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    /* The right half of a 1280x720 window: player two. */
    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 10.0f },
                                          (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    MyeCamera3D *c = ecs_get_mut(world, cam, MyeCamera3D);
    c->viewport = (Rectangle){ 640.0f, 0.0f, 640.0f, 720.0f };
    ecs_modified(world, cam, MyeCamera3D);
    mye_progress(world, FIXED_DT);

    /* The centre of THAT viewport looks straight down the camera's axis. */
    Ray centre = mye_screen_ray_for(world, cam, (Vector2){ 960.0f, 360.0f });
    assert_near_v3(T, centre.position, (Vector3){ 0.0f, 0.0f, 10.0f }, 0.01f,
                   "ray origin");
    assert_near_v3(T, centre.direction, (Vector3){ 0.0f, 0.0f, -1.0f }, 0.01f,
                   "centre ray");

    /* The centre of the WINDOW is that viewport's left edge, so its ray leans
     * left. Ignore the offset and this is the ray above -- which is exactly
     * how player two's clicks land in player one's world. */
    Ray edge = mye_screen_ray_for(world, cam, (Vector2){ 640.0f, 360.0f });
    ASSERT_TRUE(edge.direction.x < -0.4f);

    /* Projecting lands in the camera's own half, at 960 rather than 640. */
    Vector2 ahead = mye_world_to_screen_for(world, cam,
                                            (Vector3){ 0.0f, 0.0f, 0.0f });
    ASSERT_NEAR(960.0f, ahead.x, 0.5f);
    ASSERT_NEAR(360.0f, ahead.y, 0.5f);

    /* And with the RECT's aspect, not the window's: at ten units away the
     * half-width of a 640x720 viewport at fov 60 is tan(30)*10*640/720 =
     * 5.132 world units, so that point sits on its right edge. Projected
     * against the window's aspect it would land at 1120 instead. */
    Vector2 right_edge = mye_world_to_screen_for(world, cam,
                                                 (Vector3){ 5.132f, 0.0f, 0.0f });
    ASSERT_NEAR(1280.0f, right_edge.x, 1.0f);

    /* Round trip: the ray under the pixel a point projects to passes through
     * that point. */
    Vector3 point = { 1.0f, 0.5f, 0.0f };
    Vector2 screen = mye_world_to_screen_for(world, cam, point);
    ASSERT_TRUE(screen.x > 640.0f && screen.x < 1280.0f);
    Ray ray = mye_screen_ray_for(world, cam, screen);
    Vector3 to_point = Vector3Subtract(point, ray.position);
    Vector3 closest = Vector3Add(
        ray.position,
        Vector3Scale(ray.direction, Vector3DotProduct(to_point, ray.direction)));
    ASSERT_TRUE(Vector3Distance(closest, point) < 0.01f);

    /* Not a 3D camera: zero, rather than a plausible wrong answer. */
    ecs_entity_t flat = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    ASSERT_NEAR(0.0f, mye_world_to_screen_for(world, flat, point).x, 0.001f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Split-screen picking end to end: the same window pixel means different
 * world points to the two players, and the implicit helpers must answer with
 * the camera the pixel is actually in. */
TEST(a_click_in_the_second_viewport_is_read_in_that_camera_s_world)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t left = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f },
                                           1.0f);
    MyeCamera2D *l = ecs_get_mut(world, left, MyeCamera2D);
    l->viewport = (Rectangle){ 0.0f, 0.0f, 640.0f, 720.0f };
    /* The offset is where the camera's position lands WITHIN its viewport, so
     * each player's centre is the centre of their own half. */
    l->offset = (Vector2){ 320.0f, 360.0f };
    ecs_modified(world, left, MyeCamera2D);

    /* Player two is a thousand units away in the same world, and draws first
     * -- so it is also the "active" camera, which is what makes the
     * world→screen half of this test bite. */
    ecs_entity_t right = mye_camera2d_spawn(world, (Vector2){ 1000.0f, 0.0f },
                                            1.0f);
    MyeCamera2D *r = ecs_get_mut(world, right, MyeCamera2D);
    r->viewport = (Rectangle){ 640.0f, 0.0f, 640.0f, 720.0f };
    r->offset = (Vector2){ 320.0f, 360.0f };
    r->order = -1;
    ecs_modified(world, right, MyeCamera2D);
    mye_progress(world, FIXED_DT);

    /* A click in the middle of each half is that player's own centre. Read
     * against the full window instead and the right-hand click reports
     * (320, 0) -- a real point in player one's world, which is what makes the
     * bug invisible. */
    Vector2 in_right = mye_screen_to_world_2d(world, (Vector2){ 960.0f, 360.0f });
    ASSERT_NEAR(1000.0f, in_right.x, 0.01f);
    ASSERT_NEAR(0.0f, in_right.y, 0.01f);

    Vector2 in_left = mye_screen_to_world_2d(world, (Vector2){ 320.0f, 360.0f });
    ASSERT_NEAR(0.0f, in_left.x, 0.01f);
    ASSERT_NEAR(0.0f, in_left.y, 0.01f);

    /* And back: the active camera is player two's, whose viewport starts at
     * x = 640, so its own position lands at 960 on the window. */
    Vector2 screen = mye_world_to_screen_2d(world, (Vector2){ 1000.0f, 0.0f });
    ASSERT_NEAR(960.0f, screen.x, 0.01f);
    ASSERT_NEAR(360.0f, screen.y, 0.01f);

    /* A pixel in no viewport at all has no right answer, so the active camera
     * answers -- still through ITS rect, which is what a single-camera game
     * has always got. Below both halves and 320 px left of player two's
     * origin: 640 px from their centre, so 640 world units left of it. */
    Vector2 outside = mye_screen_to_world_2d(world, (Vector2){ 320.0f, 900.0f });
    ASSERT_NEAR(360.0f, outside.x, 0.01f);
    ASSERT_NEAR(540.0f, outside.y, 0.01f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* An inactive camera is not drawn, but is still resolvable -- a cutscene rig
 * on standby is a normal thing to keep around. */
TEST(an_inactive_camera_is_not_collected_but_still_resolves)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t live = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 5.0f },
                                           (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    ecs_entity_t standby = mye_camera3d_spawn(world, (Vector3){ 0.0f, 9.0f, 0.0f },
                                              (Vector3){ 0.0f, 0.0f, 0.0f }, 60.0f);
    MyeCamera3D *c = ecs_get_mut(world, standby, MyeCamera3D);
    c->active = false;
    ecs_modified(world, standby, MyeCamera3D);
    mye_progress(world, FIXED_DT);

    ecs_entity_t got[MYE_MAX_DRAWN_CAMERAS];
    ASSERT_EQ_INT(1, mye_camera3d_collect(world, got, MYE_MAX_DRAWN_CAMERAS));
    ASSERT_TRUE(got[0] == live);

    Camera3D r;
    ASSERT_TRUE(mye_camera3d_resolve(world, standby, &r));
    ASSERT_NEAR(9.0f, r.position.y, 0.001f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --- gaps a review found ------------------------------------------------- */

/* The player the design was built for: an interpolated sprite that is NOT in
 * the hierarchy (mye_sprite_spawn + MyeInterpolate, as in the tutorial and
 * Asteroids). The sprite pass draws it blended; the drawn position must say
 * the same, or a follow camera slides the world under the player. */
TEST(render_position_blends_an_interpolated_sprite_outside_the_hierarchy)
{
    ecs_world_t *world = make_moving_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t player = mye_entity_new(world);
    ecs_set(world, player, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, player, Drift, { 600.0f, 0.0f });
    ecs_set(world, player, MyeInterpolate, { 0 });
    /* No MyeLocalTransform / MyeWorldTransform on purpose. */

    for (int i = 0; i < 9; ++i) {
        mye_progress(world, FIXED_DT * 1.5f);
    }
    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    ASSERT_NEAR(0.5f, time->alpha, 0.01f);

    const MyePosition2D *pos = ecs_get(world, player, MyePosition2D);
    const MyeInterpolate *interp = ecs_get(world, player, MyeInterpolate);
    float drawn_by_sprite_pass =
        interp->prev_x + (pos->x - interp->prev_x) * time->alpha;

    Vector3 reported = mye_render_position(world, player);
    ASSERT_TRUE(fabsf(pos->x - drawn_by_sprite_pass) > 1.0f); /* they differ */
    ASSERT_NEAR(drawn_by_sprite_pass, reported.x, 0.01f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* QuaternionFromMatrix on a scaled matrix gives a quaternion that scales
 * whatever it rotates. A follow offset on the showcase's x900 boombox would
 * land tens of kilometres away. */
TEST(render_rotation_is_a_pure_rotation_even_for_a_scaled_entity)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t big = mye_spawn_3d(world, (Vector3){ 0.0f, 0.0f, 0.0f });
    ecs_set(world, big, MyeScale3D, { { 900.0f, 900.0f, 900.0f } });
    ecs_set(world, big, MyeRotation3D,
            { QuaternionFromAxisAngle((Vector3){ 0.0f, 1.0f, 0.0f },
                                      PI / 2.0f) });
    mye_progress(world, FIXED_DT);

    Quaternion q = mye_render_rotation(world, big);
    ASSERT_NEAR(1.0f, QuaternionLength(q), 0.001f);

    /* Yawed a quarter turn: local +Z points along world +X, unit length. */
    Vector3 v = Vector3RotateByQuaternion((Vector3){ 0.0f, 0.0f, 1.0f }, q);
    assert_near_v3(T, v, (Vector3){ 1.0f, 0.0f, 0.0f }, 0.001f, "rotated");

    /* And the offset a follow would apply stays an offset. */
    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 0.0f },
                                          (Vector3){ 0.0f, 0.0f, 1.0f }, 60.0f);
    ecs_set(world, cam, MyeCameraFollow, { .target = big,
                                           .offset = { 0.0f, 2.0f, 5.0f },
                                           .stiffness = 0.0f });
    mye_progress(world, FIXED_DT);
    assert_near_v3(T, ecs_get(world, cam, MyePosition3D)->v,
                   (Vector3){ 5.0f, 2.0f, 0.0f }, 0.01f, "offset");

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A following camera that is itself parented: the target is a world point,
 * the camera's position is in its parent's space. */
TEST(a_parented_follow_camera_lands_on_the_target_not_beside_it)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t rig = mye_spawn_2d(world, (Vector2){ 1000.0f, 0.0f });
    ecs_entity_t player = mye_spawn_2d(world, (Vector2){ 50.0f, 0.0f });
    ecs_entity_t cam = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    mye_set_parent(world, cam, rig);
    ecs_set(world, cam, MyeCameraFollow, { .target = player,
                                           .stiffness = 0.0f });
    mye_progress(world, FIXED_DT);
    mye_progress(world, FIXED_DT);

    Camera2D c;
    ASSERT_TRUE(mye_camera2d_resolve(world, cam, &c));
    ASSERT_NEAR(50.0f, c.target.x, 0.01f); /* not 1050 */

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The 2D view turns with the entity, from the transform -- there is no
 * separate rotation field to fall out of step with it. */
TEST(a_2d_camera_s_view_turns_with_its_rotation)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t cam = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    ecs_set(world, cam, MyeRotation2D, { PI / 2.0f });
    mye_progress(world, FIXED_DT);

    Camera2D c;
    ASSERT_TRUE(mye_camera2d_resolve(world, cam, &c));
    ASSERT_NEAR(90.0f, fabsf(c.rotation), 0.01f);

    /* The camera's local +x axis is what should run along the screen's +x:
     * a world point 100 units along it lands 100 px right of centre. */
    Vector2 along_local_x = { 0.0f, 100.0f }; /* world = rotate((100,0), 90) */
    Vector2 on_screen = mye_world_to_screen_2d(world, along_local_x);
    ASSERT_NEAR(740.0f, on_screen.x, 0.5f);
    ASSERT_NEAR(360.0f, on_screen.y, 0.5f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A camera parented to an interpolated entity is carried by the BLENDED
 * parent -- the path through camera_parent_matrix's MyeRenderTransform. */
TEST(a_camera_child_of_an_interpolated_parent_rides_the_blend)
{
    ecs_world_t *world = make_moving_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t player = mye_spawn_2d(world, (Vector2){ 0.0f, 0.0f });
    ecs_set(world, player, Drift, { 600.0f, 0.0f });
    ecs_set(world, player, MyeInterpolate, { 0 });
    ecs_entity_t cam = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    mye_set_parent(world, cam, player);

    for (int i = 0; i < 9; ++i) {
        mye_progress(world, FIXED_DT * 1.5f);
    }
    ASSERT_NEAR(0.5f, ecs_singleton_get(world, MyeTime)->alpha, 0.01f);

    Camera2D c;
    ASSERT_TRUE(mye_camera2d_resolve(world, cam, &c));
    Vector3 drawn = mye_render_position(world, player);
    Vector3 stepped = mye_world_position(world, player);
    ASSERT_TRUE(fabsf(drawn.x - stepped.x) > 1.0f);
    ASSERT_NEAR(drawn.x, c.target.x, 0.01f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_3d_follow_applies_its_offset_in_the_target_s_frame)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t hero = mye_spawn_3d(world, (Vector3){ 10.0f, 0.0f, 10.0f });
    ecs_set(world, hero, MyeRotation3D,
            { QuaternionFromAxisAngle((Vector3){ 0.0f, 1.0f, 0.0f },
                                      PI / 2.0f) });
    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 0.0f, 0.0f },
                                          (Vector3){ 0.0f, 0.0f, 1.0f }, 60.0f);
    /* "Behind and above" -- and it stays behind as the hero turns. */
    ecs_set(world, cam, MyeCameraFollow, { .target = hero,
                                           .offset = { 0.0f, 2.0f, 5.0f },
                                           .stiffness = 0.0f });
    mye_progress(world, FIXED_DT);

    /* Hero yawed +90: its local +Z is world +X, so behind = +5 in X. */
    assert_near_v3(T, ecs_get(world, cam, MyePosition3D)->v,
                   (Vector3){ 15.0f, 2.0f, 10.0f }, 0.01f, "3d follow");

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(orthographic_projection_passes_through)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t cam = mye_camera3d_spawn(world, (Vector3){ 0.0f, 10.0f, 0.0f },
                                          (Vector3){ 0.0f, 0.0f, 0.0f }, 20.0f);
    MyeCamera3D *c = ecs_get_mut(world, cam, MyeCamera3D);
    c->projection = CAMERA_ORTHOGRAPHIC;
    ecs_modified(world, cam, MyeCamera3D);
    mye_progress(world, FIXED_DT);

    Camera3D r;
    ASSERT_TRUE(mye_camera3d_resolve(world, cam, &r));
    ASSERT_EQ_INT(CAMERA_ORTHOGRAPHIC, r.projection);
    ASSERT_NEAR(20.0f, r.fovy, 0.001f); /* ortho: fovy is the view height */

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A camera parented to a bare entity cannot be carried; it must say so
 * rather than silently sit at the origin. */
TEST(a_camera_parented_to_a_transformless_entity_warns_once)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t bare = mye_entity_new(world);
    ecs_set(world, bare, MyePosition2D, { 300.0f, 200.0f });
    ecs_entity_t cam = mye_camera2d_spawn(world, (Vector2){ 0.0f, 0.0f }, 1.0f);
    mye_set_parent(world, cam, bare);

    /* Headless has no draw pass to resolve the camera, so resolve it the
     * way a game's own code would. Five times: the warning latches. */
    mye_log_counts before = mye_log_get_counts();
    Camera2D c;
    for (int i = 0; i < 5; ++i) {
        mye_progress(world, FIXED_DT);
        ASSERT_TRUE(mye_camera2d_resolve(world, cam, &c));
    }
    ASSERT_EQ_U64(before.warn + 1, mye_log_get_counts().warn);
    ASSERT_NEAR(0.0f, c.target.x, 0.001f); /* and it does sit at the origin */

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
          TEST_CASE(a_dead_follow_target_changes_nothing_at_all),
          TEST_CASE(a_world_point_projects_to_the_pixel_whose_ray_hits_it),
          TEST_CASE(screen_and_world_round_trip_through_a_2d_camera),
          TEST_CASE(a_camera_with_no_transform_components_still_resolves),
          TEST_CASE(no_active_camera_is_reported_rather_than_guessed),
          TEST_CASE(every_active_camera_is_collected_in_order),
          TEST_CASE(a_zero_viewport_means_the_whole_window),
          TEST_CASE(a_screen_point_resolves_to_the_camera_whose_viewport_contains_it),
          TEST_CASE(a_camera_draws_only_the_layers_it_shares),
          TEST_CASE(an_entity_with_no_visibility_layers_is_visible_to_every_camera),
          TEST_CASE(a_ray_from_a_viewport_restricted_camera_accounts_for_the_offset),
          TEST_CASE(a_click_in_the_second_viewport_is_read_in_that_camera_s_world),
          TEST_CASE(an_inactive_camera_is_not_collected_but_still_resolves),
          TEST_CASE(render_position_blends_an_interpolated_sprite_outside_the_hierarchy),
          TEST_CASE(render_rotation_is_a_pure_rotation_even_for_a_scaled_entity),
          TEST_CASE(a_parented_follow_camera_lands_on_the_target_not_beside_it),
          TEST_CASE(a_2d_camera_s_view_turns_with_its_rotation),
          TEST_CASE(a_camera_child_of_an_interpolated_parent_rides_the_blend),
          TEST_CASE(a_3d_follow_applies_its_offset_in_the_target_s_frame),
          TEST_CASE(orthographic_projection_passes_through),
          TEST_CASE(a_camera_parented_to_a_transformless_entity_warns_once))
