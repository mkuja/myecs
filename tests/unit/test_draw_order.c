/* Draw-list sort order: the pure comparator the sprite pass hands to qsort.
 *
 * This is the "render logic" row of plan/09-testing.md -- draw-list sorting
 * as a pure function, no window and no GL. The comparator is exposed through
 * the testing seam in render/render2d.h.
 *
 * The order is back-to-front: layer, then dest.y, then texture address. Layer
 * is the one a game controls directly, and the one whose failure looks like
 * "my HUD is behind the world"; y is the top-down depth illusion; texture is
 * only a batching hint and must never outrank the other two. */
#include "render/render2d.h"
#include "mye_test.h"

#include <stdlib.h>

/* Stand-ins for real textures: the comparator only ever compares their
 * addresses, so two distinct objects are all it takes. Distinct initialisers
 * keep them genuinely distinct objects. Which address is lower is not
 * knowable, so no test asserts a direction for the texture tie-break -- only
 * that it is consistent. */
static Texture2D texture_a = { .id = 1, .width = 8, .height = 8 };
static Texture2D texture_b = { .id = 2, .width = 8, .height = 8 };

static MyeDrawItem item(int32_t layer, float y, const Texture2D *texture)
{
    return (MyeDrawItem){
        .texture = texture,
        .dest = { 0.0f, y, 8.0f, 8.0f },
        .tint = WHITE,
        .layer = layer,
    };
}

/* Sign of the comparator, so the tests read as "a before b". */
static int cmp(MyeDrawItem a, MyeDrawItem b)
{
    int r = mye_draw_item_compare(&a, &b);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

TEST(layer_outranks_everything_else)
{
    /* Higher layer draws in front, i.e. later in the list. */
    ASSERT_EQ_INT(-1, cmp(item(0, 0.0f, &texture_a), item(1, 0.0f, &texture_a)));
    ASSERT_EQ_INT(1, cmp(item(1, 0.0f, &texture_a), item(0, 0.0f, &texture_a)));

    /* ...even when y and texture both argue the other way. */
    ASSERT_EQ_INT(-1, cmp(item(0, 999.0f, &texture_b),
                          item(1, -999.0f, &texture_a)));
    ASSERT_EQ_INT(1, cmp(item(1, -999.0f, &texture_a),
                         item(0, 999.0f, &texture_b)));

    /* Negative layers sort below zero, not by magnitude. */
    ASSERT_EQ_INT(-1, cmp(item(-1, 0.0f, &texture_a),
                          item(0, 0.0f, &texture_a)));
    ASSERT_EQ_INT(-1, cmp(item(-5, 0.0f, &texture_a),
                          item(-1, 0.0f, &texture_a)));
}

TEST(within_a_layer_lower_on_screen_draws_in_front)
{
    /* +y is down, so the larger y draws later and overlaps the smaller. */
    ASSERT_EQ_INT(-1, cmp(item(3, 10.0f, &texture_a),
                          item(3, 20.0f, &texture_a)));
    ASSERT_EQ_INT(1, cmp(item(3, 20.0f, &texture_a),
                         item(3, 10.0f, &texture_a)));

    /* y beats texture, whichever way the addresses happen to fall. */
    ASSERT_EQ_INT(-1, cmp(item(3, 10.0f, &texture_b),
                          item(3, 20.0f, &texture_a)));
    ASSERT_EQ_INT(-1, cmp(item(3, 10.0f, &texture_a),
                          item(3, 20.0f, &texture_b)));

    /* Negative and fractional y are ordinary values, not special cases. */
    ASSERT_EQ_INT(-1, cmp(item(3, -0.5f, &texture_a),
                          item(3, 0.0f, &texture_a)));
    ASSERT_EQ_INT(-1, cmp(item(3, 0.25f, &texture_a),
                          item(3, 0.5f, &texture_a)));
}

TEST(equal_layer_and_y_group_by_texture)
{
    /* The tie-break exists only so raylib can batch identical textures. It
     * has no meaning of its own, so the assertion is that the two orderings
     * disagree -- whichever way the addresses fall. */
    int forward = cmp(item(0, 0.0f, &texture_a), item(0, 0.0f, &texture_b));
    int backward = cmp(item(0, 0.0f, &texture_b), item(0, 0.0f, &texture_a));
    ASSERT_TRUE(forward != 0);
    ASSERT_EQ_INT(-forward, backward);
}

TEST(items_equal_on_all_three_keys_compare_equal)
{
    /* Everything else about the item -- source rect, origin, rotation, tint --
     * is irrelevant to the order. Two items that differ only there must tie,
     * or the sort would shuffle by a field nothing means to sort on. */
    MyeDrawItem a = item(2, 7.0f, &texture_a);
    MyeDrawItem b = item(2, 7.0f, &texture_a);
    b.source = (Rectangle){ 4.0f, 4.0f, 2.0f, 2.0f };
    b.origin = (Vector2){ 1.0f, 1.0f };
    b.rotation_degrees = 90.0f;
    b.tint = RED;
    b.dest.x = 500.0f; /* x is not a sort key: only dest.y is */
    b.dest.width = 64.0f;

    ASSERT_EQ_INT(0, cmp(a, b));
    ASSERT_EQ_INT(0, cmp(b, a));
    ASSERT_EQ_INT(0, cmp(a, a));
}

/* The comparator's contract, checked over the whole cross product: it must be
 * antisymmetric and transitive, or qsort's behaviour is undefined. */
TEST(the_comparator_is_a_total_order)
{
    static const int32_t layers[] = { -1, 0, 1 };
    static const float ys[] = { -2.0f, 0.0f, 5.0f };
    const Texture2D *textures[] = { &texture_a, &texture_b };

    MyeDrawItem all[3 * 3 * 2];
    size_t n = 0;
    for (size_t l = 0; l < 3; ++l) {
        for (size_t y = 0; y < 3; ++y) {
            for (size_t t = 0; t < 2; ++t) {
                all[n++] = item(layers[l], ys[y], textures[t]);
            }
        }
    }
    ASSERT_EQ_U64(sizeof all / sizeof all[0], n);

    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            ASSERT_EQ_INT(-cmp(all[i], all[j]), cmp(all[j], all[i]));
            for (size_t k = 0; k < n; ++k) {
                if (cmp(all[i], all[j]) < 0 && cmp(all[j], all[k]) < 0) {
                    ASSERT_TRUE(cmp(all[i], all[k]) < 0);
                }
            }
        }
    }
}

/* End to end through qsort, the way the sprite pass uses it: a deliberately
 * scrambled list must come out layer-major, y-minor, with the tied pair
 * adjacent so the batch survives. */
TEST(qsort_puts_a_scrambled_list_back_to_front)
{
    MyeDrawItem items[] = {
        item(1, 50.0f, &texture_a),  /* 5 */
        item(0, 90.0f, &texture_a),  /* 3 */
        item(-2, 0.0f, &texture_b),  /* 0 */
        item(1, 10.0f, &texture_a),  /* 4 */
        item(0, 10.0f, &texture_a),  /* 1 or 2 */
        item(0, 10.0f, &texture_b),  /* 1 or 2 */
    };
    const size_t n = sizeof items / sizeof items[0];

    qsort(items, n, sizeof items[0], mye_draw_item_compare);

    /* Sorted: every adjacent pair is in order. */
    for (size_t i = 0; i + 1 < n; ++i) {
        ASSERT_TRUE(mye_draw_item_compare(&items[i], &items[i + 1]) <= 0);
    }

    /* And the keys landed where they should, spelled out rather than
     * re-derived from the comparator under test. */
    ASSERT_EQ_INT(-2, items[0].layer);
    ASSERT_EQ_INT(0, items[1].layer);
    ASSERT_NEAR(10.0, items[1].dest.y, 1e-6);
    ASSERT_EQ_INT(0, items[2].layer);
    ASSERT_NEAR(10.0, items[2].dest.y, 1e-6);
    /* The two textures at the same layer and y are adjacent: that is the
     * batch the texture tie-break exists to preserve. */
    ASSERT_TRUE(items[1].texture != items[2].texture);
    ASSERT_EQ_INT(0, items[3].layer);
    ASSERT_NEAR(90.0, items[3].dest.y, 1e-6);
    ASSERT_EQ_INT(1, items[4].layer);
    ASSERT_NEAR(10.0, items[4].dest.y, 1e-6);
    ASSERT_EQ_INT(1, items[5].layer);
    ASSERT_NEAR(50.0, items[5].dest.y, 1e-6);
}

TEST_MAIN(TEST_CASE(layer_outranks_everything_else),
          TEST_CASE(within_a_layer_lower_on_screen_draws_in_front),
          TEST_CASE(equal_layer_and_y_group_by_texture),
          TEST_CASE(items_equal_on_all_three_keys_compare_equal),
          TEST_CASE(the_comparator_is_a_total_order),
          TEST_CASE(qsort_puts_a_scrambled_list_back_to_front))
