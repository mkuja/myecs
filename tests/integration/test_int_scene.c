/* Integration tests for the scene system. See plan/07-roadmap.md (M6).
 *
 * The properties that matter: a scene owns what it creates, unloading takes
 * exactly that away, shared assets survive, and switching back and forth
 * many times leaks nothing. Headless. */
#include "asset/asset.h"
#include "core/engine.h"
#include "mye_test.h"
#include "render/render2d.h"
#include "scene/scene.h"

#include <raylib.h>

#define FIXED_DT (1.0f / 60.0f)

typedef struct Marker {
    int scene_id;
} Marker;

ECS_COMPONENT_DECLARE(Marker);

static int load_calls_a;
static int load_calls_b;
static int unload_calls_a;

static ecs_world_t *make_world(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .headless = true});
    if (world != NULL) {
        ECS_COMPONENT_DEFINE(world, Marker);
    }
    return world;
}

static int count_markers(ecs_world_t *world)
{
    ecs_query_t *q = ecs_query(world, {
        .terms = {{ .id = ecs_id(Marker) }},
        .cache_kind = EcsQueryCacheNone,
    });
    if (q == NULL) {
        return -1;
    }
    int total = 0;
    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
        total += it.count;
    }
    ecs_query_fini(q);
    return total;
}

/* Scene A: three entities and a texture of its own. */
static void load_a(ecs_world_t *world, void *user)
{
    (void)user;
    ++load_calls_a;
    for (int i = 0; i < 3; ++i) {
        ecs_entity_t e = mye_entity_new(world);
        ecs_set(world, e, Marker, { 1 });
    }
    mye_texture_from_image(world, "scene_a_only",
                           GenImageColor(4, 4, RED));
    mye_texture_from_image(world, "shared", GenImageColor(4, 4, BLUE));
}

static void unload_a(ecs_world_t *world, void *user)
{
    (void)world;
    (void)user;
    ++unload_calls_a;
}

/* Scene B: five entities, and it also wants the shared texture. */
static void load_b(ecs_world_t *world, void *user)
{
    (void)user;
    ++load_calls_b;
    for (int i = 0; i < 5; ++i) {
        ecs_entity_t e = mye_entity_new(world);
        ecs_set(world, e, Marker, { 2 });
    }
    mye_texture_from_image(world, "shared", GenImageColor(4, 4, BLUE));
}

static void register_both(ecs_world_t *world)
{
    mye_scene_register(world, &(mye_scene_desc){
        .name = "a", .load = load_a, .unload = unload_a });
    mye_scene_register(world, &(mye_scene_desc){
        .name = "b", .load = load_b });
}

/* --------------------------------------------------------- registration -- */

TEST(registration_rejects_duplicates_and_nonsense)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ASSERT_TRUE(mye_scene_register(world, &(mye_scene_desc){
                    .name = "a", .load = load_a }) != 0);
    /* Same name twice would make "switch to a" ambiguous. */
    ASSERT_EQ_U64(0, mye_scene_register(world, &(mye_scene_desc){
                          .name = "a", .load = load_b }));
    ASSERT_EQ_U64(0, mye_scene_register(world, &(mye_scene_desc){
                          .name = NULL, .load = load_a }));
    ASSERT_EQ_U64(0, mye_scene_register(world, NULL));

    ASSERT_NULL(mye_scene_current(world)); /* registering does not activate */
    ASSERT_FALSE(mye_scene_switch(world, "nope"));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* ------------------------------------------------------------ switching -- */

TEST(switching_is_deferred_to_the_frame_boundary)
{
    /* Applying a switch mid-frame would delete entities out from under
     * systems that are iterating them. */
    load_calls_a = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    register_both(world);

    ASSERT_TRUE(mye_scene_switch(world, "a"));
    ASSERT_TRUE(mye_scene_switch_pending(world));
    ASSERT_EQ_INT(0, load_calls_a);   /* not yet */
    ASSERT_NULL(mye_scene_current(world));

    mye_progress(world, FIXED_DT);

    ASSERT_FALSE(mye_scene_switch_pending(world));
    ASSERT_EQ_INT(1, load_calls_a);
    ASSERT_STR_EQ("a", mye_scene_current(world));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_scene_owns_the_entities_it_creates)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    register_both(world);

    mye_scene_switch(world, "a");
    mye_progress(world, FIXED_DT);

    ASSERT_EQ_INT(3, count_markers(world));
    ASSERT_EQ_INT(3, mye_scene_entity_count(world));

    /* Switching away deletes exactly scene A's entities and creates B's --
     * no manual bookkeeping in either load function. */
    mye_scene_switch(world, "b");
    mye_progress(world, FIXED_DT);

    ASSERT_STR_EQ("b", mye_scene_current(world));
    ASSERT_EQ_INT(5, count_markers(world));
    ASSERT_EQ_INT(5, mye_scene_entity_count(world));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(entities_spawned_during_play_belong_to_the_scene_too)
{
    /* Not just what the load callback made: anything created while the scene
     * is active is owned by it, which is what makes bullets and enemies
     * disappear correctly on a level change. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    register_both(world);

    mye_scene_switch(world, "a");
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(3, count_markers(world));

    for (int i = 0; i < 4; ++i) {
        ecs_entity_t e = mye_entity_new(world);
        ecs_set(world, e, Marker, { 1 });
    }
    ASSERT_EQ_INT(7, count_markers(world));

    mye_scene_switch(world, "b");
    mye_progress(world, FIXED_DT);

    ASSERT_EQ_INT(5, count_markers(world)); /* all seven went away */

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(the_unload_callback_runs_before_entities_are_deleted)
{
    unload_calls_a = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    register_both(world);

    mye_scene_switch(world, "a");
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(0, unload_calls_a);

    mye_scene_switch(world, "b");
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, unload_calls_a);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(reload_rebuilds_the_same_scene)
{
    load_calls_a = 0;
    unload_calls_a = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    register_both(world);

    mye_scene_switch(world, "a");
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, load_calls_a);

    /* Add some mess, then reload: the mess goes, the scene comes back fresh. */
    for (int i = 0; i < 10; ++i) {
        ecs_entity_t e = mye_entity_new(world);
        ecs_set(world, e, Marker, { 1 });
    }
    ASSERT_EQ_INT(13, count_markers(world));

    ASSERT_TRUE(mye_scene_reload(world));
    mye_progress(world, FIXED_DT);

    ASSERT_EQ_INT(2, load_calls_a);
    ASSERT_EQ_INT(1, unload_calls_a);
    ASSERT_EQ_INT(3, count_markers(world));
    ASSERT_STR_EQ("a", mye_scene_current(world));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(switching_to_the_active_scene_does_nothing)
{
    load_calls_a = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    register_both(world);

    mye_scene_switch(world, "a");
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, load_calls_a);

    mye_scene_switch(world, "a"); /* already there */
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, load_calls_a); /* not reloaded */

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --------------------------------------------------------------- assets -- */

TEST(scene_assets_are_released_but_shared_ones_survive)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    register_both(world);

    mye_scene_switch(world, "a");
    mye_progress(world, FIXED_DT);

    mye_asset_stats stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(2, (int)stats.textures_live); /* scene_a_only + shared */

    /* B also asks for "shared", so it must outlive A's unload while A's
     * private texture is freed. */
    mye_scene_switch(world, "b");
    mye_progress(world, FIXED_DT);

    stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(1, (int)stats.textures_live);

    /* And the survivor is the shared one: asking again is a dedupe hit, not
     * a fresh load. */
    mye_texture again = mye_texture_from_image(world, "shared",
                                               GenImageColor(4, 4, BLUE));
    ASSERT_TRUE(mye_texture_valid(world, again));
    stats = mye_asset_stats_get(world);
    ASSERT_EQ_INT(1, (int)stats.textures_live);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(many_switches_leave_nothing_behind)
{
    /* The real test of ownership: cycle scenes repeatedly and check that
     * neither entities nor assets accumulate. A missing cleanup shows up as
     * steady growth rather than an immediate failure. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    register_both(world);

    for (int i = 0; i < 20; ++i) {
        mye_scene_switch(world, (i % 2) == 0 ? "a" : "b");
        mye_progress(world, FIXED_DT);

        /* Some gameplay churn each round. */
        for (int j = 0; j < 5; ++j) {
            ecs_entity_t e = mye_entity_new(world);
            ecs_set(world, e, Marker, { 9 });
        }
    }

    /* Final state is one scene's worth plus this round's churn -- not 20
     * rounds' worth. */
    ASSERT_TRUE(count_markers(world) <= 10);

    mye_asset_stats stats = mye_asset_stats_get(world);
    ASSERT_TRUE(stats.textures_live <= 2);

    /* And zero leaks overall, which covers everything flecs allocated too. */
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(raw_ecs_new_escapes_scene_ownership)
{
    /* Documenting a sharp edge rather than pretending it does not exist:
     * ecs_new() bypasses flecs' ecs_set_with, so an entity created that way
     * is NOT owned by the scene and survives a switch. mye_entity_new() is
     * the one to use; every engine spawn helper already does. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    register_both(world);

    mye_scene_switch(world, "a");
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(3, count_markers(world));

    ecs_entity_t owned = mye_entity_new(world);
    ecs_set(world, owned, Marker, { 1 });
    ecs_entity_t stray = ecs_new(world); /* escapes the scene */
    ecs_set(world, stray, Marker, { 1 });
    ASSERT_EQ_INT(5, count_markers(world));

    mye_scene_switch(world, "b");
    mye_progress(world, FIXED_DT);

    /* Scene B's five, plus the stray that was never owned. */
    ASSERT_EQ_INT(6, count_markers(world));
    ASSERT_TRUE(ecs_is_alive(world, stray));
    ASSERT_FALSE(ecs_is_alive(world, owned));

    ecs_delete(world, stray);
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(registration_rejects_duplicates_and_nonsense),
          TEST_CASE(raw_ecs_new_escapes_scene_ownership),
          TEST_CASE(switching_is_deferred_to_the_frame_boundary),
          TEST_CASE(a_scene_owns_the_entities_it_creates),
          TEST_CASE(entities_spawned_during_play_belong_to_the_scene_too),
          TEST_CASE(the_unload_callback_runs_before_entities_are_deleted),
          TEST_CASE(reload_rebuilds_the_same_scene),
          TEST_CASE(switching_to_the_active_scene_does_nothing),
          TEST_CASE(scene_assets_are_released_but_shared_ones_survive),
          TEST_CASE(many_switches_leave_nothing_behind))
