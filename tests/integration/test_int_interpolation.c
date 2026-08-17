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
#include "scene/transform.h"

#include <raylib.h>
#include <raymath.h>

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
    ecs_world_t *world = mye_init(&(mye_config){ .headless = true});
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

/* --- interpolation composed through the hierarchy ------------------------ */

static ecs_entity_t spawn_node(ecs_world_t *world, float x, float y,
                               bool interpolated)
{
    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, MyePosition2D, { x, y });
    ecs_set(world, e, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, e, MyeWorldTransform, { MatrixIdentity() });
    if (interpolated) {
        ecs_set(world, e, MyeInterpolate, { 0 });
    }
    return e;
}

static Vector3 drawn_at(const ecs_world_t *world, ecs_entity_t e)
{
    const MyeRenderTransform *tf = ecs_get(world, e, MyeRenderTransform);
    return tf != NULL ? mye_matrix_translation(tf->m) : (Vector3){ 0, 0, 0 };
}

/* The bug this guards: the parent was drawn blended while the child was drawn
 * from the un-blended world transform, so the child drifted away from its
 * parent by up to one step of motion, every frame, at the display rate. */
TEST(a_child_is_drawn_rigidly_attached_to_an_interpolated_parent)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t parent = spawn_node(world, 0.0f, 0.0f, true);
    ecs_set(world, parent, Velocity2, { 600.0f, 0.0f });
    ecs_entity_t child = spawn_node(world, 10.0f, 0.0f, false);
    mye_set_parent(world, child, parent);

    /* 1.5 steps per frame alternates alpha between 0.5 and 0, so stop on an
     * odd frame: the blend is then live, which is exactly when parent and
     * child used to disagree. */
    for (int i = 0; i < 9; ++i) {
        mye_progress(world, FIXED_DT * 1.5f);
    }

    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    ASSERT_NEAR(0.5f, time->alpha, 0.01f);

    Vector3 p = drawn_at(world, parent);
    Vector3 c = drawn_at(world, child);
    ASSERT_NEAR(10.0f, c.x - p.x, 0.001f);
    ASSERT_NEAR(0.0f, c.y - p.y, 0.001f);

    /* And the blend is genuinely happening -- otherwise the assertion above
     * would pass for the trivial reason that nothing moved. */
    const MyeWorldTransform *world_tf = ecs_get(world, parent, MyeWorldTransform);
    ASSERT_TRUE(mye_matrix_translation(world_tf->m).x - p.x > 0.5f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(every_link_in_a_chain_contributes_its_own_blend)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t root = spawn_node(world, 0.0f, 0.0f, true);
    ecs_set(world, root, Velocity2, { 600.0f, 0.0f });
    ecs_entity_t mid = spawn_node(world, 10.0f, 0.0f, false);
    ecs_entity_t leaf = spawn_node(world, 5.0f, 3.0f, false);
    mye_set_parent(world, mid, root);
    mye_set_parent(world, leaf, mid);

    for (int i = 0; i < 9; ++i) {
        mye_progress(world, FIXED_DT * 1.5f);
    }

    Vector3 r = drawn_at(world, root);
    Vector3 l = drawn_at(world, leaf);
    ASSERT_NEAR(15.0f, l.x - r.x, 0.001f);
    ASSERT_NEAR(3.0f, l.y - r.y, 0.001f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* An interpolated child blends its OWN offset, on top of its parent's blend,
 * rather than having its local offset mistaken for a world position. */
TEST(an_interpolated_child_blends_its_own_offset)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t parent = spawn_node(world, 100.0f, 0.0f, false);
    ecs_entity_t child = spawn_node(world, 0.0f, 0.0f, true);
    ecs_set(world, child, Velocity2, { 600.0f, 0.0f });
    mye_set_parent(world, child, parent);

    for (int i = 0; i < 9; ++i) {
        mye_progress(world, FIXED_DT * 1.5f);
    }

    /* Drawn relative to the stationary parent, never near the origin... */
    Vector3 drawn = drawn_at(world, child);
    Vector3 simulated =
        mye_matrix_translation(ecs_get(world, child, MyeWorldTransform)->m);
    ASSERT_TRUE(drawn.x > 100.0f);

    /* ...and genuinely blended: at alpha 0.5 the drawn position trails the
     * simulated one by half a step of travel. Asserting the lag is what makes
     * this fail if the child's own blend is dropped. */
    float step_travel = 600.0f * FIXED_DT;
    ASSERT_NEAR(simulated.x - step_travel * 0.5f, drawn.x, 0.01f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* With nothing interpolating, the drawn transform must equal the simulated
 * one exactly -- opting out has to cost nothing and change nothing. */
TEST(the_render_transform_matches_the_world_transform_without_interpolation)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t parent = spawn_node(world, 7.0f, 11.0f, false);
    ecs_set(world, parent, Velocity2, { 600.0f, 0.0f });
    ecs_entity_t child = spawn_node(world, 4.0f, 2.0f, false);
    mye_set_parent(world, child, parent);

    for (int i = 0; i < 9; ++i) {
        mye_progress(world, FIXED_DT * 1.5f);
    }

    ecs_entity_t nodes[] = { parent, child };
    for (size_t i = 0; i < sizeof nodes / sizeof nodes[0]; ++i) {
        Vector3 drawn = drawn_at(world, nodes[i]);
        Vector3 simulated =
            mye_matrix_translation(ecs_get(world, nodes[i], MyeWorldTransform)->m);
        ASSERT_NEAR(simulated.x, drawn.x, 0.0001f);
        ASSERT_NEAR(simulated.y, drawn.y, 0.0001f);
    }

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Snapping a child suppresses only its own blend; its parent keeps blending,
 * and the child must still be composed onto it rather than dropping to the
 * origin. */
TEST(snapping_a_child_keeps_it_on_its_parent)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t parent = spawn_node(world, 0.0f, 0.0f, true);
    ecs_set(world, parent, Velocity2, { 600.0f, 0.0f });
    ecs_entity_t child = spawn_node(world, 10.0f, 0.0f, true);
    mye_set_parent(world, child, parent);

    for (int i = 0; i < 9; ++i) {
        mye_progress(world, FIXED_DT * 1.5f);
    }
    mye_transform_snap(world, child);
    mye_progress(world, FIXED_DT * 1.5f);

    Vector3 p = drawn_at(world, parent);
    Vector3 c = drawn_at(world, child);
    ASSERT_NEAR(10.0f, c.x - p.x, 0.001f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Leaving the hierarchy has to take the drawn transform with it: a stale one
 * outranks the entity's position when drawing, which would freeze the sprite
 * wherever it was last composed. */
TEST(leaving_the_hierarchy_drops_the_render_transform)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t e = spawn_node(world, 5.0f, 6.0f, false);
    mye_progress(world, FIXED_DT);
    ASSERT_TRUE(ecs_get(world, e, MyeRenderTransform) != NULL);

    ecs_remove(world, e, MyeWorldTransform);
    mye_progress(world, FIXED_DT);
    ASSERT_TRUE(ecs_get(world, e, MyeRenderTransform) == NULL);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(opting_in_is_required_and_costs_nothing_otherwise),
          TEST_CASE(snapping_a_child_keeps_it_on_its_parent),
          TEST_CASE(leaving_the_hierarchy_drops_the_render_transform),
          TEST_CASE(a_child_is_drawn_rigidly_attached_to_an_interpolated_parent),
          TEST_CASE(every_link_in_a_chain_contributes_its_own_blend),
          TEST_CASE(an_interpolated_child_blends_its_own_offset),
          TEST_CASE(the_render_transform_matches_the_world_transform_without_interpolation),
          TEST_CASE(previous_position_trails_the_current_one_by_exactly_one_step),
          TEST_CASE(a_frame_running_several_steps_still_trails_by_one),
          TEST_CASE(snapping_suppresses_the_blend_after_a_teleport),
          TEST_CASE(snapping_an_uninterpolated_entity_is_harmless),
          TEST_CASE(alpha_stays_in_range_so_the_blend_never_overshoots))
