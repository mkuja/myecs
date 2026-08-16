# 02 — ECS Design (flecs)

We use [flecs](https://github.com/SanderMertens/flecs) v4 (pure C99,
single-header option available) as the ECS core. It is archetype-based like
Bevy's ECS: entities with identical component sets are stored in contiguous
tables, so systems iterate over dense arrays.

## Bevy → flecs mapping

If a tutorial or idea is described in Bevy terms, translate with this table:

| Bevy concept | flecs equivalent | Notes |
|---|---|---|
| `World` | `ecs_world_t` | Create with `ecs_init()` |
| `#[derive(Component)]` | `ECS_COMPONENT(world, T)` | Registers a plain C struct |
| System (`fn sys(query: Query<...>)`) | `ECS_SYSTEM(world, fn, Phase, Pos, Vel)` | The signature *is* the query |
| `Query<(&mut Pos, &Vel)>` | `ecs_query_t` / system terms | `ecs_field(it, Pos, 0)` in the callback |
| Schedule stages / system sets | Pipeline phases (`EcsOnLoad` … `EcsOnStore`) | Custom phases possible; ordering via `DependsOn` |
| `Res<T>` / `ResMut<T>` (resources) | Singleton: `ecs_singleton_set(world, T, {...})` | A component stored on its own entity |
| `Events<T>` / `EventReader` | Observers: `ecs_observer` + `ecs_emit` | Also fire on component add/set/remove |
| `Commands` (deferred ops) | Automatic: structural changes inside systems are deferred until merge | `ecs_defer_begin/end` for manual control |
| Plugin | Module: `ECS_MODULE` + `ECS_IMPORT` | Our engine subsystems are modules |
| `Parent` / `Children` | `EcsChildOf` relationship: `ecs_add_pair(w, child, EcsChildOf, parent)` | Deleting a parent deletes children by default |
| Bundles | Prefabs (`EcsPrefab`) or plain helper functions | `ecs_add_pair(w, e, EcsIsA, prefab)` |
| `Changed<T>` / change detection | `ecs_query_desc_t` change detection, `flecs.pipeline` handles system ordering | Also observers on `OnSet` |
| `bevy_reflect` / scenes | flecs reflection + JSON serialization | Used for scene save/load in M6 |
| Rayon-parallel systems | `ecs_set_threads(world, n)` + `.multi_threaded = true` on a system | flecs shards matched tables across workers |

## Core components (defined by the engine)

All plain C structs, registered by engine modules. Sketch (names final at
implementation time):

```c
// scene module — transforms
typedef struct { float x, y; }            MyePosition2D;
typedef struct { float angle; }           MyeRotation2D;    // radians
typedef struct { Vector3 v; }             MyePosition3D;    // raylib Vector3
typedef struct { Quaternion q; }          MyeRotation3D;
typedef struct { float x, y, z; }         MyeScale;
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
`MyeInput` (action states, see input module), `MyeAssetDb` (registries).

## Phase layout

Systems register into flecs' builtin phases as described in
[01-architecture.md](01-architecture.md). Conventions:

- **Gameplay systems** (game-defined) go in `EcsOnUpdate` and, where they need
  fixed timestep, read `MyeTime.fixed_dt` and run inside the accumulator
  pattern (helper: `mye_fixed_system()` wrapper, designed in M2).
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
- **Tags for state**: empty components (`MyeHidden`, `MyeActiveScene`) instead
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
