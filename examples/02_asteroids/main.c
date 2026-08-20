/* M3 -- a complete little game: Asteroids. With M6's scenes on top.
 *
 * Everything the engine offers, used the way a game would use it:
 *   - action-based input          (engine/input)
 *   - fixed-timestep simulation   (MyeOnFixedUpdate)
 *   - sprites, camera, sorting    (engine/render)
 *   - handle-based assets         (engine/asset), textures generated at
 *                                  runtime so the example needs no art files
 *   - collision via raylib's CheckCollisionCircles
 *   - menu -> play -> menu        (engine/scene)
 *
 * Three files, three jobs: asteroids.c is the game and knows nothing about
 * scenes, scenes.c is the flow between the menu and the game, and main is the
 * loop. The first two are libraries so tests can drive them headlessly.
 *
 * Controls: ENTER starts, left/right (or A/D) rotate, up (or W) thrusts,
 * space fires. Running out of lives returns to the menu.
 *
 * MYE_START_SCENE=play skips the menu.
 */
#include "scenes.h"

#include <raylib.h>

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .width = SCREEN_W,
        .height = SCREEN_H,
        .title = "myecs -- M3 asteroids",
        .frame_arena_bytes = 256 * 1024,
    });
    if (world == NULL) {
        return 1;
    }

    SetRandomSeed(20260816);
    asteroids_scenes_register(world);
    asteroids_scenes_boot(world);

    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }

    asteroids_teardown(world);
    return mye_shutdown(world);
}
