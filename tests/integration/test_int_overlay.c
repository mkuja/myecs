/* The debug overlay's countable half. See plan/00-overview.md (Tier 1,
 * "entity/system counts"), plan/03-rendering.md (the interpolated-entity
 * line) and plan/07-roadmap.md M7 ("profile first").
 *
 * Drawing cannot be asserted on headlessly, so the overlay keeps its counting
 * in accessors and these tests exercise those. What is being defended here is
 * arithmetic that is otherwise only ever checked by squinting at a panel:
 * a count that is one table short, or one entity short, looks exactly as
 * plausible on screen as a correct one. */
#include "core/engine.h"
#include "debug/overlay.h"
#include "mye_test.h"
#include "render/render2d.h"
#include "scene/transform.h"

#include <raylib.h>

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){ .headless = true });
}

static ecs_entity_t spawn_sprite(ecs_world_t *world, bool interpolated)
{
    ecs_entity_t e = mye_sprite_spawn(world, (mye_texture){ 0 }, 10.0f, 20.0f,
                                      WHITE);
    if (interpolated) {
        ecs_set(world, e, MyeInterpolate, { 0 });
    }
    return e;
}

static void Nothing(ecs_iter_t *it)
{
    (void)it;
}

TEST(spawning_entities_moves_the_entity_count_by_exactly_that_many)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    /* A fresh world is not empty -- flecs registers several hundred entities
     * for itself -- so the assertion is about the delta, which is the number
     * the overlay is actually used to watch. */
    mye_overlay_counts before = mye_overlay_counts_get(world);
    ASSERT_TRUE(before.entities > 0);

    for (int i = 0; i < 7; ++i) {
        spawn_sprite(world, false);
    }

    mye_overlay_counts after = mye_overlay_counts_get(world);
    ASSERT_EQ_INT(before.entities + 7, after.entities);

    /* And back down again: an overlay that only ever counted up would hide
     * the leak it exists to reveal. */
    ecs_entity_t doomed = spawn_sprite(world, false);
    ecs_delete(world, doomed);
    ASSERT_EQ_INT(after.entities, mye_overlay_counts_get(world).entities);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

static void register_system(ecs_world_t *world, const char *name,
                            ecs_entity_t phase)
{
    ecs_system(world, {
        .entity = ecs_entity(world, { .name = name,
                                      .add = ecs_ids(ecs_dependson(phase)) }),
        .query.terms = {{ .id = ecs_id(MyePosition2D) }},
        .callback = Nothing,
    });
}

TEST(registering_systems_moves_the_system_count_by_that_many)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_overlay_counts before = mye_overlay_counts_get(world);
    /* The engine's own systems are already registered, so this is well clear
     * of zero before the test adds anything. */
    ASSERT_TRUE(before.systems > 0);

    /* Two systems in the same phase with the same query share an archetype,
     * so they arrive as one iteration carrying two entities. Adding one per
     * iteration instead of one per entity would pass a test that registered
     * only one system at a time; it does not pass this one. */
    register_system(world, "OverlayTestOnUpdateA", EcsOnUpdate);
    register_system(world, "OverlayTestOnUpdateB", EcsOnUpdate);
    ASSERT_EQ_INT(before.systems + 2, mye_overlay_counts_get(world).systems);

    /* Fixed-step systems live in a pipeline of the engine's own, outside the
     * one ecs_progress() runs. Counting only the main pipeline would miss
     * exactly the systems most likely to need sharding. */
    register_system(world, "OverlayTestFixed", MyeOnFixedUpdate);
    ASSERT_EQ_INT(before.systems + 3, mye_overlay_counts_get(world).systems);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(the_interpolated_count_is_a_fraction_of_what_is_drawn)
{
    /* The line plan/03-rendering.md promises: "alpha 0.37 - interpolated
     * 0/412 entities". Three sprites, one of them smoothed. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    spawn_sprite(world, false);
    spawn_sprite(world, false);
    spawn_sprite(world, true);

    mye_overlay_counts counts = mye_overlay_counts_get(world);
    ASSERT_EQ_INT(3, counts.drawn_2d);
    ASSERT_EQ_INT(1, counts.interpolated);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(both_counts_add_up_across_several_tables)
{
    /* Interpolated and plain sprites land in different tables, and each table
     * is one iteration of the query. Counting tables instead of the entities
     * in them, or adding the whole table to the interpolated total, both
     * survive the 1-of-3 case above; neither survives this one. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    for (int i = 0; i < 4; ++i) {
        spawn_sprite(world, false);
    }
    for (int i = 0; i < 3; ++i) {
        spawn_sprite(world, true);
    }

    mye_overlay_counts counts = mye_overlay_counts_get(world);
    ASSERT_EQ_INT(7, counts.drawn_2d);
    ASSERT_EQ_INT(3, counts.interpolated);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_hidden_sprite_is_not_counted_as_drawn)
{
    /* The counts come from the sprite pass's own query rather than a copy of
     * its terms, so the MyeHidden exclusion is not something the overlay can
     * forget to apply. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    spawn_sprite(world, false);
    ecs_entity_t hidden = spawn_sprite(world, true);
    ecs_add(world, hidden, MyeHidden);

    mye_overlay_counts counts = mye_overlay_counts_get(world);
    ASSERT_EQ_INT(1, counts.drawn_2d);
    ASSERT_EQ_INT(0, counts.interpolated);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(counting_an_empty_world_is_safe)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_overlay_counts counts = mye_overlay_counts_get(world);
    ASSERT_EQ_INT(0, counts.drawn_2d);
    ASSERT_EQ_INT(0, counts.interpolated);

    /* A NULL world is what a caller gets after a failed mye_init; the
     * overlay must not be the thing that turns that into a crash. */
    mye_overlay_counts none = mye_overlay_counts_get(NULL);
    ASSERT_EQ_INT(0, none.entities);
    ASSERT_EQ_INT(0, none.systems);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

static volatile double mye_overlay_test_sink;

/* Deliberately expensive, so it cannot tie with the engine's own systems for
 * the top spot. */
static void Slow(ecs_iter_t *it)
{
    (void)it;
    /* Volatile and read-modify-written, so no optimiser is entitled to
     * delete the work the profile is supposed to measure. */
    for (int i = 0; i < 200000; ++i) {
        mye_overlay_test_sink += (double)i;
    }
}

TEST(the_profile_names_the_slowest_system)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    /* Opening the overlay is what turns flecs' per-system timing on -- a
     * hidden debug tool costs the game nothing. Without this the profile is
     * legitimately all zeros. */
    mye_debug_overlay_show(world, true);

    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "OverlaySlowSystem",
                                      .add = ecs_ids(ecs_dependson(
                                          EcsOnUpdate)) }),
        .query.terms = {{ .id = ecs_id(MyePosition2D) }},
        .callback = Slow,
    });
    spawn_sprite(world, false); /* so the system has something to match */

    for (int i = 0; i < 20; ++i) {
        mye_progress(world, 1.0f / 60.0f);
    }

    mye_overlay_system_time top[MYE_OVERLAY_TOP_SYSTEMS];
    double total = 0.0;
    int count = mye_overlay_system_times(world, top, MYE_OVERLAY_TOP_SYSTEMS,
                                         &total);

#ifdef FLECS_STATS
    /* Debug: the stats addon is compiled in, so "which system should be
     * sharded" has an answer. */
    ASSERT_TRUE(count > 0);
    ASSERT_TRUE(count <= MYE_OVERLAY_TOP_SYSTEMS);
    ASSERT_STR_EQ("OverlaySlowSystem", top[0].name);
    ASSERT_TRUE(top[0].seconds > 0.0);
    /* Worst first, and the total is at least what the listed rows account
     * for -- it sums every system, not only the ones that fit. */
    double listed = 0.0;
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            ASSERT_TRUE(top[i - 1].seconds >= top[i].seconds);
        }
        listed += top[i].seconds;
    }
    ASSERT_TRUE(total >= listed - 1e-9);
#else
    /* Release: FLECS_STATS is not in the binary (see
     * cmake/MyeDependencies.cmake), and the accessor says so with a zero
     * rather than reporting every system as free. */
    ASSERT_EQ_INT(0, count);
    ASSERT_TRUE(total == 0.0);
#endif

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_hidden_overlay_measures_nothing)
{
    /* The house rule the overlay is built to: hidden, it costs the game
     * nothing. Per-system timing is the one part of it that would otherwise
     * charge every frame, so a closed overlay must leave the clocks alone --
     * observable here as a profile that stays at zero however long the world
     * runs. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_debug_overlay_show(world, false); /* explicit: MYE_OVERLAY may be set */

    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "OverlayUnwatchedSystem",
                                      .add = ecs_ids(ecs_dependson(
                                          EcsOnUpdate)) }),
        .query.terms = {{ .id = ecs_id(MyePosition2D) }},
        .callback = Slow,
    });
    spawn_sprite(world, false);

    for (int i = 0; i < 20; ++i) {
        mye_progress(world, 1.0f / 60.0f);
    }

    mye_overlay_system_time top[MYE_OVERLAY_TOP_SYSTEMS];
    double total = -1.0;
    int count = mye_overlay_system_times(world, top, MYE_OVERLAY_TOP_SYSTEMS,
                                         &total);
    ASSERT_TRUE(total == 0.0);
    for (int i = 0; i < count; ++i) {
        ASSERT_TRUE(top[i].seconds == 0.0);
    }

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(the_profile_refuses_a_zero_sized_buffer)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_overlay_system_time one[1];
    ASSERT_EQ_INT(0, mye_overlay_system_times(world, one, 0, NULL));
    ASSERT_EQ_INT(0, mye_overlay_system_times(world, NULL, 1, NULL));
    ASSERT_EQ_INT(0, mye_overlay_system_times(NULL, one, 1, NULL));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(spawning_entities_moves_the_entity_count_by_exactly_that_many),
          TEST_CASE(registering_systems_moves_the_system_count_by_that_many),
          TEST_CASE(the_interpolated_count_is_a_fraction_of_what_is_drawn),
          TEST_CASE(both_counts_add_up_across_several_tables),
          TEST_CASE(a_hidden_sprite_is_not_counted_as_drawn),
          TEST_CASE(counting_an_empty_world_is_safe),
          TEST_CASE(the_profile_names_the_slowest_system),
          TEST_CASE(a_hidden_overlay_measures_nothing),
          TEST_CASE(the_profile_refuses_a_zero_sized_buffer))
