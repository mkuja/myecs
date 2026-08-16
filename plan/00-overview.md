# 00 — Overview: Vision & Features

## What this engine is

A 2D + 3D game engine written in **strict C11** (no C++ anywhere in first-party
code), built as a static library plus example games, on top of two pure-C
foundations:

| Layer | Library | What it provides |
|---|---|---|
| Platform | [raylib](https://github.com/raysan5/raylib) (C99) | Window, OpenGL rendering (2D & 3D), input, audio, file loading |
| ECS | [flecs](https://github.com/SanderMertens/flecs) (C99) | Entities, components, systems, queries, hierarchies, observers, multithreaded pipeline |

The engine's job is everything *between* those layers and a game: scene
management, rendering systems, asset management, input mapping, memory
allocators, and a concurrency model — organized as ECS modules, each one a
function that registers its components and systems into the world.

**Guiding principles**

1. **Data-oriented**: game state lives in components; behavior lives in systems.
2. **Runnable at every milestone**: each roadmap step ends with something you
   can execute and see (see [07-roadmap.md](07-roadmap.md)).
3. **Single-threaded first, parallel later**: correctness before concurrency;
   flecs makes turning workers on nearly a one-liner
   (see [05-concurrency.md](05-concurrency.md)).
4. **Strict C**: C11, warnings-as-errors, sanitizers in debug
   (see [08-build.md](08-build.md)).
5. **Tested**: every module ships with unit tests; every finished feature ships
   with an integration test (see [09-testing.md](09-testing.md)).

## Feature enumeration (prioritized)

### Tier 1 — Core (must-have)

These define the minimum engine. Everything in Tier 1 is scheduled in
milestones M0–M5.

- **Game loop** — fixed-timestep simulation with interpolated rendering;
  driven by `ecs_progress()`.
- **ECS** — entities, components, systems, queries, parent-child hierarchies,
  singletons-as-resources (all via flecs; see [02-ecs.md](02-ecs.md)).
- **2D rendering** — sprites, texture atlases, text, `Camera2D`,
  layer + y-sorting (see [03-rendering.md](03-rendering.md)).
- **3D rendering** — meshes/models, materials, `Camera3D`, basic lighting.
- **Transforms** — local/world transforms with parent→child propagation.
- **Input mapping** — keyboard/mouse/gamepad mapped to named *actions*
  ("jump", "move_x"), not raw keycodes in gameplay code.
- **Asset management** — handle-based registry for textures, models, sounds,
  fonts; load/unload tied to scenes (see [06-assets.md](06-assets.md)).
- **Scenes** — load/unload/switch; a scene can be 2D, 3D, or mixed
  (3D world + 2D UI).
- **Memory allocators** — arena, frame/scratch, pool + debug tracking
  (see [04-memory.md](04-memory.md)).
- **Audio** — sound effects and music playback (raylib audio).

### Tier 2 — Important (second wave)

- **Async asset loading** — worker thread does file I/O + decode; main thread
  does GPU upload via a channel.
- **Events** — gameplay events via flecs observers / `ecs_emit`.
- **Prefabs** — reusable entity templates (flecs `EcsPrefab`).
- **Sprite animation** — flipbook animation from atlas frames.
- **2D collision** — AABB + circle overlap tests and collision events
  (not a physics engine).
- **Debug overlay** — FPS, frame time graph, entity/system counts, gizmos.
- **Scene serialization** — save/load world state via flecs JSON reflection.

### Tier 3 — Later / nice-to-have

Explicitly deferred; the architecture should not block them, but no design
work is spent on them now:

skeletal animation · custom shaders & post-processing · particle systems ·
3D physics (e.g. Jolt has no C API — candidate: ODE or a C wrapper) ·
editor tooling · scripting (e.g. Lua) · networking (now planned --
see [12-networking.md](12-networking.md)).

## Non-goals

- **Photorealistic rendering** — raylib's forward renderer is the ceiling.
- **Writing our own physics engine.**
- **C++ interop** — no C++ compilers involved, ever.
- **Building a general-purpose ECS** — flecs is the ECS; we build an engine.
- **Custom platform layers** — raylib owns windowing/GL/audio abstraction.

## Glossary (for a first-time game developer)

| Term | Meaning |
|---|---|
| **Entity** | An ID. Nothing more. Things in the game are entities. |
| **Component** | Plain data attached to an entity (`Position`, `Sprite`). No behavior. |
| **System** | A function that runs every frame over all entities matching a query ("all with Position and Velocity"). |
| **Query** | A filter selecting entities by which components they have. |
| **World** | The container holding all entities/components/systems (`ecs_world_t`). |
| **Resource / singleton** | Global data that exists once (e.g. `Time`, `InputState`); in flecs, a component set on itself. |
| **Frame** | One iteration of the game loop: read input → simulate → render. |
| **Delta time (dt)** | Seconds elapsed since the previous frame; multiply speeds by it so movement is framerate-independent. |
| **Fixed timestep** | Running simulation in constant increments (e.g. 1/60 s) regardless of render framerate — makes physics stable and deterministic. |
| **Draw call** | One command asking the GPU to draw something. Fewer = faster; batching merges many sprites into one call (raylib does this). |
| **Asset** | Data loaded from disk: texture, model, sound, font. |
| **Handle** | A small ID referring to an asset, instead of a raw pointer — stays valid-checkable after unloads. |
| **Archetype ECS** | ECS storage strategy grouping entities with identical component sets into contiguous tables — fast iteration. |
| **Scene** | A self-contained set of entities + the assets they need, loaded/unloaded as a unit (a level, a menu). |
| **Prefab** | A template entity that spawned entities copy/inherit from. |

## Document map

- [01-architecture.md](01-architecture.md) — layers, repo layout, main loop
- [02-ecs.md](02-ecs.md) — flecs usage, ECS vocabulary, core components
- [03-rendering.md](03-rendering.md) — 2D/3D rendering design
- [04-memory.md](04-memory.md) — allocator strategy
- [05-concurrency.md](05-concurrency.md) — parallelism & channels
- [06-assets.md](06-assets.md) — asset system
- [07-roadmap.md](07-roadmap.md) — milestones
- [08-build.md](08-build.md) — build system & dependencies
- [09-testing.md](09-testing.md) — unit & integration testing policy
