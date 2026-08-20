# 07 — Roadmap

Each milestone ends with something runnable, and its **definition of done
(DoD)** includes the tests required by [09-testing.md](09-testing.md).
Relative size: S ≈ an evening or two, M ≈ a week of evenings, L ≈ several
weeks.

**Ordering note:** memory allocators were moved to the front (they were
originally scheduled after the first 2D game). The reason is the engine rule
in [04-memory.md](04-memory.md): *every engine interface that allocates takes
a `mye_allocator`*. That rule is cheap to follow from the first line of engine
code and expensive to retrofit, so the allocator library must exist before any
subsystem API is designed.

| # | Milestone | Status |
|---|---|---|
| M0 | Hello window | **done** |
| M1 | Core memory | **done** |
| M2 | ECS loop | **done** |
| M3 | First complete 2D game | **done** |
| M4 | Asset manager | **done** |
| M5 | 3D scenes | **done** |
| M6 | Scene system | **done** |
| M7 | Parallelism & polish | **done** |
| M8 | Web build (WebAssembly) | **done** (tier-1 reload) |

## M0 — Hello window (S) ✅

Prove the toolchain.

- CMake project per [08-build.md](08-build.md): warnings-as-errors,
  FetchContent pins (raylib 6.0, flecs v4.1.6), Debug ASan/UBSan.
- `examples/00_hello`: window, clear color, `DrawFPS`.
- CTest wired up; `tests/mye_test.h` harness + its smoke test.

**DoD**: ✅ `cmake --build` + `ctest` green from a fresh tree; window runs.

## M1 — Core memory (S/M) ✅

The allocator library, before any subsystem exists to be retrofitted.

- `mye_allocator` interface (vtable + ctx, passed by value) — the type every
  later engine API accepts.
- Backends: heap (malloc), arena (bump, mark/rewind, top-block rollback),
  pool (intrusive free list), tracking decorator (leak + high-water report).
- `MYE_NEW` / `MYE_NEW_ARRAY` / `MYE_DELETE` convenience macros.

**DoD**: ✅ 18 unit tests in `tests/unit/test_alloc.c` covering alignment,
exhaustion→NULL, reset/rewind, in-place resize, LIFO rollback, pool reuse and
ownership, tracking counts and leak detection — clean under ASan/UBSan.

## M2 — ECS loop (S/M) ✅

Prove flecs + raylib together, with allocators wired in from the start.

- `mye_init(&(mye_config){ .allocator = ..., .width = ... })` /
  `mye_shutdown` — engine owns an allocator, never a global.
- Sized-header adapters (`mye_alloc_hdr` family) so third-party APIs that free
  without a size — flecs `ecs_os_api`, later raylib `RL_MALLOC` — still route
  through our allocator and show up in tracking.
- `MyeCore` module: `MyeTime` singleton, frame arena reset in `EcsPreUpdate`.
- `examples/01_bounce`: 1000 entities with position/velocity, Move + Bounce
  systems in `EcsOnUpdate`, draw system in `EcsOnStore`.
- flecs REST/Explorer in debug builds for live world inspection.

**DoD**: ✅ 1000 movers in `examples/01_bounce`; `tests/integration/test_int_ecs_loop.c`
(5 headless tests: startup/shutdown, movement over 100 frames, time singleton,
frame-arena reclamation, repeated init/shutdown) plus a `render`-labeled smoke
test rendering 30 frames through a hidden window. Every test asserts
`mye_shutdown() == 0`, i.e. the tracking allocator found no leaks — which
covers everything flecs allocated.

Notes for later:

- flecs 4 wants the **`EcsSingleton` trait** on a component
  (`ecs_add_id(world, ecs_id(T), EcsSingleton)`), after which systems name the
  component directly. The `T($)` DSL source syntax aborts at registration.
- Do not name engine functions `flecs_*` — flecs uses that prefix for internal
  macros and the collision produces baffling errors.
- Set `MYE_MAX_FRAMES=N` to make any example exit after N frames, so windowed
  examples can be leak-checked without a human watching.

## M3 — First complete 2D game (M) ✅

The "you've made a game" milestone. An Asteroids-like: ship, thrust, bullets,
splitting rocks, score, restart.

- Input module: action mapping (`MyeInput` singleton).
- Fixed-timestep accumulator + render interpolation.
- Sprite rendering with atlas + layer/y sorting; `Camera2D`; text.
- Sprite flipbook animation.
- 2D collision: circle/AABB overlap → collision events. *(Shipped
  engine-side 2026-08-20 as `engine/collision`; until then the example did
  its own overlap test in game code.)*
- Audio module: sfx + music. *(Music -- streaming, `mye_music` -- shipped
  2026-08-20; M3 itself shipped sfx only.)*
- Sync asset loading (minimal registry from [06-assets.md](06-assets.md)).
- Prefabs for bullets/rocks.

**DoD**: ✅ `examples/02_asteroids` is playable start→game-over→restart.
Game logic lives in `asteroids.c` (a library) so `test_int_asteroids.c` can
drive it headlessly with synthetic input: 7 tests covering firing and bullet
expiry, rocks splitting and scoring, life loss, game over, restart, thrust and
screen wrap, wave progression. Plus 8 input unit tests and 6 fixed-step
integration tests including determinism across irregular frame pacing.

Follow-ups completed after M4 (in the order agreed with the user):

- **raylib RL_MALLOC hookup** — every raylib allocation now routes through
  `mye_allocator`. See the traps documented in
  [04-memory.md](04-memory.md); `tests/unit/test_rl_alloc.c` guards it.
- **Audio** (`engine/audio/`) — sounds are *queued* and flushed once per
  frame, so several fixed steps in one frame collapse into a single play
  rather than stacking into a blast. Sounds are synthesized in code
  (`mye_sound_from_wave`), so the example still ships no binary assets.
- **Sprite animation** — `MyeSpriteAnim` flipbook over an atlas grid, with
  pure `mye_atlas_frame` / `mye_sprite_anim_advance` functions underneath so
  the frame maths is unit tested headlessly. Asteroids now spawns an animated
  explosion when a rock breaks.

Notes for later:

- **flecs 4 COPIES prefab components to instances by default**
  ((OnInstantiate, Override)). Sharing is opt-in per component type via
  (OnInstantiate, Inherit), and the trait must be applied *before* any query
  references the component or flecs aborts.
- Sharing was tried and **deliberately backed out**: a shared field is a
  single value, not a per-entity array, so every system reading it must branch
  on `ecs_field_is_self()` and index `[0]` instead of `[row]`. Missing that is
  an out-of-bounds read (ASan caught it in `SpinRocks`). That burden on every
  reader is not worth a few hundred bytes at this entity count -- revisit only
  for tens of thousands of identical entities, and fix every reader then.
- An animation is **finished only after the last frame has had its display
  time**, not when it is reached -- otherwise an explosion despawns before its
  final frame is ever drawn.
- `ecs_iter_fini()` must ONLY be called when breaking out of an iteration
  early. A loop that runs until `ecs_query_next()` returns false is already
  finalised, and finalising twice aborts.
- A custom phase tagged `EcsPhase` is picked up by the *builtin* pipeline.
  `MyeOnFixedUpdate` deliberately omits that tag, or every fixed system would
  also run once per frame in the main pipeline.
- `ecs_run_pipeline()` cannot be called from inside a running pipeline, which
  is why `mye_progress()` owns the frame instead of a driver system.
- Queries are built once and stored, never per frame or per step -- creating
  one allocates.

## M4 — Asset manager (M) ✅

- Full handle registry ([06-assets.md](06-assets.md)): generations, dedupe,
  refcount, placeholder-on-stale. Slot storage is plain fixed arrays -- the
  planned `mye_pool` backing was never adopted, and the pool currently has no
  consumer at all (see plan/15-gaps.md).
- Worker pool (C11 threads) + `mye_channel` on Concurrency Kit
  ([05-concurrency.md](05-concurrency.md)).
- ~~Async load~~ — removed later: see plan/06-assets.md. Loading happens at scene boundaries.
- Loading-screen support: a scene load is the pause, so the screen is drawn around it.

**DoD**: ✅ `tests/integration/test_int_assets.c` — 5 headless registry tests
covering dedupe with refcounting, generation-checked stale handles, missing
files, and named generated textures. Channel and job-pool unit tests (13
more). All three suites TSan-clean.

The original DoD here was the async pipeline, which was later removed
(see plan/06-assets.md): assets load at scene boundaries instead.

Notes for later:

- The channel is a **mutex-protected ring buffer**, not lock-free: a handful
  of messages per frame is not a hot spot, and it is multi-producer with
  messages that must not be lost. See the primitive policy in
  [05-concurrency.md](05-concurrency.md).
- The **tracking allocator's counters are atomic**. TSan found 30 races there
  the moment asset workers started allocating — an allocator shared across
  threads is a real hot spot, so atomics (relaxed, with a CAS loop for the
  peak) rather than a mutex that would serialise every allocation.
- Dedupe must match slots in the **LOADING** state too, or a second request
  for an in-flight path starts a duplicate decode and upload.
- Releasing a handle mid-flight must be allowed: it bumps the generation, and
  the upload system discards the late message on the generation check.
- TSan cannot instrument glibc's C11 `<threads.h>` (`DEADLYSIGNAL` under both
  gcc and clang), which is why `engine/core/thread.h` uses pthreads on POSIX.

## M5 — 3D scenes (M/L) ✅

- 3D transform components + `MyeWorldTransform`; parent→child propagation via
  `EcsChildOf` cascade queries, shared by 2D and 3D.
- `MyeCamera3D` + fly/orbit controllers.
- `MyeMeshInstance` rendering (glTF/OBJ). **Note:** raylib 6.0 redesigned the
  `Model`/`ModelAnimation`/`ModelSkeleton` structs — check the 6.0 changelog
  before writing this module; pre-6.0 tutorials will not match.
- Basic lighting (rlights-style shader, `MyeLight` components).
- `examples/03_scene3d`: 3D scene with a transform hierarchy and a 2D HUD,
  proving mixed rendering.

**DoD**: ✅ `examples/03_scene3d` -- a tank whose turret and barrel are
parented, a spinning planet with bobbing moons, two directional lights, and a
2D HUD composited over the 3D scene. 10 headless transform tests
(`test_int_transform.c`) covering TRS composition, parent translation /
rotation / scale, 8-deep chains resolving in a single frame, reparenting,
cascade deletion, and 2D entities in the same hierarchy.

Verified visually, not just by exit code: `MYE_SCREENSHOT=out.png` captures
the final frame, and the HUD prints the turret's computed world position
(-6.00, 1.40, 0.00 = body y 0.5 + turret local y 0.9) as a numeric check on
the hierarchy.

Notes for later:

- Draw order is an explicit **phase chain** in `engine.h` (`EcsOnStore` ->
  `MyeOnDraw3D` -> `MyeOnDraw2D` -> `MyeOnDrawUI` -> `MyeOnRenderEnd`), not
  registration order, so any module or game can register a draw system at any
  time and still land in the right place.
- Propagation uses **EcsCascade** on the parent term, which iterates
  breadth-first. Without it a deep hierarchy would settle one level per frame
  and the tip would visibly lag the root.
- The parent's world transform is a **shared field, not an array**: read
  `parent->m` once per table, never `parent[i]`.
- raylib's `TakeScreenshot` prepends its own `basePath`, mangling absolute
  paths. The engine uses `LoadImageFromScreen` + `ExportImage` instead.
- The lighting shader is the engine's own: raylib ships `rlights.h` under
  `examples/`, not in the library.

## M6 — Scene system (M) ✅

- `mye_scene_load/unload/switch`; scene arena; scene-owned entities and
  assets (release on unload).
- Prefab library pattern for spawnables.
- Serialization via flecs JSON reflection.
- Menu→game→menu flow in the M3 game. *(Shipped 2026-08-20: Asteroids now
  runs a menu scene and a play scene through the scene system.)*

**DoD**: ✅ `engine/scene/scene.[ch]` and `engine/scene/serialize.[ch]`.
16 headless tests: 10 for scene lifecycle (deferred switching, entity
ownership, unload callbacks, reload, shared-asset survival, 20 switch cycles
leaking nothing) and 6 for serialization (reflection coverage, world to JSON,
file round trip with nested structs, malformed input, scene-subset saves).

Notes for later:

- **Scene ownership is explicit, not `ecs_set_with`.** flecs' with-id is
  designed for a bracketed burst of creations; it also does not apply to bare
  `ecs_new()`, which is the natural call. `mye_entity_new()` tags the entity
  with `(MyeSceneOf, owner)` directly, and every engine spawn helper uses it.
  Raw `ecs_new()` therefore escapes scene ownership -- documented and pinned
  by a test rather than left as a surprise.
- Unloading works by deleting the scene's owner entity: flecs'
  `(OnDeleteTarget, Delete)` policy removes everything tagged with it. The
  engine keeps no entity lists.
- `MyeSceneOf` is marked `(OnInstantiate, DontInherit)`, or a prefab instance
  would be owned by whichever scene defined the prefab.
- Switching is applied at the frame boundary in `mye_progress`, never
  mid-frame: deleting entities while systems iterate them is a crash.
- Assets are scoped by scene and released refcounted, so an asset two scenes
  share survives the first unload.
- `ecs_iter_to_json` needs **`serialize_table = true`** to write an entity's
  actual components; without it only the matched query term appears and
  entities come back empty.

## M7 — Parallelism & polish (M) ✅

- Profile first (debug overlay timings, `ecs_progress` stats).
- Enable flecs workers (`ecs_set_threads`); mark eligible systems
  `multi_threaded` per the [05-concurrency.md](05-concurrency.md) policy.
- TSan build config; fix what it finds.
- Stress demo: 50k+ entities, 2D and 3D.
- README with build/run instructions and a module overview.

**DoD**: ✅ measured, TSan-clean, and determinism proven bit-identical.

`examples/04_stress` benchmarks headless simulation at a configurable entity
count and thread count. Release build, 16 cores:

| Entities | 1 thread | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| 50,000 | 0.77 ms | 0.39 | 0.19 | 0.10 | **0.07** |
| 200,000 | 2.85 ms | 1.46 | 0.75 | 0.39 | **0.27** |

Roughly 10.5-11x on 16 cores -- near-linear. But the crossover matters more
than the peak: at 1,000 entities threading breaks even, and at 100 entities
8 threads is **4x slower** than one. Workers stay off by default.

`tests/integration/test_int_workers.c` proves the part that matters more than
speed: a worker-threaded run produces **bit-identical** results to a
single-threaded one, and the test also asserts the system really was sharded
(callback invocation count rises), so it cannot pass by threading silently
never engaging.

Notes for later:

- The **atomic tracking counters from M4 are what make this safe**. Enabling
  workers today is TSan-clean only because that race was already fixed.
- **`mye_frame_allocator()` must never be used from a multi_threaded system**:
  the frame arena is an unsynchronised bump pointer. Documented in
  `mye_config.worker_threads` and the README.
- raylib's `GetTime()` returns 0 forever without a window, which silently
  zeroes any headless benchmark. `mye_time_now()` uses `timespec_get`.

## M8 — Web build (WebAssembly) (M) ✅

Full design in [10-web.md](10-web.md). Both foundations already support it:
raylib has a first-class `Web` platform, flecs compiles under emscripten. The
substantive work is inverting the main loop (browsers cannot block, so the
frame becomes a callback), shipping single-threaded first (emscripten threads
need cross-origin isolation), and asset packaging -- which Asteroids does not
need at all, since it generates its art and audio in code.

**Done**: all five examples compile to WebAssembly, and
`tools/web_dev.py` serves them with rebuild-and-reload on save (measured:
2.1 s from touching a `.c` file to a new build id).

The plan's bet paid off exactly as written -- **ASYNCIFY meant zero engine
and zero example changes**, and the two graceful paths built earlier
absorbed the rest: `MYE_THREADS_NONE` makes `mye_jobs_create` fail, which
falls through to synchronous asset loading, which was already tested.

Notes:

- **raylib's web backend needs GNU extensions.** `EM_ASM` is rejected under
  `-std=c11` ("use -std=gnu* modes instead"). `C_EXTENSIONS ON` is granted to
  the raylib target alone, so first-party code stays strict ISO C11 -- the
  same shape as `-Werror` applying only to our targets.
- **WebGL 2** (`OPENGL_VERSION "ES 3.0"`), not raylib's WebGL 1 default, so
  the GLSL ES 300 prologue is the only shader difference.
- Sizes: 1.4-1.7 MB wasm per example, ASYNCIFY included.
- The dev server sends COOP/COEP already, so enabling pthreads later is a
  build flag rather than a redeployment.

Tier 2 of [11-web-dev-loop.md](11-web-dev-loop.md) -- snapshot the world to
JSON before a reload, restore after -- **shipped** (2026-08-20): a rebuild no
longer restarts the game. Known limits are recorded in 11-web-dev-loop.md.

Two habits to keep so this stays cheap: never add a second blocking loop, and
keep every path working with zero workers.

## After M8 (Tier 3, unscheduled)

Custom shaders/post-fx · particles · physics integration · Lua scripting ·
editor tooling. Revisit after shipping a second small game. (Skeletal
animation was on this list and has since shipped: `MyeModelAnimator`, demoed
by the showcase's fox.)
