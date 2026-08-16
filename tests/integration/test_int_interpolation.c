/* Integration tests for opt-in render interpolation.
 * See plan/03-rendering.md.
 *
 * Interpolation is invisible from outside the renderer by design -- the blend
 * is computed into the draw list and never stored -- so these tests verify
 * the observable contract instead: the engine maintains previous positions
 * for entities that opted in, leaves everything else alone, and suppresses
 * the blend after a teleport. */
#include "core/engine.h"
#include "mye_test.h"
#include "render/render2d.h"

#include <raylib.h>

#define FIXED_DT (1.0f / 60.0f)

typedef struct Velocity2 {
    float x, y;
} Velocity2;

ECS_COMPONENT_DECLARE(Velocity2);

static void Move(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const Velocity2 *vel = ecs_field(it, Velocity2, 1);
    for (int i = 0; i < it->count; ++i) {
        pos[i].x += vel[i].x * (float)it->delta_time;
        pos[i].y += vel[i].y * (float)it->delta_time;
    }
}

static ecs_world_t *make_world(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .headless = true,
                                                 .asset_workers = -1 });
    if (world == NULL) {
        return NULL;
    }
    ECS_COMPONENT_DEFINE(world, Velocity2);
    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "Move",
                                      .add = ecs_ids(ecs_dependson(
                                          MyeOnFixedUpdate)) }),
        .query.terms = {{ .id = ecs_id(MyePosition2D) },
                        { .id = ecs_id(Velocity2), .inout = EcsIn }},
        .callback = Move,
    });
    return world;
}

static ecs_entity_t spawn(ecs_world_t *world, bool interpolated)
{
    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, e, Velocity2, { 60.0f, 0.0f }); /* 1 unit per fixed step */
    if (interpolated) {
        ecs_set(world, e, MyeInterpolate, { 0 });
    }
    return e;
}

TEST(opting_in_is_required_and_costs_nothing_otherwise)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t plain = spawn(world, false);
    ecs_entity_t smooth = spawn(world, true);

    mye_progress(world, FIXED_DT);

    /* The engine maintains previous positions only for the entity that
     * asked. Nothing is added behind the developer's back. */
    ASSERT_NULL(ecs_get(world, plain, MyeInterpolate));
    ASSERT_NOT_NULL(ecs_get(world, smooth, MyeInterpolate));

    /* And both simulate identically -- interpolation is a rendering
     * concern and must not affect the simulation at all. */
    const MyePosition2D *a = ecs_get(world, plain, MyePosition2D);
    const MyePosition2D *b = ecs_get(world, smooth, MyePosition2D);
    ASSERT_NEAR(a->x, b->x, 1e-6);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(previous_position_trails_the_current_one_by_exactly_one_step)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t e = spawn(world, true);

    /* One step: moves 1 unit, and previous records where it started. */
    mye_progress(world, FIXED_DT);
    const MyePosition2D *pos = ecs_get(world, e, MyePosition2D);
    const MyeInterpolate *interp = ecs_get(world, e, MyeInterpolate);
    ASSERT_NEAR(1.0, pos->x, 1e-4);
    ASSERT_NEAR(0.0, interp->prev_x, 1e-4);

    /* Another step: the gap stays exactly one step wide. */
    mye_progress(world, FIXED_DT);
    pos = ecs_get(world, e, MyePosition2D);
    interp = ecs_get(world, e, MyeInterpolate);
    ASSERT_NEAR(2.0, pos->x, 1e-4);
    ASSERT_NEAR(1.0, interp->prev_x, 1e-4);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_frame_running_several_steps_still_trails_by_one)
{
    /* The subtle case: capturing once per *frame* would leave previous three
     * steps behind after a catch-up, and the entity would appear to lurch.
     * Capturing at the top of each step keeps the gap at one. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t e = spawn(world, true);

    mye_progress(world, FIXED_DT * 4.0f); /* four steps in one frame */

    const MyePosition2D *pos = ecs_get(world, e, MyePosition2D);
    const MyeInterpolate *interp = ecs_get(world, e, MyeInterpolate);
    ASSERT_NEAR(4.0, pos->x, 1e-4);
    ASSERT_NEAR(3.0, interp->prev_x, 1e-4); /* one step back, not four */

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(snapping_suppresses_the_blend_after_a_teleport)
{
    /* Without this, a screen wrap from x=1270 to x=10 would draw the sprite
     * streaking across everything in between. Asteroids wraps constantly. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t e = spawn(world, true);
    mye_progress(world, FIXED_DT);
    mye_progress(world, FIXED_DT);

    /* Teleport, the way a wrap would. */
    ecs_set(world, e, MyePosition2D, { 1000.0f, 0.0f });
    mye_transform_snap(world, e);

    const MyeInterpolate *interp = ecs_get(world, e, MyeInterpolate);
    ASSERT_TRUE(interp->snap);
    /* previous was moved to the destination, so even a blend would be a
     * no-op rather than a streak across 1000 units. */
    ASSERT_NEAR(1000.0, interp->prev_x, 1e-4);

    /* The flag clears on the next step, so smoothing resumes immediately
     * afterwards rather than being disabled for good. */
    mye_progress(world, FIXED_DT);
    interp = ecs_get(world, e, MyeInterpolate);
    ASSERT_FALSE(interp->snap);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(snapping_an_uninterpolated_entity_is_harmless)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t plain = spawn(world, false);
    mye_transform_snap(world, plain);          /* no component: no-op */
    mye_transform_snap(world, 0);              /* not an entity */
    ASSERT_NULL(ecs_get(world, plain, MyeInterpolate));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(alpha_stays_in_range_so_the_blend_never_overshoots)
{
    /* The blend factor must be a fraction of one step. If it exceeded 1 the
     * renderer would extrapolate past the current position and objects would
     * visibly overshoot and snap back. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    spawn(world, true);

    const float deltas[] = { FIXED_DT * 0.25f, FIXED_DT, FIXED_DT * 2.75f,
                             FIXED_DT * 0.9f, 10.0f };
    for (size_t i = 0; i < sizeof deltas / sizeof deltas[0]; ++i) {
        mye_progress(world, deltas[i]);
        const MyeTime *time = ecs_singleton_get(world, MyeTime);
        ASSERT_TRUE(time->alpha >= 0.0f);
        ASSERT_TRUE(time->alpha < 1.0f);
    }

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(opting_in_is_required_and_costs_nothing_otherwise),
          TEST_CASE(previous_position_trails_the_current_one_by_exactly_one_step),
          TEST_CASE(a_frame_running_several_steps_still_trails_by_one),
          TEST_CASE(snapping_suppresses_the_blend_after_a_teleport),
          TEST_CASE(snapping_an_uninterpolated_entity_is_harmless),
          TEST_CASE(alpha_stays_in_range_so_the_blend_never_overshoots))
