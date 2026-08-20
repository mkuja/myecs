/* Menu -> play -> menu, on top of the scene system (M6).
 *
 * This is the wiring, and only the wiring: asteroids.c has the game and does
 * not know scenes exist. Split out of main.c so the flow can be driven
 * headlessly by tests/integration/test_int_asteroids_scenes.c -- the same
 * reason the game itself is a library.
 *
 * The point of the exercise is that neither scene cleans up after itself.
 * Everything the play scene spawns -- the ship, every rock, every bullet
 * fired ten seconds in -- is tagged as the scene's, so unloading it deletes
 * exactly that set and nothing else. See engine/scene/scene.h. */
#ifndef ASTEROIDS_SCENES_H
#define ASTEROIDS_SCENES_H

#include "asteroids.h"

/* Rocks drifting behind the title. They exist to be forgotten about: nothing
 * deletes them, the switch to "play" does. */
#define MENU_BACKDROP_ROCKS 4

/* How long GAME OVER stays up before the menu comes back. ENTER skips it. */
#define GAME_OVER_LINGER 1.5f

/* Flow state, as opposed to game state: what the menu needs to know between
 * runs. Lives on the component entity, so it outlives both scenes. */
typedef struct AsteroidsFlow {
    float since_game_over;
    int last_score;
    bool have_last_score;
} AsteroidsFlow;

extern ECS_COMPONENT_DECLARE(AsteroidsFlow);

/* Registers the game (asteroids_register), both scenes, and the system that
 * moves between them. Enters neither: call asteroids_scenes_boot. */
void asteroids_scenes_register(ecs_world_t *world);

/* Requests the starting scene: MYE_START_SCENE when set -- so a screenshot
 * run or a bot can jump straight to "play" -- otherwise "menu". An unknown
 * name falls back to the menu rather than leaving the game in no scene at
 * all. Applied at the next frame boundary, like every switch. */
void asteroids_scenes_boot(ecs_world_t *world);

#endif /* ASTEROIDS_SCENES_H */
