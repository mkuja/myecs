/* Integration tests for scene serialization. See plan/07-roadmap.md (M6).
 *
 * The property that matters is a round trip: save a world, load it into a
 * fresh one, and get the same values back. Headless. */
#include "core/engine.h"
#include "mye_test.h"
#include "render/render2d.h"
#include "scene/scene.h"
#include "scene/serialize.h"
#include "scene/transform.h"

#include <raylib.h>
#include <stdio.h>
#include <string.h>

#define FIXED_DT (1.0f / 60.0f)

typedef struct Health {
    int32_t current;
    int32_t max;
} Health;

/* Deliberately never given reflection data, to test that omission is
 * detectable rather than silent. */
typedef struct Opaque {
    void *pointer;
} Opaque;

ECS_COMPONENT_DECLARE(Health);
ECS_COMPONENT_DECLARE(Opaque);

static char g_path[512];

static ecs_world_t *make_world(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .headless = true});
    if (world == NULL) {
        return NULL;
    }

    ECS_COMPONENT_DEFINE(world, Health);
    ECS_COMPONENT_DEFINE(world, Opaque);

    /* A game describes its own components to flecs; the engine does the same
     * for its own in mye_serialize_register_engine_components. */
    ecs_struct(world, {
        .entity = ecs_id(Health),
        .members = {
            { .name = "current", .type = ecs_id(ecs_i32_t) },
            { .name = "max", .type = ecs_id(ecs_i32_t) },
        }
    });

    return world;
}

TEST(engine_components_have_reflection_data)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ASSERT_TRUE(mye_component_serializable(world, ecs_id(MyePosition2D)));
    ASSERT_TRUE(mye_component_serializable(world, ecs_id(MyePosition3D)));
    ASSERT_TRUE(mye_component_serializable(world, ecs_id(MyeRotation3D)));
    ASSERT_TRUE(mye_component_serializable(world, ecs_id(Health)));

    /* And a component nobody described is reported as not serializable,
     * rather than quietly producing an empty value in the save. */
    ASSERT_FALSE(mye_component_serializable(world, ecs_id(Opaque)));
    ASSERT_FALSE(mye_component_serializable(world, 0));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_world_serializes_to_json_containing_its_values)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t e = ecs_entity(world, { .name = "hero" });
    ecs_set(world, e, MyePosition2D, { 12.5f, -3.25f });
    ecs_set(world, e, Health, { 70, 100 });

    char *json = mye_world_to_json(world);
    ASSERT_NOT_NULL(json);

    /* The entity, both components, and a distinctive value are all present. */
    ASSERT_NOT_NULL(strstr(json, "hero"));
    ASSERT_NOT_NULL(strstr(json, "Health"));
    ASSERT_NOT_NULL(strstr(json, "12.5"));
    ASSERT_NOT_NULL(strstr(json, "70"));

    mye_json_free(world, json);
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(save_and_load_round_trips_through_a_file)
{
    snprintf(g_path, sizeof g_path, "%s/mye_serialize_test.json",
             getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");

    /* --- world one: build and save ----------------------------------- */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t hero = ecs_entity(world, { .name = "hero" });
    ecs_set(world, hero, MyePosition2D, { 100.0f, 200.0f });
    ecs_set(world, hero, Health, { 42, 99 });

    ecs_entity_t rock = ecs_entity(world, { .name = "rock" });
    ecs_set(world, rock, MyePosition3D, { { 1.0f, 2.0f, 3.0f } });

    ASSERT_TRUE(mye_world_save_json(world, g_path));
    ASSERT_EQ_INT(0, mye_shutdown(world));

    /* --- world two: load and compare ---------------------------------- */
    ecs_world_t *restored = make_world();
    ASSERT_NOT_NULL(restored);
    ASSERT_TRUE(mye_world_load_json(restored, g_path));

    ecs_entity_t hero2 = ecs_lookup(restored, "hero");
    ASSERT_TRUE(hero2 != 0);

    const MyePosition2D *pos = ecs_get(restored, hero2, MyePosition2D);
    ASSERT_NOT_NULL(pos);
    ASSERT_NEAR(100.0, pos->x, 1e-4);
    ASSERT_NEAR(200.0, pos->y, 1e-4);

    const Health *health = ecs_get(restored, hero2, Health);
    ASSERT_NOT_NULL(health);
    ASSERT_EQ_INT(42, health->current);
    ASSERT_EQ_INT(99, health->max);

    /* Nested structs survive too -- Vector3 inside MyePosition3D. */
    ecs_entity_t rock2 = ecs_lookup(restored, "rock");
    ASSERT_TRUE(rock2 != 0);
    const MyePosition3D *p3 = ecs_get(restored, rock2, MyePosition3D);
    ASSERT_NOT_NULL(p3);
    ASSERT_NEAR(1.0, p3->v.x, 1e-4);
    ASSERT_NEAR(2.0, p3->v.y, 1e-4);
    ASSERT_NEAR(3.0, p3->v.z, 1e-4);

    ASSERT_EQ_INT(0, mye_shutdown(restored));
}

TEST(loading_rubbish_fails_instead_of_corrupting_the_world)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t e = ecs_entity(world, { .name = "survivor" });
    ecs_set(world, e, Health, { 5, 5 });

    ASSERT_FALSE(mye_world_from_json(world, "this is not json"));
    ASSERT_FALSE(mye_world_from_json(world, "{ \"results\": "));
    ASSERT_FALSE(mye_world_from_json(world, NULL));
    ASSERT_FALSE(mye_world_load_json(world, "/nonexistent/path.json"));

    /* The world is intact after every failure. */
    const Health *health = ecs_get(world, e, Health);
    ASSERT_NOT_NULL(health);
    ASSERT_EQ_INT(5, health->current);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* ------------------------------------------------------- scene subsets -- */

static void level_load(ecs_world_t *world, void *user)
{
    (void)user;
    for (int i = 0; i < 3; ++i) {
        ecs_entity_t e = mye_entity_new(world);
        ecs_set(world, e, MyePosition2D, { (float)i * 10.0f, 0.0f });
        ecs_set(world, e, Health, { 10 + i, 20 });
    }
}

TEST(a_scene_serializes_only_its_own_entities)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    /* Something outside the scene, which must NOT appear in the scene save. */
    ecs_entity_t global = ecs_entity(world, { .name = "global_config" });
    ecs_set(world, global, Health, { 1, 1 });

    mye_scene_register(world, &(mye_scene_desc){ .name = "level",
                                                 .load = level_load });
    mye_scene_switch(world, "level");
    mye_progress(world, FIXED_DT);

    char *json = mye_scene_to_json(world);
    ASSERT_NOT_NULL(json);

    /* The scene's values are there... */
    ASSERT_NOT_NULL(strstr(json, "Health"));
    ASSERT_NOT_NULL(strstr(json, "20"));
    /* ...and the entity that lives outside it is not. */
    ASSERT_NULL(strstr(json, "global_config"));

    mye_json_free(world, json);
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(no_active_scene_serializes_to_nothing)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ASSERT_NULL(mye_scene_to_json(world));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(engine_components_have_reflection_data),
          TEST_CASE(a_world_serializes_to_json_containing_its_values),
          TEST_CASE(save_and_load_round_trips_through_a_file),
          TEST_CASE(loading_rubbish_fails_instead_of_corrupting_the_world),
          TEST_CASE(a_scene_serializes_only_its_own_entities),
          TEST_CASE(no_active_scene_serializes_to_nothing))
