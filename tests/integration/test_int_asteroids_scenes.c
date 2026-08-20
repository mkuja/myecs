/* Menu -> play -> menu in the real Asteroids game, headless.
 *
 * The gameplay itself is tested in test_int_asteroids.c, which never touches
 * a scene. This file tests the seam between the two: that booting lands in
 * the menu, that ENTER starts a game, that running out of lives hands back to
 * the menu, and -- the one that matters -- that the second game is as clean
 * as the first because the play scene owned everything the first one made.
 *
 * Nothing here deletes an entity. If a test has to tidy up to pass, scene
 * ownership is not doing its job. See plan/07-roadmap.md (M6) and
 * engine/scene/scene.h. */
#define _POSIX_C_SOURCE 200809L

#include "mye_test.h"
#include "scene/scene.h"
#include "scenes.h"

#include <raylib.h>

#include <stdlib.h>
#include <string.h>

#define FIXED_DT (1.0f / 60.0f)

/* Everything the play scene puts on the field the moment it loads. */
#define PLAY_ENTITIES (1 + STARTING_ROCKS) /* the ship, and the first wave */

static ecs_world_t *boot(const char *start_scene)
{
    if (start_scene != NULL) {
        setenv("MYE_START_SCENE", start_scene, 1);
    } else {
        unsetenv("MYE_START_SCENE");
    }

    ecs_world_t *world = mye_init(&(mye_config){
        .headless = true,
        .frame_arena_bytes = 256 * 1024,
    });
    if (world == NULL) {
        return NULL;
    }

    SetRandomSeed(4242); /* deterministic rock placement */
    asteroids_scenes_register(world);
    asteroids_scenes_boot(world);

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

/* A switch is requested during a frame and applied at the top of the next
 * one, so getting from A to B always costs two: one to ask, one to arrive. */
static void press_enter(ecs_world_t *world)
{
    const int confirm[] = { ACT_CONFIRM };
    step_with(world, confirm, 1);
    step_idle(world, 1);
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

static const char *current(ecs_world_t *world)
{
    const char *name = mye_scene_current(world);
    return name != NULL ? name : "<none>";
}

/* Loses every life the honest way: park a rock on the ship and keep clearing
 * the invulnerability it respawns with. Returns true once the game is over. */
static bool play_until_game_over(ecs_world_t *world)
{
    /* mye_entity_new, not ecs_new: this rock belongs to the play scene like
     * any other, so it goes away with it and the counts stay honest. */
    ecs_entity_t rock = mye_entity_new(world);
    ecs_set(world, rock, MyePosition2D, { SCREEN_W * 0.5f, SCREEN_H * 0.5f });
    ecs_set(world, rock, MyeRotation2D, { 0.0f });
    ecs_set(world, rock, Velocity, { 0.0f, 0.0f });
    ecs_set(world, rock, Collider, { 40.0f });
    ecs_set(world, rock, Rock, { 3 });

    ecs_query_t *ships = ecs_query(world, { .terms = {{ .id = ecs_id(Ship) }} });
    if (ships == NULL) {
        return false;
    }

    bool over = false;
    for (int attempt = 0; attempt < 40 && !over; ++attempt) {
        ecs_iter_t it = ecs_query_iter(world, ships);
        while (ecs_query_next(&it)) {
            Ship *ship = ecs_field(&it, Ship, 0);
            for (int i = 0; i < it.count; ++i) {
                ship[i].invulnerable = 0.0f;
            }
        }
        step_idle(world, 2);

        const GameState *state = ecs_singleton_get(world, GameState);
        over = state != NULL && state->game_over;
    }
    ecs_query_fini(ships);
    return over;
}

TEST(booting_lands_in_the_menu_with_no_game_running)
{
    ecs_world_t *world = boot(NULL);
    ASSERT_NOT_NULL(world);

    /* Switches apply at a frame boundary, so before the first frame there is
     * no scene at all -- only a request. */
    ASSERT_NULL(mye_scene_current(world));
    ASSERT_TRUE(mye_scene_switch_pending(world));

    step_idle(world, 1);
    ASSERT_STR_EQ("menu", current(world));

    /* A menu, not a paused game: no ship, and the game is not running. */
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Ship)));
    const GameState *state = ecs_singleton_get(world, GameState);
    ASSERT_NOT_NULL(state);
    ASSERT_FALSE(state->playing);

    /* The backdrop, and only the backdrop. It stays that size: without the
     * `playing` gate, NextWaveWhenClear would mistake it for a cleared wave
     * and start filling the menu with rocks. */
    ASSERT_EQ_INT(MENU_BACKDROP_ROCKS, count_of(world, ecs_id(Rock)));
    ASSERT_EQ_INT(MENU_BACKDROP_ROCKS, mye_scene_entity_count(world));
    step_idle(world, 120);
    ASSERT_EQ_INT(MENU_BACKDROP_ROCKS, count_of(world, ecs_id(Rock)));

    ASSERT_EQ_INT(0, finish(world));
}

TEST(enter_starts_a_fresh_game_and_the_menu_goes_with_it)
{
    ecs_world_t *world = boot(NULL);
    ASSERT_NOT_NULL(world);
    step_idle(world, 1);
    ASSERT_STR_EQ("menu", current(world));

    press_enter(world);
    ASSERT_STR_EQ("play", current(world));

    /* Exactly one wave, not one wave plus the four the menu was drifting:
     * the menu owned those, and unloading it took them. */
    ASSERT_EQ_INT(1, count_of(world, ecs_id(Ship)));
    ASSERT_EQ_INT(STARTING_ROCKS, count_of(world, ecs_id(Rock)));
    ASSERT_EQ_INT(PLAY_ENTITIES, mye_scene_entity_count(world));

    const GameState *state = ecs_singleton_get(world, GameState);
    ASSERT_TRUE(state->playing);
    ASSERT_FALSE(state->game_over);
    ASSERT_EQ_INT(0, state->score);
    ASSERT_EQ_INT(STARTING_LIVES, state->lives);
    ASSERT_EQ_INT(0, state->wave);

    ASSERT_EQ_INT(0, finish(world));
}

TEST(mye_start_scene_jumps_straight_to_play)
{
    /* The automation hatch, same as the tutorial's: a screenshot run or a bot
     * should not have to press ENTER through a menu. */
    ecs_world_t *world = boot("play");
    ASSERT_NOT_NULL(world);

    step_idle(world, 1);
    ASSERT_STR_EQ("play", current(world));
    ASSERT_EQ_INT(1, count_of(world, ecs_id(Ship)));
    ASSERT_EQ_INT(STARTING_ROCKS, count_of(world, ecs_id(Rock)));

    ASSERT_EQ_INT(0, finish(world));
}

TEST(an_unknown_start_scene_falls_back_to_the_menu)
{
    /* A typo in an environment variable must not leave the game in no scene,
     * with nothing on screen and nothing to press. */
    ecs_world_t *world = boot("levl_1");
    ASSERT_NOT_NULL(world);

    step_idle(world, 1);
    ASSERT_STR_EQ("menu", current(world));

    ASSERT_EQ_INT(0, finish(world));
}

TEST(running_out_of_lives_returns_to_the_menu_on_its_own)
{
    ecs_world_t *world = boot("play");
    ASSERT_NOT_NULL(world);
    step_idle(world, 1);
    ASSERT_STR_EQ("play", current(world));

    ASSERT_TRUE(play_until_game_over(world));
    ASSERT_STR_EQ("play", current(world)); /* GAME OVER is still up */

    /* No input from here on: the game over screen times out by itself. */
    step_idle(world, (int)(GAME_OVER_LINGER * 60.0f) + 4);
    ASSERT_STR_EQ("menu", current(world));

    /* The field went with the scene -- ship, rocks, the lot. */
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Ship)));
    ASSERT_EQ_INT(MENU_BACKDROP_ROCKS, count_of(world, ecs_id(Rock)));
    ASSERT_EQ_INT(MENU_BACKDROP_ROCKS, mye_scene_entity_count(world));

    const GameState *state = ecs_singleton_get(world, GameState);
    ASSERT_FALSE(state->playing);

    /* The unload callback read the score before the entities were deleted --
     * the one thing the engine could not have saved by itself. */
    const AsteroidsFlow *flow = ecs_singleton_get(world, AsteroidsFlow);
    ASSERT_NOT_NULL(flow);
    ASSERT_TRUE(flow->have_last_score);

    ASSERT_EQ_INT(0, finish(world));
}

/* Every gameplay entity alive right now, however it got here. */
static int field_size(ecs_world_t *world)
{
    return count_of(world, ecs_id(Ship)) + count_of(world, ecs_id(Rock)) +
           count_of(world, ecs_id(Bullet)) + count_of(world, ecs_id(Explosion));
}

TEST(entities_spawned_mid_game_belong_to_the_play_scene)
{
    /* The load callback's ship and first wave are the easy half. The half
     * that catches people out is what gameplay spawns afterwards -- a bullet
     * comes out of ShipControl, a fragment out of BulletsHitRocks -- because
     * those are what a hand-written cleanup list forgets. They are owned too,
     * as long as the spawn went through mye_entity_new. */
    ecs_world_t *world = boot("play");
    ASSERT_NOT_NULL(world);
    step_idle(world, 1);
    ASSERT_STR_EQ("play", current(world));
    ASSERT_EQ_INT(PLAY_ENTITIES, mye_scene_entity_count(world));

    const int fire[] = { ACT_FIRE };
    step_with(world, fire, 1);
    ASSERT_TRUE(count_of(world, ecs_id(Bullet)) > 0);

    /* Not "the count went up by one" -- the stronger claim, that the scene
     * owns the entire field whatever happened during that frame. */
    ASSERT_EQ_INT(field_size(world), mye_scene_entity_count(world));

    /* Leave without a game over, so the bullet is unquestionably alive rather
     * than merely expired when the scene goes. */
    ASSERT_TRUE(mye_scene_switch(world, "menu"));
    step_idle(world, 1);

    ASSERT_STR_EQ("menu", current(world));
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Bullet)));
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Ship)));
    ASSERT_EQ_INT(MENU_BACKDROP_ROCKS, count_of(world, ecs_id(Rock)));

    ASSERT_EQ_INT(0, finish(world));
}

TEST(a_second_play_through_starts_clean)
{
    /* The whole point of the exercise. Nothing in this test deletes anything;
     * the switch does it. */
    ecs_world_t *world = boot("play");
    ASSERT_NOT_NULL(world);
    step_idle(world, 1);
    ASSERT_STR_EQ("play", current(world));

    /* Make a mess: shoot for a while so rocks split into fragments nothing
     * placed by hand, and put a score on the board. */
    const int fire[] = { ACT_FIRE };
    for (int i = 0; i < 45; ++i) {
        step_with(world, fire, 1);
    }

    GameState *state = ecs_singleton_ensure(world, GameState);
    state->score = 1234;
    state->wave = 7;
    state->lives = 1;
    ecs_singleton_modified(world, GameState);

    ASSERT_TRUE(play_until_game_over(world));
    ASSERT_TRUE(field_size(world) > PLAY_ENTITIES); /* it really is a mess */

    /* Whatever the final score turned out to be -- 1234 plus whatever the
     * last shots were worth. */
    int final_score = ecs_singleton_get(world, GameState)->score;
    ASSERT_TRUE(final_score >= 1234);

    /* ENTER skips the game-over wait. */
    press_enter(world);
    ASSERT_STR_EQ("menu", current(world));
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Bullet)));
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Ship)));

    /* The menu kept the score it was handed. */
    const AsteroidsFlow *flow = ecs_singleton_get(world, AsteroidsFlow);
    ASSERT_NOT_NULL(flow);
    ASSERT_EQ_INT(final_score, flow->last_score);

    /* Round two, from scratch. */
    press_enter(world);
    ASSERT_STR_EQ("play", current(world));

    ASSERT_EQ_INT(1, count_of(world, ecs_id(Ship)));
    ASSERT_EQ_INT(STARTING_ROCKS, count_of(world, ecs_id(Rock)));
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Bullet)));
    ASSERT_EQ_INT(PLAY_ENTITIES, mye_scene_entity_count(world));

    state = ecs_singleton_ensure(world, GameState);
    ASSERT_EQ_INT(0, state->score);
    ASSERT_EQ_INT(STARTING_LIVES, state->lives);
    ASSERT_EQ_INT(0, state->wave);
    ASSERT_FALSE(state->game_over);
    ASSERT_TRUE(state->playing);

    ASSERT_EQ_INT(0, finish(world));
}

TEST(cycling_between_the_menu_and_the_game_leaks_nothing)
{
    /* The scene tests do twenty cycles of two empty scenes; this does five of
     * a real game, with rocks splitting and bullets in flight. Each cycle
     * must land on the same counts as the last, and mye_shutdown must still
     * report a clean allocator afterwards. */
    ecs_world_t *world = boot(NULL);
    ASSERT_NOT_NULL(world);
    step_idle(world, 1);

    const int fire[] = { ACT_FIRE };

    for (int cycle = 0; cycle < 5; ++cycle) {
        ASSERT_STR_EQ("menu", current(world));
        ASSERT_EQ_INT(MENU_BACKDROP_ROCKS, mye_scene_entity_count(world));

        press_enter(world);
        ASSERT_STR_EQ("play", current(world));
        ASSERT_EQ_INT(PLAY_ENTITIES, mye_scene_entity_count(world));

        /* Play a little: shoot, split rocks, leave debris behind. */
        for (int i = 0; i < 30; ++i) {
            step_with(world, fire, 1);
        }

        GameState *state = ecs_singleton_ensure(world, GameState);
        state->lives = 1;
        ecs_singleton_modified(world, GameState);
        ASSERT_TRUE(play_until_game_over(world));

        press_enter(world);
    }
    ASSERT_STR_EQ("menu", current(world));
    ASSERT_EQ_INT(MENU_BACKDROP_ROCKS, mye_scene_entity_count(world));

    /* asteroids_teardown releases the one query the game owns; everything
     * else is the scene system's problem, and it had better have solved it.
     * A non-zero return means the tracking allocator still holds memory. */
    ASSERT_EQ_INT(0, finish(world));
}

TEST_MAIN(TEST_CASE(booting_lands_in_the_menu_with_no_game_running),
          TEST_CASE(enter_starts_a_fresh_game_and_the_menu_goes_with_it),
          TEST_CASE(mye_start_scene_jumps_straight_to_play),
          TEST_CASE(an_unknown_start_scene_falls_back_to_the_menu),
          TEST_CASE(running_out_of_lives_returns_to_the_menu_on_its_own),
          TEST_CASE(entities_spawned_mid_game_belong_to_the_play_scene),
          TEST_CASE(a_second_play_through_starts_clean),
          TEST_CASE(cycling_between_the_menu_and_the_game_leaks_nothing))
