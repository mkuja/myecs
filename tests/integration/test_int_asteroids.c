/* End-to-end gameplay test: drives the real Asteroids game headlessly with
 * synthetic input and asserts what a player would observe -- bullets appear,
 * rocks break apart, the score rises, lives run out, restart works.
 *
 * This is the M3 definition-of-done test. See plan/09-testing.md. */
#include "asteroids.h"
#include "mye_test.h"

#include <raylib.h>

#define FIXED_DT (1.0f / 60.0f)

static ecs_world_t *start_game(void)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .headless = true,
        .frame_arena_bytes = 256 * 1024,
    });
    if (world == NULL) {
        return NULL;
    }

    /* Same seed every run: the test is deterministic. */
    SetRandomSeed(4242);
    asteroids_setup(world);

    /* Take over input: the engine must not overwrite what we set. */
    MyeInput *input = ecs_singleton_ensure(world, MyeInput);
    input->synthetic = true;
    ecs_singleton_modified(world, MyeInput);
    return world;
}

/* Holds the given actions for one frame and advances the game. */
static void step_with(ecs_world_t *world, const int *actions, int action_count)
{
    MyeInput *input = ecs_singleton_ensure(world, MyeInput);
    mye_input_frame_begin(input);
    for (int i = 0; i < action_count; ++i) {
        mye_input_apply(input, actions[i], true, 1.0f);
    }
    mye_input_frame_end(input);
    ecs_singleton_modified(world, MyeInput);

    mye_progress(world, FIXED_DT);
}

static void step_idle(ecs_world_t *world, int frames)
{
    for (int i = 0; i < frames; ++i) {
        step_with(world, NULL, 0);
    }
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

/* Tear down in order and report the leak check: non-zero means the tracking
 * allocator still had live allocations. */
static int finish(ecs_world_t *world)
{
    asteroids_teardown(world);
    return mye_shutdown(world);
}

TEST(game_starts_with_a_ship_and_a_wave_of_rocks)
{
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    ASSERT_EQ_INT(1, count_of(world, ecs_id(Ship)));
    ASSERT_EQ_INT(STARTING_ROCKS, count_of(world, ecs_id(Rock)));

    const GameState *state = ecs_singleton_get(world, GameState);
    ASSERT_NOT_NULL(state);
    ASSERT_EQ_INT(STARTING_LIVES, state->lives);
    ASSERT_EQ_INT(0, state->score);
    ASSERT_FALSE(state->game_over);

    ASSERT_EQ_INT(0, finish(world));
}

TEST(firing_spawns_bullets_that_expire)
{
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    ASSERT_EQ_INT(0, count_of(world, ecs_id(Bullet)));

    const int fire[] = { ACT_FIRE };
    step_with(world, fire, 1);
    ASSERT_EQ_INT(1, count_of(world, ecs_id(Bullet)));

    /* The cooldown limits the rate: holding fire does not spray a bullet per
     * frame. */
    for (int i = 0; i < 10; ++i) {
        step_with(world, fire, 1);
    }
    int after_ten = count_of(world, ecs_id(Bullet));
    ASSERT_TRUE(after_ten >= 1);
    ASSERT_TRUE(after_ten <= 3); /* 11 frames / 0.22s cooldown */

    /* Bullets die of old age rather than accumulating forever. */
    step_idle(world, (int)(BULLET_LIFETIME * 60.0f) + 10);
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Bullet)));

    ASSERT_EQ_INT(0, finish(world));
}

TEST(shooting_a_rock_splits_it_and_scores)
{
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    /* Clear the randomly placed wave and stage an exact scenario: one large
     * rock sitting directly to the ship's right, on a collision course with
     * a bullet fired straight ahead. */
    ecs_delete_with(world, ecs_id(Rock));
    GameState *state = ecs_singleton_ensure(world, GameState);
    state->rocks_alive = 0;

    ecs_entity_t rock = ecs_new(world);
    ecs_set(world, rock, MyePosition2D, { SCREEN_W * 0.5f + 120.0f,
                                          SCREEN_H * 0.5f });
    ecs_set(world, rock, MyeRotation2D, { 0.0f });
    ecs_set(world, rock, Velocity, { 0.0f, 0.0f });
    ecs_set(world, rock, Collider, { 36.0f });
    ecs_set(world, rock, Rock, { 3 });
    state->rocks_alive = 1;

    /* Point the ship at the rock (angle 0 = facing right). */
    ecs_query_t *ships = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyeRotation2D) }, { .id = ecs_id(Ship) }},
    });
    ASSERT_NOT_NULL(ships);
    ecs_iter_t it = ecs_query_iter(world, ships);
    while (ecs_query_next(&it)) {
        MyeRotation2D *rot = ecs_field(&it, MyeRotation2D, 0);
        for (int i = 0; i < it.count; ++i) {
            rot[i].angle = 0.0f;
        }
    }
    ecs_query_fini(ships);

    const int fire[] = { ACT_FIRE };
    step_with(world, fire, 1);
    ASSERT_EQ_INT(1, count_of(world, ecs_id(Bullet)));

    /* Give the bullet time to cross the 120 px gap. */
    step_idle(world, 30);

    state = ecs_singleton_ensure(world, GameState);
    ASSERT_TRUE(state->score > 0);              /* it scored */
    ASSERT_EQ_INT(2, count_of(world, ecs_id(Rock))); /* large -> two mediums */

    ASSERT_EQ_INT(0, finish(world));
}

TEST(colliding_with_a_rock_costs_a_life)
{
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    /* Clear the invulnerability the ship spawns with. */
    ecs_query_t *ships = ecs_query(world, {
        .terms = {{ .id = ecs_id(Ship) }, { .id = ecs_id(MyePosition2D) }},
    });
    ASSERT_NOT_NULL(ships);
    ecs_iter_t it = ecs_query_iter(world, ships);
    while (ecs_query_next(&it)) {
        Ship *ship = ecs_field(&it, Ship, 0);
        for (int i = 0; i < it.count; ++i) {
            ship[i].invulnerable = 0.0f;
        }
    }
    ecs_query_fini(ships);

    /* Drop a rock right on top of the ship. */
    ecs_entity_t rock = ecs_new(world);
    ecs_set(world, rock, MyePosition2D, { SCREEN_W * 0.5f, SCREEN_H * 0.5f });
    ecs_set(world, rock, MyeRotation2D, { 0.0f });
    ecs_set(world, rock, Velocity, { 0.0f, 0.0f });
    ecs_set(world, rock, Collider, { 30.0f });
    ecs_set(world, rock, Rock, { 3 });

    step_idle(world, 2);

    const GameState *state = ecs_singleton_get(world, GameState);
    ASSERT_EQ_INT(STARTING_LIVES - 1, state->lives);
    ASSERT_FALSE(state->game_over);

    ASSERT_EQ_INT(0, finish(world));
}

TEST(running_out_of_lives_ends_the_game_and_restart_recovers)
{
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    /* Park a rock in the middle and repeatedly clear invulnerability: the
     * ship respawns into it and loses a life each time. */
    ecs_entity_t rock = ecs_new(world);
    ecs_set(world, rock, MyePosition2D, { SCREEN_W * 0.5f, SCREEN_H * 0.5f });
    ecs_set(world, rock, MyeRotation2D, { 0.0f });
    ecs_set(world, rock, Velocity, { 0.0f, 0.0f });
    ecs_set(world, rock, Collider, { 40.0f });
    ecs_set(world, rock, Rock, { 3 });

    ecs_query_t *ships = ecs_query(world, {
        .terms = {{ .id = ecs_id(Ship) }},
    });
    ASSERT_NOT_NULL(ships);

    for (int attempt = 0; attempt < 40; ++attempt) {
        ecs_iter_t it = ecs_query_iter(world, ships);
        while (ecs_query_next(&it)) {
            Ship *ship = ecs_field(&it, Ship, 0);
            for (int i = 0; i < it.count; ++i) {
                ship[i].invulnerable = 0.0f;
            }
        }
        step_idle(world, 2);

        const GameState *s = ecs_singleton_get(world, GameState);
        if (s->game_over) {
            break;
        }
    }
    ecs_query_fini(ships);

    const GameState *state = ecs_singleton_get(world, GameState);
    ASSERT_TRUE(state->game_over);
    ASSERT_TRUE(state->lives <= 0);

    /* Restart: pressing R puts a fresh ship and wave on the field. */
    const int restart[] = { ACT_RESTART };
    step_with(world, restart, 1);

    state = ecs_singleton_get(world, GameState);
    ASSERT_FALSE(state->game_over);
    ASSERT_EQ_INT(STARTING_LIVES, state->lives);
    ASSERT_EQ_INT(0, state->score);
    ASSERT_EQ_INT(1, count_of(world, ecs_id(Ship)));
    ASSERT_EQ_INT(STARTING_ROCKS, count_of(world, ecs_id(Rock)));
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Bullet)));

    ASSERT_EQ_INT(0, finish(world));
}

TEST(thrust_moves_the_ship_and_the_screen_wraps)
{
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);
    ecs_delete_with(world, ecs_id(Rock)); /* nothing to crash into */

    ecs_query_t *ships = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyePosition2D) },
                  { .id = ecs_id(MyeRotation2D) },
                  { .id = ecs_id(Ship) }},
    });
    ASSERT_NOT_NULL(ships);

    /* Face right and thrust. */
    ecs_iter_t it = ecs_query_iter(world, ships);
    while (ecs_query_next(&it)) {
        MyeRotation2D *rot = ecs_field(&it, MyeRotation2D, 1);
        for (int i = 0; i < it.count; ++i) {
            rot[i].angle = 0.0f;
        }
    }

    const int thrust[] = { ACT_THRUST };
    for (int i = 0; i < 60; ++i) {
        step_with(world, thrust, 1);
    }

    float x = 0.0f;
    it = ecs_query_iter(world, ships);
    while (ecs_query_next(&it)) {
        const MyePosition2D *pos = ecs_field(&it, MyePosition2D, 0);
        for (int i = 0; i < it.count; ++i) {
            x = pos[i].x;
        }
    }
    ASSERT_TRUE(x > SCREEN_W * 0.5f); /* it moved right */

    /* Keep going: the ship must wrap around rather than fly off forever. */
    for (int i = 0; i < 240; ++i) {
        step_with(world, thrust, 1);
    }
    it = ecs_query_iter(world, ships);
    while (ecs_query_next(&it)) {
        const MyePosition2D *pos = ecs_field(&it, MyePosition2D, 0);
        for (int i = 0; i < it.count; ++i) {
            ASSERT_TRUE(pos[i].x >= 0.0f && pos[i].x <= (float)SCREEN_W);
            ASSERT_TRUE(pos[i].y >= 0.0f && pos[i].y <= (float)SCREEN_H);
        }
    }

    ecs_query_fini(ships);
    ASSERT_EQ_INT(0, finish(world));
}

TEST(clearing_a_wave_spawns_the_next_one)
{
    ecs_world_t *world = start_game();
    ASSERT_NOT_NULL(world);

    GameState *state = ecs_singleton_ensure(world, GameState);
    int wave_before = state->wave;

    /* Wipe the field: the next fixed step should refill it, larger. */
    ecs_delete_with(world, ecs_id(Rock));
    state->rocks_alive = 0;

    step_idle(world, 2);

    state = ecs_singleton_ensure(world, GameState);
    ASSERT_EQ_INT(wave_before + 1, state->wave);
    ASSERT_TRUE(count_of(world, ecs_id(Rock)) > STARTING_ROCKS);

    ASSERT_EQ_INT(0, finish(world));
}

TEST_MAIN(TEST_CASE(game_starts_with_a_ship_and_a_wave_of_rocks),
          TEST_CASE(firing_spawns_bullets_that_expire),
          TEST_CASE(shooting_a_rock_splits_it_and_scores),
          TEST_CASE(colliding_with_a_rock_costs_a_life),
          TEST_CASE(running_out_of_lives_ends_the_game_and_restart_recovers),
          TEST_CASE(thrust_moves_the_ship_and_the_screen_wraps),
          TEST_CASE(clearing_a_wave_spawns_the_next_one))
