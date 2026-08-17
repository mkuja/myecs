/* Integration tests for prefabs, using the real Asteroids templates.
 *
 * A prefab is a template entity; instances are created with an EcsIsA pair
 * and receive copies of its components. The tests pin both halves: what
 * instances inherit from the template, and the fact that each owns its own
 * storage afterwards. See plan/07-roadmap.md. */
#include "asteroids.h"
#include "mye_test.h"

#include <raylib.h>

#define FIXED_DT (1.0f / 60.0f)

static ecs_world_t *start_game(void)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .headless = true,
    });
    if (world == NULL) {
        return NULL;
    }
    SetRandomSeed(7);
    asteroids_setup(world);

    MyeInput *input = ecs_singleton_ensure(world, MyeInput);
    input->synthetic = true;
    ecs_singleton_modified(world, MyeInput);
    return world;
}

static int finish(ecs_world_t *world)
{
    asteroids_teardown(world);
    return mye_shutdown(world);
}

static int count_of(ecs_world_t *world, ecs_entity_t component)
{
    ecs_query_t *q = ecs_query(world, { .terms = {{ .id = component }} });
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

TEST(prefabs_exist_and_stay_out_of_the_game)
{
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    const GameState *state = ecs_singleton_get(world, GameState);
    ASSERT_NOT_NULL(state);
    ASSERT_TRUE(state->prefab_ship != 0);
    ASSERT_TRUE(state->prefab_bullet != 0);
    ASSERT_TRUE(state->prefab_explosion != 0);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(state->prefab_rock[i] != 0);
    }

    /* The templates carry Rock and Ship components, yet must not be counted
     * as rocks or ships: prefabs are excluded from queries. */
    ASSERT_EQ_INT(STARTING_ROCKS, count_of(world, ecs_id(Rock)));
    ASSERT_EQ_INT(1, count_of(world, ecs_id(Ship)));

    ASSERT_EQ_INT(0, finish(world));
}

TEST(instances_inherit_component_values)
{
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    const GameState *state = ecs_singleton_get(world, GameState);

    /* Rocks were spawned with only position, rotation and velocity set; the
     * collider and size came from the prefab. */
    ecs_query_t *rocks = ecs_query(world, {
        .terms = {{ .id = ecs_id(Rock) }, { .id = ecs_id(Collider) }},
    });
    ASSERT_NOT_NULL(rocks);

    int checked = 0;
    ecs_iter_t it = ecs_query_iter(world, rocks);
    while (ecs_query_next(&it)) {
        const Rock *rock = ecs_field(&it, Rock, 0);
        const Collider *collider = ecs_field(&it, Collider, 1);
        for (int i = 0; i < it.count; ++i) {
            ASSERT_EQ_INT(3, rock[i].size); /* wave one is all large rocks */
            ASSERT_NEAR(36.0, collider[i].radius, 1e-4);
            ++checked;
        }
    }
    ecs_query_fini(rocks);
    ASSERT_EQ_INT(STARTING_ROCKS, checked);

    /* And the values really are the prefab's. */
    const Collider *prefab_collider =
        ecs_get(world, state->prefab_rock[2], Collider);
    ASSERT_NOT_NULL(prefab_collider);
    ASSERT_NEAR(36.0, prefab_collider->radius, 1e-4);

    ASSERT_EQ_INT(0, finish(world));
}

TEST(instances_get_private_copies_not_shared_storage)
{
    /* flecs 4 copies prefab components to each instance by default, and this
     * project keeps that default deliberately -- see the long note in
     * build_prefabs. The consequence tested here is that every instance owns
     * its components and can be mutated without touching its neighbours or
     * the template. */
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    const GameState *state = ecs_singleton_get(world, GameState);
    const Collider *prefab_collider =
        ecs_get(world, state->prefab_rock[2], Collider);
    ASSERT_NOT_NULL(prefab_collider);

    ecs_query_t *rocks = ecs_query(world, {
        .terms = {{ .id = ecs_id(Rock) }, { .id = ecs_id(Collider) }},
    });
    ASSERT_NOT_NULL(rocks);

    int checked = 0;
    ecs_iter_t it = ecs_query_iter(world, rocks);
    while (ecs_query_next(&it)) {
        const Collider *collider = ecs_field(&it, Collider, 1);
        /* Distinct storage from the template... */
        ASSERT_TRUE(collider != prefab_collider);
        /* ...but the template's value. */
        for (int i = 0; i < it.count; ++i) {
            ASSERT_NEAR(prefab_collider->radius, collider[i].radius, 1e-4);
            ++checked;
        }
    }
    ecs_query_fini(rocks);
    ASSERT_EQ_INT(STARTING_ROCKS, checked);

    /* Because the field is per-entity rather than shared, systems may index
     * it by row. A shared field would be a single value and indexing it would
     * read out of bounds -- the reason sharing was not adopted. */
    ASSERT_EQ_INT(0, finish(world));
}

TEST(overridden_components_are_private_per_instance)
{
    /* Explosions must animate independently: each needs its own playhead.
     * If components were shared rather than copied, every explosion on
     * screen would show the same frame. */
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    const GameState *state = ecs_singleton_get(world, GameState);
    const MyeSpriteAnim *prefab_anim =
        ecs_get(world, state->prefab_explosion, MyeSpriteAnim);
    ASSERT_NOT_NULL(prefab_anim);

    ecs_entity_t a = ecs_new_w_pair(world, EcsIsA, state->prefab_explosion);
    ecs_set(world, a, MyePosition2D, { 100.0f, 100.0f });
    ecs_entity_t b = ecs_new_w_pair(world, EcsIsA, state->prefab_explosion);
    ecs_set(world, b, MyePosition2D, { 200.0f, 200.0f });

    const MyeSpriteAnim *anim_a = ecs_get(world, a, MyeSpriteAnim);
    const MyeSpriteAnim *anim_b = ecs_get(world, b, MyeSpriteAnim);
    ASSERT_NOT_NULL(anim_a);
    ASSERT_NOT_NULL(anim_b);

    /* Distinct storage from each other and from the template. */
    ASSERT_TRUE(anim_a != anim_b);
    ASSERT_TRUE(anim_a != prefab_anim);

    /* Advance one of them by hand: the other must not move with it. */
    MyeSpriteAnim *mutable_a = ecs_ensure(world, a, MyeSpriteAnim);
    mutable_a->current = 3;
    ecs_modified(world, a, MyeSpriteAnim);

    ASSERT_EQ_INT(3, ecs_get(world, a, MyeSpriteAnim)->current);
    ASSERT_EQ_INT(0, ecs_get(world, b, MyeSpriteAnim)->current);
    ASSERT_EQ_INT(0, prefab_anim->current); /* template untouched */

    ASSERT_EQ_INT(0, finish(world));
}

TEST(the_ship_sprite_is_private_so_blinking_is_not_global)
{
    /* BlinkInvulnerableShip writes MyeSprite.tint every frame, so the ship
     * must own its sprite. It does, because MyeSprite is left on the default
     * copy-on-instantiate behaviour. */
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    const GameState *state = ecs_singleton_get(world, GameState);
    const MyeSprite *prefab_sprite = ecs_get(world, state->prefab_ship,
                                             MyeSprite);
    ASSERT_NOT_NULL(prefab_sprite);

    ecs_query_t *ships = ecs_query(world, {
        .terms = {{ .id = ecs_id(Ship) }, { .id = ecs_id(MyeSprite) }},
    });
    ASSERT_NOT_NULL(ships);
    ecs_iter_t it = ecs_query_iter(world, ships);
    while (ecs_query_next(&it)) {
        const MyeSprite *sprite = ecs_field(&it, MyeSprite, 1);
        /* Private copy, unlike the rocks above. */
        ASSERT_TRUE(sprite != prefab_sprite);
    }
    ecs_query_fini(ships);

    ASSERT_EQ_INT(0, finish(world));
}

TEST(gameplay_still_works_through_prefabs)
{
    /* End-to-end: firing spawns a bullet from the prefab, and it behaves --
     * inherited collider, private lifetime that actually expires. */
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    MyeInput *input = ecs_singleton_ensure(world, MyeInput);
    mye_input_frame_begin(input);
    mye_input_apply(input, ACT_FIRE, true, 1.0f);
    mye_input_frame_end(input);
    ecs_singleton_modified(world, MyeInput);
    mye_progress(world, FIXED_DT);

    ASSERT_EQ_INT(1, count_of(world, ecs_id(Bullet)));

    /* The bullet has a collider it never set itself. */
    ecs_query_t *bullets = ecs_query(world, {
        .terms = {{ .id = ecs_id(Bullet) }, { .id = ecs_id(Collider) }},
    });
    ASSERT_NOT_NULL(bullets);
    int seen = 0;
    ecs_iter_t it = ecs_query_iter(world, bullets);
    while (ecs_query_next(&it)) {
        const Collider *collider = ecs_field(&it, Collider, 1);
        for (int i = 0; i < it.count; ++i) {
            ASSERT_NEAR(BULLET_RADIUS, collider[i].radius, 1e-4);
            ++seen;
        }
    }
    ecs_query_fini(bullets);
    ASSERT_EQ_INT(1, seen);

    /* Its private lifetime runs out -- proving Lifetime was not shared, which
     * would have counted down once for all bullets at N times the rate. */
    for (int i = 0; i < (int)(BULLET_LIFETIME * 60.0f) + 15; ++i) {
        mye_input_frame_begin(input);
        mye_input_frame_end(input);
        ecs_singleton_modified(world, MyeInput);
        mye_progress(world, FIXED_DT);
    }
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Bullet)));

    ASSERT_EQ_INT(0, finish(world));
}

TEST_MAIN(TEST_CASE(prefabs_exist_and_stay_out_of_the_game),
          TEST_CASE(instances_inherit_component_values),
          TEST_CASE(instances_get_private_copies_not_shared_storage),
          TEST_CASE(overridden_components_are_private_per_instance),
          TEST_CASE(the_ship_sprite_is_private_so_blinking_is_not_global),
          TEST_CASE(gameplay_still_works_through_prefabs))
