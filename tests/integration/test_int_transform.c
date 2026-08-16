/* Integration tests for the transform hierarchy. Headless: matrices are
 * maths, not pixels. See plan/03-rendering.md. */
#include "core/engine.h"
#include "mye_test.h"
#include "render/render2d.h"
#include "scene/transform.h"

#include <raymath.h>

#define FIXED_DT (1.0f / 60.0f)

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){ .headless = true, .asset_workers = -1 });
}

static void assert_near_v3(mye_test_ctx *T, Vector3 expected, Vector3 actual,
                           float eps)
{
    ASSERT_NEAR(expected.x, actual.x, eps);
    ASSERT_NEAR(expected.y, actual.y, eps);
    ASSERT_NEAR(expected.z, actual.z, eps);
}

/* -------------------------------------------------------------- pure maths -- */

TEST(trs_applies_scale_then_rotation_then_translation)
{
    /* A point at (1,0,0), scaled x2, rotated 90 degrees about Z, then moved
     * to (10,0,0), must land at (10,2,0): scale first, translation last. */
    Matrix m = mye_trs_matrix((Vector3){ 10.0f, 0.0f, 0.0f },
                              QuaternionFromAxisAngle(
                                  (Vector3){ 0.0f, 0.0f, 1.0f }, PI * 0.5f),
                              (Vector3){ 2.0f, 2.0f, 2.0f });

    Vector3 p = Vector3Transform((Vector3){ 1.0f, 0.0f, 0.0f }, m);
    assert_near_v3(T, (Vector3){ 10.0f, 2.0f, 0.0f }, p, 1e-4f);

    /* The translation column is the origin's image. */
    assert_near_v3(T, (Vector3){ 10.0f, 0.0f, 0.0f },
                   mye_matrix_translation(m), 1e-4f);
}

TEST(identity_trs_is_identity)
{
    Matrix m = mye_trs_matrix((Vector3){ 0.0f, 0.0f, 0.0f },
                              QuaternionIdentity(),
                              (Vector3){ 1.0f, 1.0f, 1.0f });
    Vector3 p = Vector3Transform((Vector3){ 3.0f, -4.0f, 5.0f }, m);
    assert_near_v3(T, (Vector3){ 3.0f, -4.0f, 5.0f }, p, 1e-5f);
}

/* ------------------------------------------------------------- hierarchy -- */

TEST(a_root_entity_lands_at_its_own_position)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t e = mye_spawn_3d(world, (Vector3){ 5.0f, 6.0f, 7.0f });
    mye_progress(world, FIXED_DT);

    assert_near_v3(T, (Vector3){ 5.0f, 6.0f, 7.0f },
                   mye_world_position(world, e), 1e-4f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_child_is_positioned_relative_to_its_parent)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t parent = mye_spawn_3d(world, (Vector3){ 10.0f, 0.0f, 0.0f });
    ecs_entity_t child = mye_spawn_3d(world, (Vector3){ 0.0f, 5.0f, 0.0f });
    mye_set_parent(world, child, parent);

    mye_progress(world, FIXED_DT);

    /* The child's own position is an offset from the parent. */
    assert_near_v3(T, (Vector3){ 10.0f, 5.0f, 0.0f },
                   mye_world_position(world, child), 1e-4f);

    /* Move the parent: the child follows without touching its own position. */
    ecs_set(world, parent, MyePosition3D, { { -3.0f, 1.0f, 2.0f } });
    mye_progress(world, FIXED_DT);
    assert_near_v3(T, (Vector3){ -3.0f, 6.0f, 2.0f },
                   mye_world_position(world, child), 1e-4f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(parent_rotation_carries_children_around_it)
{
    /* The turret-on-a-tank case: rotating the parent must orbit the child,
     * not merely translate it. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t parent = mye_spawn_3d(world, (Vector3){ 0.0f, 0.0f, 0.0f });
    ecs_entity_t child = mye_spawn_3d(world, (Vector3){ 4.0f, 0.0f, 0.0f });
    mye_set_parent(world, child, parent);

    /* Rotate the parent 90 degrees about Y: +X swings to -Z. */
    ecs_set(world, parent, MyeRotation3D,
            { QuaternionFromAxisAngle((Vector3){ 0.0f, 1.0f, 0.0f },
                                      PI * 0.5f) });
    mye_progress(world, FIXED_DT);

    assert_near_v3(T, (Vector3){ 0.0f, 0.0f, -4.0f },
                   mye_world_position(world, child), 1e-3f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(parent_scale_multiplies_child_offsets)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t parent = mye_spawn_3d(world, (Vector3){ 0.0f, 0.0f, 0.0f });
    ecs_entity_t child = mye_spawn_3d(world, (Vector3){ 2.0f, 0.0f, 0.0f });
    mye_set_parent(world, child, parent);

    ecs_set(world, parent, MyeScale3D, { { 3.0f, 3.0f, 3.0f } });
    mye_progress(world, FIXED_DT);

    assert_near_v3(T, (Vector3){ 6.0f, 0.0f, 0.0f },
                   mye_world_position(world, child), 1e-4f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(deep_chains_resolve_in_a_single_frame)
{
    /* The reason the query uses EcsCascade. Without breadth-first ordering a
     * chain this deep would need one frame per level to settle, and the tip
     * would visibly lag the root. */
    enum { DEPTH = 8 };
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t chain[DEPTH];
    chain[0] = mye_spawn_3d(world, (Vector3){ 1.0f, 0.0f, 0.0f });
    for (int i = 1; i < DEPTH; ++i) {
        chain[i] = mye_spawn_3d(world, (Vector3){ 1.0f, 0.0f, 0.0f });
        mye_set_parent(world, chain[i], chain[i - 1]);
    }

    /* One frame only. */
    mye_progress(world, FIXED_DT);

    for (int i = 0; i < DEPTH; ++i) {
        assert_near_v3(T, (Vector3){ (float)(i + 1), 0.0f, 0.0f },
                       mye_world_position(world, chain[i]), 1e-4f);
    }

    /* Moving the root propagates all the way down, also in one frame. */
    ecs_set(world, chain[0], MyePosition3D, { { 100.0f, 0.0f, 0.0f } });
    mye_progress(world, FIXED_DT);
    assert_near_v3(T, (Vector3){ 100.0f + (float)(DEPTH - 1), 0.0f, 0.0f },
                   mye_world_position(world, chain[DEPTH - 1]), 1e-4f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(reparenting_moves_an_entity_between_frames_of_reference)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t a = mye_spawn_3d(world, (Vector3){ 10.0f, 0.0f, 0.0f });
    ecs_entity_t b = mye_spawn_3d(world, (Vector3){ -10.0f, 0.0f, 0.0f });
    ecs_entity_t child = mye_spawn_3d(world, (Vector3){ 1.0f, 0.0f, 0.0f });

    mye_set_parent(world, child, a);
    mye_progress(world, FIXED_DT);
    assert_near_v3(T, (Vector3){ 11.0f, 0.0f, 0.0f },
                   mye_world_position(world, child), 1e-4f);

    /* Same local offset, different parent. */
    mye_set_parent(world, child, b);
    mye_progress(world, FIXED_DT);
    assert_near_v3(T, (Vector3){ -9.0f, 0.0f, 0.0f },
                   mye_world_position(world, child), 1e-4f);

    /* Detaching leaves it at its own local position. */
    mye_set_parent(world, child, 0);
    mye_progress(world, FIXED_DT);
    assert_near_v3(T, (Vector3){ 1.0f, 0.0f, 0.0f },
                   mye_world_position(world, child), 1e-4f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(deleting_a_parent_deletes_its_children)
{
    /* flecs' own EcsChildOf cleanup, which the engine deliberately relies on
     * rather than reimplementing: a scene root can be deleted as a unit. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t parent = mye_spawn_3d(world, (Vector3){ 0.0f, 0.0f, 0.0f });
    ecs_entity_t child = mye_spawn_3d(world, (Vector3){ 1.0f, 0.0f, 0.0f });
    ecs_entity_t grandchild = mye_spawn_3d(world, (Vector3){ 1.0f, 0.0f, 0.0f });
    mye_set_parent(world, child, parent);
    mye_set_parent(world, grandchild, child);
    mye_progress(world, FIXED_DT);

    ASSERT_TRUE(ecs_is_alive(world, grandchild));
    ecs_delete(world, parent);

    ASSERT_FALSE(ecs_is_alive(world, child));
    ASSERT_FALSE(ecs_is_alive(world, grandchild));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(two_dimensional_entities_can_be_parented_too)
{
    /* The hierarchy is shared: a 2D entity gets a world matrix from its 2D
     * placement, with rotation about Z. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t parent = ecs_new(world);
    ecs_set(world, parent, MyePosition2D, { 100.0f, 50.0f });
    ecs_set(world, parent, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, parent, MyeWorldTransform, { MatrixIdentity() });

    ecs_entity_t child = ecs_new(world);
    ecs_set(world, child, MyePosition2D, { 10.0f, 0.0f });
    ecs_set(world, child, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, child, MyeWorldTransform, { MatrixIdentity() });
    mye_set_parent(world, child, parent);

    mye_progress(world, FIXED_DT);

    Vector3 p = mye_world_position(world, child);
    ASSERT_NEAR(110.0, p.x, 1e-4);
    ASSERT_NEAR(50.0, p.y, 1e-4);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(trs_applies_scale_then_rotation_then_translation),
          TEST_CASE(identity_trs_is_identity),
          TEST_CASE(a_root_entity_lands_at_its_own_position),
          TEST_CASE(a_child_is_positioned_relative_to_its_parent),
          TEST_CASE(parent_rotation_carries_children_around_it),
          TEST_CASE(parent_scale_multiplies_child_offsets),
          TEST_CASE(deep_chains_resolve_in_a_single_frame),
          TEST_CASE(reparenting_moves_an_entity_between_frames_of_reference),
          TEST_CASE(deleting_a_parent_deletes_its_children),
          TEST_CASE(two_dimensional_entities_can_be_parented_too))
