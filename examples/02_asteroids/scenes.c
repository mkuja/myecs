/* Asteroids' menu <-> play flow. See scenes.h. */
#include "scenes.h"

#include "core/log.h"
#include "scene/scene.h"

#include <raylib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ECS_COMPONENT_DECLARE(AsteroidsFlow);

/* Draw and flow systems run every frame whichever scene is up, so each one
 * asks first. Cheaper and clearer than registering and unregistering systems
 * per scene, which flecs would let you do and which nobody enjoys debugging. */
static bool scene_is(const ecs_world_t *world, const char *name)
{
    const char *current = mye_scene_current(world);
    return current != NULL && strcmp(current, name) == 0;
}

/* ----------------------------------------------------------- the scenes -- */

static void menu_load(ecs_world_t *world, void *user)
{
    (void)user;
    /* Scene-owned, like everything else: these drift away the moment the
     * player presses ENTER, without the menu being told to tidy up. */
    asteroids_spawn_backdrop(world, MENU_BACKDROP_ROCKS);
    mye_log_info("scene: menu");
}

static void play_load(ecs_world_t *world, void *user)
{
    (void)user;
    AsteroidsFlow *flow = ecs_singleton_ensure(world, AsteroidsFlow);
    if (flow != NULL) {
        flow->since_game_over = 0.0f;
        ecs_singleton_modified(world, AsteroidsFlow);
    }

    /* Score, lives and wave back to the beginning, ship and first wave on the
     * field. Every entry into "play" is a new game -- there is no separate
     * restart path any more, because this is it. */
    asteroids_start(world);
    mye_log_info("scene: play");
}

static void play_unload(ecs_world_t *world, void *user)
{
    (void)user;
    /* Runs before the scene's entities are deleted, which is the only reason
     * it can still read the score. Exactly the case the unload callback
     * exists for: state the engine has no way to know matters. */
    const GameState *state = ecs_singleton_get(world, GameState);
    AsteroidsFlow *flow = ecs_singleton_ensure(world, AsteroidsFlow);
    if (state != NULL && flow != NULL) {
        flow->last_score = state->score;
        flow->have_last_score = true;
        ecs_singleton_modified(world, AsteroidsFlow);
    }
    asteroids_stop(world);
}

/* ------------------------------------------------------------- the flow -- */

static void SceneFlow(ecs_iter_t *it)
{
    AsteroidsFlow *flow = ecs_field(it, AsteroidsFlow, 0);
    ecs_world_t *world = it->world;

    /* A switch already requested this frame lands at the next frame boundary;
     * asking for another one before then would just overwrite it. */
    if (mye_scene_switch_pending(world)) {
        return;
    }

    if (scene_is(world, "menu")) {
        if (mye_action_pressed(world, ACT_CONFIRM)) {
            mye_scene_switch(world, "play");
        }
        return;
    }
    if (!scene_is(world, "play")) {
        return;
    }

    const GameState *state = ecs_singleton_get(world, GameState);
    if (state == NULL || !state->game_over) {
        return;
    }

    /* Game over: hold it long enough to read, then hand back to the menu.
     * Leaving on ENTER as well, so nobody has to wait. */
    flow->since_game_over += (float)it->delta_time;
    if (flow->since_game_over >= GAME_OVER_LINGER ||
        mye_action_pressed(world, ACT_CONFIRM)) {
        mye_scene_switch(world, "menu");
    }
}

/* --------------------------------------------------------------- the UI -- */

/* The score lives in the play scene's HUD (asteroids.c); the controls live
 * here, where a player looking for them would be. */
static void DrawMenu(ecs_iter_t *it)
{
    const AsteroidsFlow *flow = ecs_field(it, AsteroidsFlow, 0);
    ecs_world_t *world = it->world;
    if (!scene_is(world, "menu")) {
        return;
    }

    const char *title = "ASTEROIDS";
    DrawText(title, SCREEN_W / 2 - MeasureText(title, 84) / 2, 170, 84,
             RAYWHITE);

    const char *prompt = "press ENTER to play";
    DrawText(prompt, SCREEN_W / 2 - MeasureText(prompt, 28) / 2, 300, 28,
             GOLD);

    const char *controls =
        "LEFT / RIGHT or A / D turn      UP or W thrusts      SPACE fires";
    DrawText(controls, SCREEN_W / 2 - MeasureText(controls, 20) / 2, 380, 20,
             (Color){ 170, 178, 195, 255 });

    if (flow->have_last_score) {
        /* Per-frame scratch: reclaimed at the top of the next frame. */
        char *line = MYE_NEW_ARRAY(mye_frame_allocator(world), char, 64);
        if (line != NULL) {
            snprintf(line, 64, "LAST SCORE %d", flow->last_score);
            DrawText(line, SCREEN_W / 2 - MeasureText(line, 24) / 2, 450, 24,
                     SKYBLUE);
        }
    }
}

/* ---------------------------------------------------------------- setup -- */

void asteroids_scenes_register(ecs_world_t *world)
{
    asteroids_register(world);

    ECS_COMPONENT_DEFINE(world, AsteroidsFlow);
    ecs_add_id(world, ecs_id(AsteroidsFlow), EcsSingleton);
    ecs_singleton_set(world, AsteroidsFlow, { 0 });

    /* Registered here rather than in a scene's load callback: systems are
     * global, and re-registering one on every switch is how you end up
     * running it twice. */
    ECS_SYSTEM(world, SceneFlow, EcsOnUpdate, AsteroidsFlow);

    const mye_engine *engine = mye_engine_get(world);
    if (engine == NULL || !engine->headless) {
        ECS_SYSTEM(world, DrawMenu, MyeOnDrawUI, [in] AsteroidsFlow);
    }

    mye_scene_register(world, &(mye_scene_desc){ .name = "menu",
                                                 .load = menu_load });
    mye_scene_register(world, &(mye_scene_desc){ .name = "play",
                                                 .load = play_load,
                                                 .unload = play_unload });
}

void asteroids_scenes_boot(ecs_world_t *world)
{
    const char *start = getenv("MYE_START_SCENE");
    if (start != NULL && start[0] != '\0' && mye_scene_switch(world, start)) {
        return;
    }
    if (start != NULL && start[0] != '\0') {
        mye_log_warn("MYE_START_SCENE=\"%s\" is not a scene; starting at the "
                     "menu", start);
    }
    mye_scene_switch(world, "menu");
}
