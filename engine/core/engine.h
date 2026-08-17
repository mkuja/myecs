/* Engine lifecycle: window, flecs world, allocators, frame timing.
 * See plan/01-architecture.md. */
#ifndef MYE_CORE_ENGINE_H
#define MYE_CORE_ENGINE_H

#include "core/alloc.h"
#include "core/log.h"

#include <flecs.h>

/* ------------------------------------------------------------ singletons -- */

/* Frame timing. Updated in EcsOnLoad, read by everything downstream. */
typedef struct MyeTime {
    float delta;    /* seconds since the previous frame (render time) */
    double elapsed; /* seconds since mye_init */
    uint64_t frame; /* frames completed since mye_init */

    /* Fixed timestep. Systems registered in the MyeOnFixedUpdate phase run
     * `steps_this_frame` times per frame with exactly `fixed_dt` elapsed,
     * which is what makes movement and collision stable and reproducible
     * regardless of framerate. See plan/01-architecture.md. */
    float fixed_dt;          /* default 1/60 */
    float accumulator;       /* unspent time carried to the next frame */
    float alpha;             /* [0,1) blend for interpolated rendering */
    int steps_this_frame;    /* fixed steps run during the current frame */
    int max_steps_per_frame; /* spiral-of-death clamp, default 5 */
    uint64_t fixed_step;     /* total fixed steps since mye_init */
} MyeTime;

/* Engine state. Held as a pointer so the struct has one stable address that
 * systems can mutate, rather than being copied into component storage. */
typedef struct mye_engine mye_engine;

typedef struct MyeApp {
    mye_engine *engine;
} MyeApp;

extern ECS_COMPONENT_DECLARE(MyeTime);
extern ECS_COMPONENT_DECLARE(MyeApp);

/* Pipeline phase for fixed-timestep systems. Register with:
 *     ECS_SYSTEM(world, Physics, MyeOnFixedUpdate, Position, Velocity);
 * The callback's it->delta_time is always exactly MyeTime.fixed_dt. */
extern ecs_entity_t MyeOnFixedUpdate;

/* The frame's draw order, as an explicit chain of phases:
 *
 *   EcsOnStore        BeginDrawing + clear      (engine)
 *   MyeOnCamera       follow / orbit systems    (engine + game)
 *   MyeOnDrawCanvases off-screen canvases       (canvas)
 *   MyeOnDraw3D       world-space 3D pass       (render3d)
 *   MyeOnDraw2D       world-space sprite pass   (render2d)
 *   MyeOnDrawUI       screen-space HUD and menus (game)
 *   MyeOnRenderEnd    EndDrawing                (engine)
 *
 * Phases rather than registration order, so a module or game can register a
 * draw system at any time and still land in the right place. 3D draws first
 * so 2D sprites and the HUD compose over it.
 *
 * MyeOnCamera runs after transforms are propagated (EcsPostUpdate) and
 * blended for display (EcsPreStore), so a system there that moves a camera
 * from a target's drawn position sees this frame's final value. Camera
 * logic belongs here, not in EcsOnUpdate where the target has not moved yet
 * -- see render/camera.h. */
extern ecs_entity_t MyeOnCamera;
/* Canvases draw here, before the window's passes: whatever displays a canvas
 * then shows this frame's contents. See render/canvas.h. */
extern ecs_entity_t MyeOnDrawCanvases;
extern ecs_entity_t MyeOnDraw3D;
extern ecs_entity_t MyeOnDraw2D;
extern ecs_entity_t MyeOnDrawUI;
extern ecs_entity_t MyeOnRenderEnd;

struct mye_engine {
    /* Long-lived allocations. In debug builds this is the tracking allocator
     * wrapping `base`, so shutdown can report leaks. */
    mye_allocator allocator;
    mye_allocator base;
    mye_tracking tracking;

    /* Reset at the top of every frame (EcsPreFrame). Per-frame scratch:
     * draw lists, sort keys, temporary strings. */
    mye_arena frame_arena;

    bool headless;
    bool window_open;

    /* The size the window was asked for. Kept even when headless, so
     * projection maths (camera screen/world helpers) works without a
     * window -- which is what makes picking testable. */
    int width;
    int height;
    uint64_t max_frames; /* 0 = unlimited */
    const char *screenshot_path;

    /* Pipeline holding every MyeOnFixedUpdate system, run once per fixed
     * step by the engine rather than once per frame by flecs. */
    ecs_entity_t fixed_pipeline;
};

/* --------------------------------------------------------------- config -- */

typedef struct mye_config {
    int width;             /* default 1280 */
    int height;            /* default 720 */
    const char *title;     /* default "myecs" */
    int target_fps;        /* default 60; 0 = uncapped */

    /* Allocator every engine subsystem allocates from. Defaults to the heap
     * allocator when left zeroed. */
    mye_allocator allocator;

    /* Per-frame scratch capacity in bytes. Default 1 MiB. */
    size_t frame_arena_bytes;

    /* flecs pipeline worker threads for SIMULATION systems. 0 or 1 keeps
     * everything on the main thread (the default, and the right starting
     * point). Only systems explicitly marked `.multi_threaded = true` are
     * ever sharded across them.
     *
     * Two rules a multi-threaded system must obey, or it will corrupt state
     * in ways no test reliably catches:
     *   - touch nothing but its own query fields and read-only singletons;
     *   - never use mye_frame_allocator() -- the frame arena is a bump
     *     pointer with no synchronisation, so concurrent use corrupts it.
     * See plan/05-concurrency.md. */
    int worker_threads;

    /* Fixed simulation step in seconds. Default 1/60. */
    float fixed_dt;
    /* Cap on fixed steps per frame; prevents a long stall from triggering an
     * ever-growing catch-up. Default 5. */
    int max_steps_per_frame;

    /* No window, no OpenGL, no audio device -- for headless tests. The ECS
     * world and every non-rendering system still run normally. */
    bool headless;

    /* Starts flecs' REST server, which serves the world to the flecs
     * Explorer: a live view of every entity, component value, system and
     * query in the running game, editable while it runs.
     *
     *   https://www.flecs.dev/explorer/?host=localhost:27750
     *
     * Debug builds default this on; Release ignores it -- a shipped game
     * should not run an HTTP server. Override with MYE_EXPLORER=0 or 1. */
    bool explorer;
    uint16_t explorer_port; /* default 27750 */

    /* Stop after this many frames (0 = run until the window closes). The
     * MYE_MAX_FRAMES environment variable overrides this, which lets any
     * example double as an automated smoke test:
     *     MYE_MAX_FRAMES=120 ./example_01_bounce
     * The run then reaches mye_shutdown normally and reports leaks. */
    uint64_t max_frames;

    /* Write a PNG of the final frame here before exiting. Set by the
     * MYE_SCREENSHOT environment variable, which makes any example verifiable
     * without a human watching:
     *     MYE_MAX_FRAMES=60 MYE_SCREENSHOT=out.png ./example_03_scene3d
     * Requires max_frames, since it fires on the last frame. */
    const char *screenshot_path;
} mye_config;

/* Creates the world, opens the window (unless headless), and imports the
 * core module. Returns NULL on failure. */
ecs_world_t *mye_init(const mye_config *config);

/* Runs one complete frame and returns what ecs_progress() returns:
 *
 *   1. poll input                          (raylib -> MyeInput)
 *   2. run MyeOnFixedUpdate systems N times (fixed timestep, N may be 0)
 *   3. run the main pipeline               (EcsPreFrame .. EcsOnStore)
 *
 * Use this instead of ecs_progress() -- fixed-timestep systems and input
 * polling both live outside the main pipeline, because flecs does not allow
 * running a pipeline from inside one. */
bool mye_progress(ecs_world_t *world, float dt);

/* Tears everything down in reverse order and reports allocator leaks.
 * Returns 0 when clean, 1 when leaks were detected -- so a test or example
 * can `return mye_shutdown(world);` and have leaks fail the run. */
int mye_shutdown(ecs_world_t *world);

/* Creates an entity that respects the active scene, so unloading the scene
 * deletes it. Use this instead of ecs_new() for anything a scene owns.
 *
 * The distinction is a flecs detail worth knowing: ecs_new() is a bare id
 * allocation that bypasses ecs_set_with(), which is the mechanism scenes use
 * to claim new entities. ecs_entity(world, {...}) applies it; ecs_new() does
 * not. Every engine spawn helper goes through this. */
ecs_entity_t mye_entity_new(ecs_world_t *world);

/* Convenience accessors. */
mye_engine *mye_engine_get(const ecs_world_t *world);
/* Per-frame scratch allocator. Everything from it is invalidated at the top
 * of the next frame -- never store these pointers in components. */
mye_allocator mye_frame_allocator(const ecs_world_t *world);
/* Long-lived engine allocator. */
mye_allocator mye_allocator_of(const ecs_world_t *world);

/* True while the window is open. Always true in headless mode, where the
 * caller decides when to stop. */
bool mye_running(const ecs_world_t *world);

/* Monotonic seconds, independent of raylib. raylib's GetTime() only works
 * once InitWindow has started its timer, so it returns 0 forever in headless
 * mode -- which silently zeroes any headless benchmark. */
double mye_time_now(void);

#endif /* MYE_CORE_ENGINE_H */
