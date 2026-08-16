# 01 — Architecture

## Layers

```text
┌─────────────────────────────────────────────┐
│ game / examples        (your gameplay code) │
├─────────────────────────────────────────────┤
│ engine modules   render2d · render3d ·      │
│ (flecs modules)  scene · asset · input ·    │
│                  audio · time · debug       │
├───────────────────────┬─────────────────────┤
│ ECS: flecs            │ core: alloc · log · │
│ (world, systems,      │ math · containers · │
│  queries, pipeline)   │ channels            │
├───────────────────────┴─────────────────────┤
│ platform: raylib  (window · GL · input ·    │
│                    audio · file I/O)        │
└─────────────────────────────────────────────┘
```

Dependency rule: **arrows point down only.** Core knows nothing about the ECS.
Engine modules use core + flecs + raylib. Games use engine modules and never
call raylib directly (exception: examples may, while the engine is young).

## Engine modules are flecs modules

Each engine subsystem is a flecs *module*: a function that registers its
components, systems and singletons into the world.

```c
// engine/render/render2d.h
void MyeRender2dImport(ecs_world_t *world);

// usage in a game
ECS_IMPORT(world, MyeRender2d);
```

A game is then: create world → import engine modules → register game
components/systems → load a scene → run the loop. Adding a capability to a
game = importing a module.

Naming: public engine symbols use the `mye_` / `Mye` prefix (my-ecs) to avoid
collisions with raylib and flecs.

## Repository layout

```text
myecs/
├── CMakeLists.txt
├── cmake/                  # FetchContent pins, warning flags, sanitizers
├── plan/                   # these documents
├── engine/
│   ├── core/               # alloc.[ch], log.[ch], math2d/3d helpers,
│   │                       # containers (only what we need), channel wrapper
│   ├── render/             # render2d.[ch], render3d.[ch], camera, sorting
│   ├── scene/              # scene lifecycle, transform propagation
│   ├── asset/              # handles, registries, (later) async loader
│   ├── input/              # action mapping
│   ├── audio/              # sound/music playback systems
│   └── debug/              # overlay, gizmos
├── examples/
│   ├── 00_hello/           # M0: window + FPS
│   ├── 01_bounce/          # M1: 1000 bouncing sprites
│   ├── 02_game2d/          # M2: the first complete 2D game
│   └── 03_scene3d/         # M5: 3D scene demo
└── tests/
    ├── unit/               # test_<module>.c  (headless)
    └── integration/        # feature tests    (headless + labeled render smokes)
```

Third-party code (raylib, flecs, later ck/TLSF) is fetched by CMake
FetchContent into the build tree — nothing vendored in the repo
(see [08-build.md](08-build.md)).

## The main loop

raylib owns the window; flecs owns the frame. The outer loop is trivial and
lives in the game executable:

```c
int main(void) {
    ecs_world_t *world = mye_init(&(mye_config){
        .width = 1280, .height = 720, .title = "game",
    });                                  // InitWindow + import engine modules

    game_setup(world);                   // game components, systems, scene

    while (!WindowShouldClose()) {
        ecs_progress(world, GetFrameTime());   // runs ALL systems, in phases
    }
    return mye_shutdown(world);          // ecs_fini + CloseWindow
}
```

`ecs_progress()` executes every system in pipeline-phase order. The frame is
structured by flecs' builtin phases:

| Phase | What runs there | Threading |
|---|---|---|
| `EcsOnLoad` | Poll input → update `MyeInput` singleton | main |
| `EcsPreUpdate` | Fixed-timestep accumulator, timers | main |
| `EcsOnUpdate` | **Gameplay simulation** (movement, AI, collision) | workers (later) |
| `EcsPostUpdate` | Transform propagation (parent→child), camera follow | workers (later) |
| `EcsPreStore` | Visibility/sort preparation for rendering | main |
| `EcsOnStore` | **All rendering**: `BeginDrawing` … `EndDrawing` | **main only** |

Rendering-in-a-phase keeps the whole engine on one clock — there is no
separate render loop to synchronize with (see [03-rendering.md](03-rendering.md)
and the main-thread constraint in [05-concurrency.md](05-concurrency.md)).

### Fixed timestep

Simulation systems that need determinism/stability (physics-ish movement,
collision) run on a fixed step (default 1/60 s) via an accumulator singleton
updated in `EcsPreUpdate`; rendering interpolates between the previous and
current simulated state. Pattern: "Fix Your Timestep" (Gaffer on Games).
Detailed design lands with M2; determinism is a test target
([09-testing.md](09-testing.md)).

## Error handling & logging conventions

- Engine functions that can fail return `bool` or a handle whose validity can
  be checked; no `abort()` in library code (asserts are for programmer errors,
  compiled out in release).
- `mye_log_{trace,info,warn,error}` wraps raylib's `TraceLog` so engine, flecs
  (`ecs_os_api.log_`), and raylib all funnel into one sink.
- All public headers are C11-clean under
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`.
