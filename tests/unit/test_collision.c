/* Unit tests for the pure half of the collision module: overlap maths and
 * the layer rule. No world, no entities, no window -- see plan/09-testing.md.
 *
 * The two rules every test below is really checking:
 *   touching exactly is NOT overlapping (the test is strict);
 *   containment IS overlapping, at any size, down to zero.
 */
#include "collision/collision.h"
#include "mye_test.h"

static MyeCircle circle(float x, float y, float r)
{
    return (MyeCircle){ .center = { x, y }, .radius = r };
}

static MyeAabb box(float x, float y, float hx, float hy)
{
    return (MyeAabb){ .center = { x, y }, .half_extents = { hx, hy } };
}

/* ---------------------------------------------------------- circle/circle -- */

TEST(circles_overlap_when_closer_than_the_sum_of_their_radii)
{
    ASSERT_TRUE(mye_overlap_circle_circle(circle(0, 0, 1), circle(1.5f, 0, 1),
                                          NULL));
    ASSERT_FALSE(mye_overlap_circle_circle(circle(0, 0, 1), circle(3, 0, 1),
                                           NULL));
}

TEST(circles_exactly_touching_do_not_overlap)
{
    /* r1 + r2 == distance. A row of tiles laid edge to edge must not report a
     * collision per seam, every step, for ever. */
    ASSERT_FALSE(mye_overlap_circle_circle(circle(0, 0, 1), circle(2, 0, 1),
                                           NULL));
    ASSERT_FALSE(mye_overlap_circle_circle(circle(0, 0, 2.5f),
                                           circle(0, 4, 1.5f), NULL));
}

TEST(a_circle_contained_in_another_overlaps_it)
{
    MyeOverlap info;
    ASSERT_TRUE(mye_overlap_circle_circle(circle(0, 0, 10), circle(1, 0, 2),
                                          &info));
    /* Depth is how far to travel to stop overlapping, not how far apart the
     * centres are: 10 + 2 - 1. */
    ASSERT_NEAR(11.0, (double)info.depth, 1e-5);
    ASSERT_NEAR(1.0, (double)info.normal.x, 1e-5);
    ASSERT_NEAR(0.0, (double)info.normal.y, 1e-5);
}

TEST(zero_size_circles_touch_but_do_not_overlap)
{
    /* Two points at the same place are touching, and touching is not
     * overlapping -- the same rule as everywhere else, applied at zero. */
    ASSERT_FALSE(mye_overlap_circle_circle(circle(0, 0, 0), circle(0, 0, 0),
                                           NULL));
    /* A point strictly inside a circle does overlap it. */
    ASSERT_TRUE(mye_overlap_circle_circle(circle(0, 0, 0), circle(0.5f, 0, 1),
                                          NULL));
    /* A point exactly on the rim does not. */
    ASSERT_FALSE(mye_overlap_circle_circle(circle(0, 0, 0), circle(1, 0, 1),
                                           NULL));
}

TEST(concentric_circles_still_report_a_usable_normal)
{
    MyeOverlap info;
    ASSERT_TRUE(mye_overlap_circle_circle(circle(3, 3, 1), circle(3, 3, 2),
                                          &info));
    ASSERT_NEAR(3.0, (double)info.depth, 1e-5);
    /* No direction exists; the contract promises a unit vector anyway. */
    ASSERT_NEAR(1.0, (double)(info.normal.x * info.normal.x +
                              info.normal.y * info.normal.y), 1e-5);
}

TEST(negative_radii_are_treated_as_zero_not_as_reach)
{
    /* Squaring a negative sum would otherwise read as "overlaps everything
     * within |r|". */
    ASSERT_FALSE(mye_overlap_circle_circle(circle(0, 0, -5), circle(3, 0, 0),
                                           NULL));
    ASSERT_TRUE(mye_overlap_circle_circle(circle(0, 0, -5), circle(0.5f, 0, 1),
                                          NULL));
}

TEST(the_circle_overlap_normal_points_from_the_first_to_the_second)
{
    MyeOverlap info;
    ASSERT_TRUE(mye_overlap_circle_circle(circle(0, 0, 1), circle(0, 1.5f, 1),
                                          &info));
    ASSERT_NEAR(0.0, (double)info.normal.x, 1e-5);
    ASSERT_NEAR(1.0, (double)info.normal.y, 1e-5);
    ASSERT_NEAR(0.5, (double)info.depth, 1e-5);

    ASSERT_TRUE(mye_overlap_circle_circle(circle(0, 1.5f, 1), circle(0, 0, 1),
                                          &info));
    ASSERT_NEAR(-1.0, (double)info.normal.y, 1e-5);
}

/* -------------------------------------------------------------- aabb/aabb -- */

TEST(boxes_overlap_when_they_overlap_on_both_axes)
{
    ASSERT_TRUE(mye_overlap_aabb_aabb(box(0, 0, 1, 1), box(1.5f, 0, 1, 1),
                                      NULL));
    /* Overlapping on x alone is not an overlap. */
    ASSERT_FALSE(mye_overlap_aabb_aabb(box(0, 0, 1, 1), box(0.5f, 5, 1, 1),
                                       NULL));
}

TEST(boxes_sharing_an_edge_do_not_overlap)
{
    ASSERT_FALSE(mye_overlap_aabb_aabb(box(0, 0, 1, 1), box(2, 0, 1, 1),
                                       NULL));
    ASSERT_FALSE(mye_overlap_aabb_aabb(box(0, 0, 1, 1), box(0, 2, 1, 1),
                                       NULL));
    /* Corner to corner: still only touching. */
    ASSERT_FALSE(mye_overlap_aabb_aabb(box(0, 0, 1, 1), box(2, 2, 1, 1),
                                       NULL));
}

TEST(a_box_contained_in_another_overlaps_it)
{
    MyeOverlap info;
    ASSERT_TRUE(mye_overlap_aabb_aabb(box(0, 0, 0.5f, 0.5f), box(0, 0, 4, 4),
                                      &info));
    /* Shallowest way out of a concentric containment: half + half. */
    ASSERT_NEAR(4.5, (double)info.depth, 1e-5);
}

TEST(zero_size_boxes_follow_the_same_two_rules)
{
    /* A point strictly inside overlaps. */
    ASSERT_TRUE(mye_overlap_aabb_aabb(box(0, 0, 0, 0), box(0, 0, 1, 1), NULL));
    /* A point exactly on the boundary only touches. */
    ASSERT_FALSE(mye_overlap_aabb_aabb(box(1, 0, 0, 0), box(0, 0, 1, 1),
                                       NULL));
    /* Two coincident points touch. */
    ASSERT_FALSE(mye_overlap_aabb_aabb(box(0, 0, 0, 0), box(0, 0, 0, 0),
                                       NULL));
}

TEST(the_box_overlap_normal_takes_the_shallowest_axis)
{
    MyeOverlap info;
    /* 0.5 deep on x, 1.9 deep on y: out through the side, not the top. */
    ASSERT_TRUE(mye_overlap_aabb_aabb(box(0, 0, 1, 1), box(1.5f, 0.1f, 1, 1),
                                      &info));
    ASSERT_NEAR(1.0, (double)info.normal.x, 1e-5);
    ASSERT_NEAR(0.0, (double)info.normal.y, 1e-5);
    ASSERT_NEAR(0.5, (double)info.depth, 1e-5);

    ASSERT_TRUE(mye_overlap_aabb_aabb(box(0, 0, 1, 1), box(0.1f, -1.5f, 1, 1),
                                      &info));
    ASSERT_NEAR(0.0, (double)info.normal.x, 1e-5);
    ASSERT_NEAR(-1.0, (double)info.normal.y, 1e-5);
    ASSERT_NEAR(0.5, (double)info.depth, 1e-5);
}

/* ------------------------------------------------------------ circle/aabb -- */

TEST(a_circle_overlapping_a_box_face_reports_that_face)
{
    MyeOverlap info;
    ASSERT_TRUE(mye_overlap_circle_aabb(circle(1.5f, 0, 1), box(0, 0, 1, 1),
                                        &info));
    ASSERT_NEAR(0.5, (double)info.depth, 1e-5);
    /* circle -> box, so back towards the box centre. */
    ASSERT_NEAR(-1.0, (double)info.normal.x, 1e-5);
    ASSERT_NEAR(1.0, (double)info.point.x, 1e-5); /* nearest point on the box */
}

TEST(a_circle_touching_a_box_face_does_not_overlap)
{
    ASSERT_FALSE(mye_overlap_circle_aabb(circle(2, 0, 1), box(0, 0, 1, 1),
                                         NULL));
}

TEST(a_circle_near_a_corner_is_not_caught_by_the_bounding_box)
{
    /* The classic false positive: the circle's own bounding box overlaps the
     * box, but the circle does not. Distance to the corner is sqrt(2) > 1. */
    ASSERT_FALSE(mye_overlap_circle_aabb(circle(2, 2, 1), box(0, 0, 1, 1),
                                         NULL));
    /* Move it in far enough and the corner test does fire. */
    MyeOverlap info;
    ASSERT_TRUE(mye_overlap_circle_aabb(circle(1.5f, 1.5f, 1), box(0, 0, 1, 1),
                                        &info));
    ASSERT_NEAR(1.0 - 0.7071068, (double)info.depth, 1e-5);
}

TEST(a_circle_centred_inside_a_box_overlaps_it_at_any_radius)
{
    MyeOverlap info;
    /* Wholly swallowed: the nearest way out is 0.5 of box plus the radius. */
    ASSERT_TRUE(mye_overlap_circle_aabb(circle(0.5f, 0, 0.1f), box(0, 0, 1, 4),
                                        &info));
    ASSERT_NEAR(0.6, (double)info.depth, 1e-5);
    ASSERT_NEAR(-1.0, (double)info.normal.x, 1e-5);

    /* A zero-radius circle -- a point -- strictly inside still overlaps, the
     * same as a zero-size box does. */
    ASSERT_TRUE(mye_overlap_circle_aabb(circle(0.5f, 0, 0), box(0, 0, 1, 1),
                                        NULL));
    /* ...and exactly on the boundary it only touches. */
    ASSERT_FALSE(mye_overlap_circle_aabb(circle(1, 0, 0), box(0, 0, 1, 1),
                                         NULL));
    /* A circle with any radius sitting on the boundary does overlap. */
    ASSERT_TRUE(mye_overlap_circle_aabb(circle(1, 0, 0.25f), box(0, 0, 1, 1),
                                        NULL));
}

TEST(a_zero_size_box_is_still_a_valid_target)
{
    ASSERT_TRUE(mye_overlap_circle_aabb(circle(0, 0, 1), box(0.5f, 0, 0, 0),
                                        NULL));
    ASSERT_FALSE(mye_overlap_circle_aabb(circle(0, 0, 1), box(1, 0, 0, 0),
                                         NULL));
}

/* -------------------------------------------------------- layers and mask -- */

TEST(a_layer_pair_matches_when_either_side_is_interested)
{
    uint32_t bullet = MYE_LAYER(0);
    uint32_t rock = MYE_LAYER(1);

    /* The bullet asks about rocks; the rock asks about nothing. One side
     * asking is enough -- the rule is symmetric so a game never has to set
     * the relationship up twice. */
    ASSERT_TRUE(mye_collision_layers_match(bullet, rock, rock, MYE_LAYER_NONE));
    ASSERT_TRUE(mye_collision_layers_match(rock, MYE_LAYER_NONE, bullet, rock));

    /* Neither side is interested in the other. */
    ASSERT_FALSE(mye_collision_layers_match(bullet, bullet, rock, rock));

    /* Interested in everything. */
    ASSERT_TRUE(mye_collision_layers_match(bullet, MYE_LAYER_ALL, rock,
                                           MYE_LAYER_NONE));
    /* On no layer at all: nobody can name it, so nothing matches. */
    ASSERT_FALSE(mye_collision_layers_match(MYE_LAYER_NONE, MYE_LAYER_ALL,
                                            MYE_LAYER_NONE, MYE_LAYER_ALL));
}

TEST(layer_bits_reach_the_top_of_the_word)
{
    ASSERT_TRUE(mye_collision_layers_match(MYE_LAYER(31), MYE_LAYER_ALL,
                                           MYE_LAYER(31), MYE_LAYER_NONE));
    ASSERT_FALSE(mye_collision_layers_match(MYE_LAYER(31), MYE_LAYER(30),
                                            MYE_LAYER(29), MYE_LAYER(28)));
}

/* --------------------------------------------------- collider dispatching -- */

TEST(collider_overlap_dispatches_on_the_shape_pair)
{
    MyeCollider2D c = { .shape = MYE_COLLIDER_CIRCLE, .radius = 1.0f };
    MyeCollider2D b = { .shape = MYE_COLLIDER_AABB,
                        .half_extents = { 1.0f, 1.0f } };

    ASSERT_TRUE(mye_collider_overlap(&c, (Vector2){ 0, 0 }, &c,
                                     (Vector2){ 1.5f, 0 }, NULL));
    ASSERT_TRUE(mye_collider_overlap(&b, (Vector2){ 0, 0 }, &b,
                                     (Vector2){ 1.5f, 0 }, NULL));
    ASSERT_TRUE(mye_collider_overlap(&c, (Vector2){ 0, 0 }, &b,
                                     (Vector2){ 1.5f, 0 }, NULL));
    ASSERT_TRUE(mye_collider_overlap(&b, (Vector2){ 0, 0 }, &c,
                                     (Vector2){ 1.5f, 0 }, NULL));
}

TEST(swapping_the_colliders_flips_the_normal_and_nothing_else)
{
    MyeCollider2D c = { .shape = MYE_COLLIDER_CIRCLE, .radius = 1.0f };
    MyeCollider2D b = { .shape = MYE_COLLIDER_AABB,
                        .half_extents = { 1.0f, 1.0f } };

    MyeOverlap circle_first;
    MyeOverlap box_first;
    ASSERT_TRUE(mye_collider_overlap(&c, (Vector2){ 1.5f, 0 }, &b,
                                     (Vector2){ 0, 0 }, &circle_first));
    ASSERT_TRUE(mye_collider_overlap(&b, (Vector2){ 0, 0 }, &c,
                                     (Vector2){ 1.5f, 0 }, &box_first));

    ASSERT_NEAR((double)circle_first.depth, (double)box_first.depth, 1e-6);
    ASSERT_NEAR(-1.0, (double)circle_first.normal.x, 1e-5);
    ASSERT_NEAR(1.0, (double)box_first.normal.x, 1e-5);
}

TEST(the_collider_offset_moves_the_shape_not_the_entity)
{
    MyeCollider2D head = { .shape = MYE_COLLIDER_CIRCLE,
                           .radius = 1.0f,
                           .offset = { 0.0f, 10.0f } };
    MyeCollider2D pebble = { .shape = MYE_COLLIDER_CIRCLE, .radius = 1.0f };

    /* Both entities stand at the origin; only the offset separates them. */
    ASSERT_FALSE(mye_collider_overlap(&head, (Vector2){ 0, 0 }, &pebble,
                                      (Vector2){ 0, 0 }, NULL));
    ASSERT_TRUE(mye_collider_overlap(&head, (Vector2){ 0, 0 }, &pebble,
                                     (Vector2){ 0, 9.5f }, NULL));
}

TEST_MAIN(TEST_CASE(circles_overlap_when_closer_than_the_sum_of_their_radii),
          TEST_CASE(circles_exactly_touching_do_not_overlap),
          TEST_CASE(a_circle_contained_in_another_overlaps_it),
          TEST_CASE(zero_size_circles_touch_but_do_not_overlap),
          TEST_CASE(concentric_circles_still_report_a_usable_normal),
          TEST_CASE(negative_radii_are_treated_as_zero_not_as_reach),
          TEST_CASE(the_circle_overlap_normal_points_from_the_first_to_the_second),
          TEST_CASE(boxes_overlap_when_they_overlap_on_both_axes),
          TEST_CASE(boxes_sharing_an_edge_do_not_overlap),
          TEST_CASE(a_box_contained_in_another_overlaps_it),
          TEST_CASE(zero_size_boxes_follow_the_same_two_rules),
          TEST_CASE(the_box_overlap_normal_takes_the_shallowest_axis),
          TEST_CASE(a_circle_overlapping_a_box_face_reports_that_face),
          TEST_CASE(a_circle_touching_a_box_face_does_not_overlap),
          TEST_CASE(a_circle_near_a_corner_is_not_caught_by_the_bounding_box),
          TEST_CASE(a_circle_centred_inside_a_box_overlaps_it_at_any_radius),
          TEST_CASE(a_zero_size_box_is_still_a_valid_target),
          TEST_CASE(a_layer_pair_matches_when_either_side_is_interested),
          TEST_CASE(layer_bits_reach_the_top_of_the_word),
          TEST_CASE(collider_overlap_dispatches_on_the_shape_pair),
          TEST_CASE(swapping_the_colliders_flips_the_normal_and_nothing_else),
          TEST_CASE(the_collider_offset_moves_the_shape_not_the_entity))
