/* M3 -- a complete little game: Asteroids.
 *
 * Everything the engine offers so far, used the way a game would use it:
 *   - action-based input          (engine/input)
 *   - fixed-timestep simulation   (MyeOnFixedUpdate)
 *   - sprites, camera, sorting    (engine/render)
 *   - handle-based assets         (engine/asset), textures generated at
 *                                  runtime so the example needs no art files
 *   - collision via raylib's CheckCollisionCircles
 *
 * The game logic lives in asteroids.c so that tests can drive it headlessly.
 *
 * Controls: left/right (or A/D) rotate, up (or W) thrusts, space fires,
 * R restarts after a game over.
 */
#include "asteroids.h"

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
    asteroids_setup(world);

    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }

    asteroids_teardown(world);
    return mye_shutdown(world);
}
