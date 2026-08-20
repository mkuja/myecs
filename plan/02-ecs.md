# 02 — ECS Design (flecs)

We use [flecs](https://github.com/SanderMertens/flecs) v4 (pure C99,
single-header option available) as the ECS core. It is **archetype-based**:
entities with identical component sets are stored together in contiguous
tables, so a system iterating them walks dense arrays rather than chasing
pointers.

## The vocabulary, and what each idea is called here

| Idea | In this engine | Notes |
|---|---|---|
| the container for everything | `ecs_world_t` | created by `mye_init()` |
| a component type | `ECS_COMPONENT_DEFINE(world, T)` | a plain C struct; no base class, no interface |
| a system | `ECS_SYSTEM(world, Fn, Phase, Pos, Vel)` | the component list *is* the query |
| a query | system terms, or `ecs_query()` for standalone use | read fields with `ecs_field(it, Pos, 0)` |
| when a system runs | pipeline phases (`EcsOnLoad` … `EcsOnStore`) | custom phases via `DependsOn` |
| global state | singleton: `ecs_singleton_set(world, T, {...})` | a component stored on its own type's entity |
| events | observers: `ecs_observer` + `ecs_emit` | also fire on component add/set/remove |
| deferred changes | automatic inside systems | structural edits are queued until the merge point |
| a reusable subsystem | module: `ECS_MODULE` + `ECS_IMPORT` | the engine's own subsystems are modules |
| parent and child | `EcsChildOf` pair: `ecs_add_pair(w, child, EcsChildOf, parent)` | deleting a parent deletes its children |
| an entity template | prefab (`EcsPrefab`) + `EcsIsA` pair | components are copied to instances by default |
| reflection / saved scenes | flecs reflection + JSON serialization | scene save/load, M6 |
| data-parallel systems | `ecs_set_threads(world, n)` + `.multi_threaded = true` | flecs shards matched tables across workers |

## Core components (defined by the engine)

All plain C structs, registered by engine modules. Sketch (names final at
implementation time):

```c
// scene module — transforms
typedef struct { float x, y; }            MyePosition2D;
typedef struct { float angle; }           MyeRotation2D;    // radians
typedef struct { Vector3 v; }             MyePosition3D;    // raylib Vector3
typedef struct { Quaternion q; }          MyeRotation3D;
typedef struct { float x, y; }            MyeScale2D;   /* and MyeScale3D */
typedef struct { Matrix m; }              MyeWorldTransform; // computed, PostUpdate

// movement
typedef struct { float x, y; }            MyeVelocity2D;
typedef struct { Vector3 v; }             MyeVelocity3D;

// render2d module
typedef struct {
    mye_texture   texture;   // asset handle, see 06-assets.md
    Rectangle     source;    // atlas sub-rect
    Vector2       origin;
    Color         tint;
    int16_t       layer;     // sort key, back-to-front
} MyeSprite;

// render3d module
typedef struct { mye_model model; Color tint; } MyeMeshInstance;

// cameras (one active per scene; component on a camera entity)
typedef struct { Camera2D cam; bool active; } MyeCamera2D;
typedef struct { Camera3D cam; bool active; } MyeCamera3D;
```

Singletons (resources): `MyeTime` (dt, fixed-dt accumulator, frame count),
`MyeInput` (action states, see input module), `MyeAssets` (registries).

## Phase layout

Systems register into flecs' builtin phases as described in
[01-architecture.md](01-architecture.md). Conventions:

- **Gameplay systems** (game-defined) go in `EcsOnUpdate` and, where they need
  fixed timestep, read `MyeTime.fixed_dt` and run inside the accumulator
  pattern. (The planned `mye_fixed_system()` wrapper became something better:
  the `MyeOnFixedUpdate` phase, run by `mye_progress` in its own pipeline --
  register a system there and the accumulator is handled for you.)
- **Transform propagation** runs in `EcsPostUpdate`: a query with
  `MyeWorldTransform` + `?MyeWorldTransform(up ChildOf)` (parent term, using
  flecs' `cascade`/`up` traversal so parents are computed before children).
- **Render systems** run in `EcsPreStore` (sorting, culling) and `EcsOnStore`
  (draw calls, main thread only).

## Idioms to follow

- **Structural changes are deferred** inside systems (flecs does this
  automatically) — never assume an entity created in a system is queryable in
  the same phase.
- **Observers for reactions**: e.g. `OnSet MyeSprite` → resolve the texture
  handle; `OnRemove` scene tag → free scene-owned assets.
- **Tags for state**: empty components (`MyeHidden`; the planned
  `MyeActiveScene` tag became scene-ownership pairs instead -- see
  scene/scene.h) instead
  of booleans in components — queries can filter on them for free.
- **Prefabs for spawnables**: enemy/bullet templates as `EcsPrefab` entities;
  instantiate with `EcsIsA` pairs.
- **Names & debugging**: give important entities names (`ecs_entity(world, {
  .name = "player" })`); the flecs Explorer (REST API + browser UI,
  `FLECS_REST`) is enabled in debug builds for live world inspection —
  invaluable while learning.

## Testing hooks

The ECS layer is fully testable without a window: unit tests create a bare
`ecs_world_t`, import a module, spawn entities, call
`ecs_progress(world, dt)` N times, and assert component values. This is the
backbone of the integration tests too — see [09-testing.md](09-testing.md).
