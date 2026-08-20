# myecs — the whole engine, one feature at a time

This is the only tutorial. It covers every feature the engine currently has:
what each one is, *why* it exists, and a working example of it. The last
section stitches all of them into one game you can run.

Nothing here assumes you have built a game before. It does assume you can read
C.

**Contents**

| | | |
|---|---|---|
| [0. Build and run](#0-build-and-run) | [8. Sprite animation](#8-sprite-animation) | [16. Saving and loading](#16-saving-and-loading) |
| [1. The world, the loop, and the shutdown](#1-the-world-the-loop-and-the-shutdown) | [9. The transform hierarchy](#9-the-transform-hierarchy) | [17. Concurrency](#17-concurrency) |
| [2. Entities, components, and systems](#2-entities-components-and-systems) | [10. Prefabs](#10-prefabs) | [18. Logging](#18-logging) |
| [3. Phases: when things run](#3-phases-when-things-run) | [11. Scenes](#11-scenes) | [19. The debug overlay and the flecs Explorer](#19-the-debug-overlay-and-the-flecs-explorer) |
| [4. The fixed timestep](#4-the-fixed-timestep) | [12. Input actions](#12-input-actions) | [20. Testing what you wrote](#20-testing-what-you-wrote) |
| [5. Render interpolation (opt-in)](#5-render-interpolation-opt-in) | [13. Audio](#13-audio) | [21. The web target](#21-the-web-target) |
| [6. Allocators](#6-allocators) | [14. 3D rendering](#14-3d-rendering) | [22. Networking](#22-networking) |
| [7. Assets and handles](#7-assets-and-handles) | [15. Skeletal animation](#15-skeletal-animation) | [23. Capstone: Orbit Collector](#23-capstone-orbit-collector) |

---

## 0. Build and run

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
ctest --test-dir build/debug
./build/debug/examples/example_06_tutorial
```

raylib and flecs are fetched by CMake; there is nothing to install first.

To add your own program, one line each in `examples/CMakeLists.txt`:

```cmake
add_executable(example_mygame mygame/main.c)
target_link_libraries(example_mygame PRIVATE mye_flags engine)
```

`mye_flags` turns on `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`
and, in Debug, the address and undefined-behaviour sanitizers. A warning stops
the build. This is deliberate: nearly every compiler warning in C is either a
bug or a lie you are telling the compiler.

Two environment variables make any program here verifiable without a human
watching it:

```sh
MYE_MAX_FRAMES=120 ./build/debug/examples/example_06_tutorial   # exit after 120 frames
MYE_MAX_FRAMES=120 MYE_SCREENSHOT=shot.png ./build/debug/examples/example_06_tutorial
```

---

## 1. The world, the loop, and the shutdown

**What it is.** Three calls: `mye_init` creates the ECS world and opens the
window, `mye_progress` runs one frame, `mye_shutdown` tears everything down and
returns a process exit code.

**Why it is.** A game needs somewhere to put its state and something to advance
it. Rather than invent a lifecycle, the engine keeps `main` short enough that
you can see the whole thing at once — the interesting code is in *systems*, not
in the loop.

`mye_shutdown` returns non-zero if the debug allocator saw a leak, so a leaked
allocation fails a test run rather than going unnoticed.

```c
/* hello.c -- the smallest complete program. */
#include "core/engine.h"

#include <raylib.h>

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .width = 800, .height = 450, .title = "hello",
    });
    if (world == NULL) return 1;

    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }
    return mye_shutdown(world);
}
```

`mye_running` is `!WindowShouldClose()` plus the frame limit, so `MYE_MAX_FRAMES`
works in every program without you writing anything.

Useful `mye_config` fields, all optional:

| Field | Default | Meaning |
|---|---|---|
| `width`, `height`, `title` | 1280×720, `"myecs"` | window |
| `target_fps` | 60 | 0 = uncapped |
| `allocator` | heap | every engine allocation comes from here |
| `frame_arena_bytes` | 1 MiB | per-frame scratch (§6) |
| `fixed_dt` | 1/60 | simulation step (§4) |
| `max_steps_per_frame` | 5 | catch-up clamp (§4) |
| `worker_threads` | 0 | parallel simulation (§17) |
| `headless` | false | no window, no GL, no audio — for tests (§20) |
| `explorer` | on in Debug | live world inspector (§19) |

---

## 2. Entities, components, and systems

**What it is.** An **entity** is an ID — nothing else. A **component** is a
plain C struct attached to an entity. A **system** is a function that runs over
every entity holding a given set of components.

**Why it is.** The alternative is a class hierarchy, and game objects do not
form one. An enemy that flies, a pickup that flies, and a bullet that flies do
not share an ancestor, but they share `Velocity`. Composition by component
means "make this thing fly" is one line, not a refactor.

The second reason is speed. flecs is an *archetype* ECS: entities with
identical component sets are stored together in contiguous arrays. A system
iterating positions walks a flat `float` array. That is why `it->count` exists
— your callback is handed a whole block at once, not one entity per call.

```c
/* movement.c -- entities, components, one system, no window. */
#include "core/engine.h"

#include <stdio.h>

typedef struct Position { float x, y; } Position;
typedef struct Velocity { float x, y; } Velocity;

ECS_COMPONENT_DECLARE(Position);
ECS_COMPONENT_DECLARE(Velocity);

static void Move(ecs_iter_t *it)
{
    Position *p = ecs_field(it, Position, 0);   /* term 0: writable */
    const Velocity *v = ecs_field(it, Velocity, 1);

    for (int i = 0; i < it->count; ++i) {       /* a block, not one entity */
        p[i].x += v[i].x * (float)it->delta_time;
        p[i].y += v[i].y * (float)it->delta_time;
    }
}

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .headless = true });
    if (world == NULL) return 1;

    ECS_COMPONENT_DEFINE(world, Position);
    ECS_COMPONENT_DEFINE(world, Velocity);

    /* The component list IS the query. */
    ECS_SYSTEM(world, Move, EcsOnUpdate, Position, [in] Velocity);

    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, Position, { 0.0f, 0.0f });
    ecs_set(world, e, Velocity, { 10.0f, 0.0f });

    for (int frame = 0; frame < 60; ++frame) mye_progress(world, 1.0f / 60.0f);

    const Position *p = ecs_get(world, e, Position);
    printf("x = %.2f\n", (double)p->x);         /* ~10.00 */
    return mye_shutdown(world);
}
```

Three things worth knowing immediately:

- `[in]` marks a term read-only. It is not decoration: read-only terms are what
  let flecs run systems in parallel later (§17).
- `mye_entity_new` rather than `ecs_new`. It tags the entity as belonging to
  the current scene so unloading a scene can delete it (§11).
- **Structural changes are deferred.** Inside a system, `ecs_delete` and adding
  or removing components are queued and applied at the next sync point — in
  practice the end of the frame, or the end of each fixed-timestep run. An
  entity you create in a system is not queryable immediately. This is what
  makes it safe to delete entities while iterating them.

### Singletons

A singleton is a component stored on its own type's entity: one instance,
world-wide. Use it for score, settings, and anything else there is exactly one
of.

```c file
typedef struct Score { int points; } Score;
ECS_COMPONENT_DECLARE(Score);
```

```c ctx
ECS_COMPONENT_DEFINE(world, Score);
ecs_add_id(world, ecs_id(Score), EcsSingleton);   /* flecs v4 needs this trait */

ecs_singleton_set(world, Score, { .points = 0 });
Score *score = ecs_singleton_ensure(world, Score);
score->points += 10;
```

A system can take a singleton as an ordinary term — `ECS_SYSTEM(world, Tick,
EcsOnUpdate, Score)` — and it matches once per frame.

---

## 3. Phases: when things run

**What it is.** Every system is registered into a *phase*. Phases run in a
fixed order each frame, so "input before movement before drawing" is a property
of the pipeline rather than of the order you happened to write your code in.

**Why it is.** Ordering bugs are the ones that reproduce once a week. Naming
the stage a system belongs to means a module can add a draw system at any time
and still land after the 3D pass and before `EndDrawing`.

A frame is two parts. `mye_progress` does a little work itself, then hands the
rest to flecs' pipeline:

| | What runs | Where |
|---|---|---|
| 1 | pending scene switch applied | `mye_progress`, not a system |
| 2 | input polled | `mye_progress`, not a system |
| 3 | **`MyeOnFixedUpdate`** — **your simulation**, run 0..n times (§4) | its own pipeline |
| 4 | everything below, once | `ecs_progress` |

Then, inside `ecs_progress`:

| Phase | What lives there |
|---|---|
| `EcsOnLoad` | `MyeTime` update |
| `EcsPreUpdate` | timers |
| `EcsOnUpdate` | per-frame gameplay: menus, scene switching, camera |
| `EcsPostUpdate` | transform propagation (§9) |
| `EcsPreStore` | sprite animation, interpolation blend, sorting |
| `EcsOnStore` | `BeginDrawing` + clear |
| `MyeOnDraw3D` | the 3D pass |
| `MyeOnDraw2D` | the world-space sprite pass |
| **`MyeOnDrawUI`** | **your HUD and menus**, screen space |
| `MyeOnRenderEnd` | `EndDrawing` |

The part that surprises people: **the fixed steps run before the whole main
pipeline, not in the middle of it.** Input is sampled outside the pipeline for
the same reason — the fixed steps need this frame's input, and flecs forbids
running a pipeline from inside one. So a system you register in `EcsOnLoad`
runs *after* this frame's simulation has already happened, not before it.

Drawing is split across phases rather than crammed into `EcsOnStore` so that a
HUD system cannot accidentally draw *underneath* the world. You register into
`MyeOnDrawUI` and the ordering takes care of itself.

```c file
/* Draws after the world, before EndDrawing -- regardless of registration order. */
static void DrawHud(ecs_iter_t *it)
{
    (void)it;
    DrawText("HP 100", 20, 20, 20, RAYWHITE);
}

static void register_hud(ecs_world_t *world)
{
    ECS_SYSTEM(world, DrawHud, MyeOnDrawUI, MyeRenderConfig);
}
```

The `MyeRenderConfig` term is there to make the system run exactly once per
frame: it queries a singleton, so it matches one entity. (flecs would also run
a term-less system once per frame, as a *task* — but naming the singleton
documents what the system depends on, and keeps it from running before the
render config exists.)

---

## 4. The fixed timestep

**What it is.** Simulation runs in constant increments — 1/60 s by default —
no matter how fast the display refreshes. `mye_progress` accumulates real
elapsed time and runs the `MyeOnFixedUpdate` phase as many whole steps as have
accumulated: sometimes 0, usually 1, sometimes 2.

**Why it is.** If you integrate with a variable `dt`, the same input produces
different results on different machines. A frame spike makes an object tunnel
through a wall; a 240 Hz monitor makes your jump height different from a 60 Hz
one. Constant steps make the simulation reproducible, which also makes it
testable — you can assert an exact position after N steps.

Inside a fixed system, `it->delta_time` is always exactly `fixed_dt`.

```c
/* fixedstep.c -- deterministic simulation, headless. */
#include "core/engine.h"

#include <stdio.h>

typedef struct Position { float x, y; } Position;
ECS_COMPONENT_DECLARE(Position);

static void Fall(ecs_iter_t *it)
{
    Position *p = ecs_field(it, Position, 0);
    for (int i = 0; i < it->count; ++i) {
        p[i].y += 100.0f * (float)it->delta_time;   /* always fixed_dt */
    }
}

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .headless = true, .fixed_dt = 1.0f / 60.0f,
    });
    if (world == NULL) return 1;

    ECS_COMPONENT_DEFINE(world, Position);
    ECS_SYSTEM(world, Fall, MyeOnFixedUpdate, Position);

    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, Position, { 0.0f, 0.0f });

    /* Ragged frame times; the total is one second either way. */
    for (int i = 0; i < 30; ++i) mye_progress(world, 1.0f / 30.0f);

    const MyeTime *t = ecs_singleton_get(world, MyeTime);
    printf("steps: %llu, y: %.2f\n",
           (unsigned long long)t->fixed_step, (double)ecs_get(world, e, Position)->y);
    /* 60 steps, y = 100.00 -- the same on any machine. */
    return mye_shutdown(world);
}
```

`max_steps_per_frame` (default 5) caps the catch-up. Without it, a machine that
cannot simulate as fast as real time falls further behind every frame, each
frame runs more steps than the last, and the game freezes solid — the "spiral
of death". With the clamp, a slow machine runs in slow motion instead, which is
survivable.

Rule of thumb: **movement, collision and physics go in `MyeOnFixedUpdate`.
Menus, cameras and anything reading raw input go in `EcsOnUpdate`.**

---

## 5. Render interpolation (opt-in)

**What it is.** Simulation at 60 Hz, display at 144 Hz, means the same
simulated position is shown for two or three consecutive frames — visible as
judder. `MyeTime.alpha` is how far the current frame sits between the last two
fixed steps. Add `MyeInterpolate` to an entity and the renderer draws it blended
between its previous and current position.

**Why it is opt-in.** With interpolation on by default, an entity's drawn
position would silently stop matching its `MyePosition2D` value. Someone
comparing the component in the Explorer against the screen would find a
discrepancy with no visible cause. The engine should not diverge data from
pixels behind your back — so you ask for it, per entity.

```c ctx
ecs_set(world, player, MyeInterpolate, { 0 });   /* that is the whole feature */
```

The one thing you must know: **interpolation blends between two positions, so
teleporting looks like a streak.** An entity wrapping from x=1270 to x=10 gets
drawn across everything in between. Tell the engine the movement was not
continuous:

```c file
/* Not named Wrap: raymath.h already defines one, and raylib's headers are
 * everywhere in a game. */
static void WrapAtEdges(ecs_iter_t *it)
{
    MyePosition2D *p = ecs_field(it, MyePosition2D, 0);
    for (int i = 0; i < it->count; ++i) {
        if (p[i].x < 0.0f)     { p[i].x += 1280.0f; mye_transform_snap(it->world, it->entities[i]); }
        if (p[i].x > 1280.0f)  { p[i].x -= 1280.0f; mye_transform_snap(it->world, it->entities[i]); }
    }
}
```

`mye_transform_snap` sets a flag the next blend consumes and clears. Calling it
on an entity without `MyeInterpolate` is harmless.

Interpolation composes through the hierarchy (§9). Each link blends its own
motion between the last two fixed steps, and the chain is multiplied together
at draw time into `MyeRenderTransform`, so a child stays rigidly attached to an
interpolated parent instead of trailing it. Blending each link separately is
what keeps the rate right: `MyeInterpolate` records where an entity was one
*step* ago, while transforms are composed once per *frame*, and a frame can run
two steps or none.

`MyeRenderTransform` is display-only and appears next to `MyeWorldTransform` in
the Explorer. The two agree exactly unless something in the entity's parent
chain interpolates, and then only while `alpha` is non-zero. Never read it
back as simulation state -- collision tested against it would make gameplay
depend on framerate, which is exactly what the fixed timestep exists to
prevent. Use `MyeWorldTransform` or `mye_world_position()` for anything that is
not drawing.

Interpolation is 2D-only: `MyeInterpolate` records `prev_x` and `prev_y`, and
3D entities are drawn at their un-blended world transform.

---

## 6. Allocators

**What it is.** Every engine interface that allocates takes a `mye_allocator`:
a vtable plus a context, passed **by value**. Four backends ship: heap, arena,
pool, and a tracking wrapper.

**Why it is.** Two reasons, and the second is the real one.

1. Different lifetimes want different strategies. Per-frame scratch should cost
   a pointer bump and be freed all at once. Fixed-size objects churned every
   frame want a free list, not `malloc`.
2. **It makes leaks a test failure.** In Debug the engine wraps its allocator
   in the tracking allocator, so `mye_shutdown` returns non-zero if anything is
   still live. You find out from `ctest`, not from a profiler six months later.

Note that free takes the size back:

```c file
void mye_free(mye_allocator a, void *ptr, size_t size);
```

That is not an oversight. Knowing the size at free time lets the arena and pool
avoid storing a per-allocation header, and the caller almost always knows it.
For the cases that genuinely do not — C libraries that call `free(p)` with no
size — there is a header-carrying adapter (`mye_alloc_hdr` and friends), which
is how raylib's allocations are routed through our tracking.

### The frame allocator

The one you will use most. A bump arena, reset at the top of every frame:
allocate, use it this frame, never free it.

```c file
static void DrawHud(ecs_iter_t *it)
{
    const Score *score = ecs_field(it, Score, 0);

    /* Reclaimed next frame. Nothing to free, nothing to leak. */
    char *line = MYE_NEW_ARRAY(mye_frame_allocator(it->world), char, 64);
    if (line == NULL) return;                    /* arena full: degrade, do not crash */

    snprintf(line, 64, "SCORE %d", score->points);
    DrawText(line, 20, 20, 22, RAYWHITE);
}
```

Set the capacity with `frame_arena_bytes`. Running out returns `NULL` rather
than growing, so a runaway allocation shows up as a missing HUD line instead of
silently eating memory.

**The frame arena is a bump pointer with no synchronisation.** Never touch it
from a multi-threaded system (§17).

### The others

```c ctx
/* Arena: many allocations, one bulk free. Good for level-load scratch. */
mye_arena arena;
mye_arena_init(&arena, mye_heap_allocator(), 64 * 1024);
mye_allocator a = mye_arena_allocator(&arena);

Thing *t = MYE_NEW(a, Thing);
mye_arena_reset(&arena);                 /* frees everything at once */
printf("peak %zu bytes\n", mye_arena_high_water(&arena));
mye_arena_deinit(&arena);

/* Pool: fixed-size slots, O(1) alloc and free, no fragmentation. */
mye_pool pool;
mye_pool_init(&pool, mye_heap_allocator(), sizeof(Particle),
               _Alignof(Particle), 4096);
Particle *p = mye_pool_alloc(&pool);     /* NULL when exhausted */
mye_pool_free(&pool, p);
mye_pool_deinit(&pool);

/* Tracking: wraps another allocator and counts. What Debug builds use. */
mye_tracking track;
mye_tracking_init(&track, mye_heap_allocator());
mye_allocator checked = mye_tracking_allocator(&track);
/* ... use `checked` ... */
if (mye_tracking_has_leaks(&track)) mye_tracking_report(&track, "assets");
```

`mye_arena_take_mark` / `mye_arena_rewind` give you a savepoint inside an arena
— useful for "try to build this, abandon it if it does not fit".

---

## 7. Assets and handles

**What it is.** Textures, models and sounds are referred to by small handles
(`mye_texture`, `mye_model`, `mye_sound`), not by pointers. The engine owns the
GPU objects; you own an ID.

**Why it is.** A `Texture2D` copied into a hundred sprite components is a
hundred chances to keep using it after an unload. A handle can be *checked* —
`mye_texture_valid` — and a stale one resolves to a placeholder rather than to
freed memory. Handles also make scene-scoped unloading possible (§11): the
registry knows every asset, so it can release a whole scene's worth at once.

```c
/* assets.c -- loading, generating, and drawing without touching a pointer. */
#include "asset/asset.h"
#include "core/engine.h"
#include "core/log.h"
#include "render/render2d.h"

#include <raylib.h>

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .title = "assets" });
    if (world == NULL) return 1;

    /* From a file. mye_asset_path makes the lookup independent of the
     * directory the program was started in -- a relative path alone works
     * only from the project root. */
    char path[1024];
    mye_asset_path("assets/player.png", path, sizeof path);
    mye_texture from_disk = mye_texture_load(world, path);

    /* Generated: the engine takes ownership of the Image and uploads it. */
    Image img = GenImageColor(32, 32, BLANK);
    ImageDrawCircle(&img, 16, 16, 15, SKYBLUE);
    mye_texture generated = mye_texture_from_image(world, "gen:disc", img);

    /* Named, so a second request for "gen:disc" returns the same handle
     * instead of uploading it twice. */
    mye_sprite_spawn(world, generated, 400.0f, 300.0f, WHITE);

    if (!mye_texture_valid(world, from_disk)) {
        mye_log_warn("no player.png; using the generated disc only");
    }

    while (mye_running(world)) mye_progress(world, GetFrameTime());
    return mye_shutdown(world);
}
```

### When to load

Loading blocks, and that is the design rather than a gap: **load at scene
boundaries** (§11). A scene's `load` function is the one place a pause is
acceptable, because that is what a loading screen is for, and the GPU upload
has to happen on the main thread regardless -- a worker could only ever do
the decode half.

Be clear about what that costs: a scene's `load` blocks the frame, so the
loading screen is a still image, not an animated one. Decoding a large PNG is
tens of milliseconds, and that is genuinely the bulk of a load -- the GPU
upload is the cheap part.

The engine used to decode on worker threads to hide exactly that, and the
capability was dropped on purpose. A worker can only do the decode, since the
upload is main-thread-only; the first web target is single-threaded because
threads there need cross-origin isolation; and a pipeline that exists for one
consumer is a concurrency story to maintain forever. Loading at a boundary you
control is the simpler trade.

If a game later needs assets *during* play, spreading loads over several
frames -- a few per frame from a list -- covers the common case. It is not a
universal answer: one asset whose decode exceeds a frame budget still hitches,
because there is no way to split a single load.

```c ctx
/* Inside a scene's load function: pull in everything it needs, once. The
 * scene system has already set the asset scope, so unloading the scene
 * releases these -- there is nothing to tag by hand. */
mye_texture ship = mye_texture_load(world, "assets/ship.png");
mye_texture rock = mye_texture_load(world, "assets/rock.png");
(void)ship;
(void)rock;
```

---

## 8. Sprite animation

**What it is.** `MyeSprite` draws a rectangle of a texture. `MyeSpriteAnim`
steps that rectangle through frames laid out in a grid — a flipbook.

**Why it is.** Every 2D character is a strip of images in one texture. Putting
the frames in one atlas keeps it to one GPU texture bind, and stepping a
rectangle is cheaper than swapping textures. The animation system runs in
`EcsPreStore`, before drawing, so the frame you see is always the one the
animation just chose.

```c
/* anim.c -- a 4-frame flipbook, generated so it needs no asset files. */
#include "asset/asset.h"
#include "core/engine.h"
#include "render/render2d.h"

#include <raylib.h>

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .title = "anim" });
    if (world == NULL) return 1;

    /* Four 24px cells, left to right, growing. */
    Image atlas = GenImageColor(24 * 4, 24, BLANK);
    for (int f = 0; f < 4; ++f) {
        ImageDrawCircle(&atlas, f * 24 + 12, 12, 4 + f * 2, LIME);
    }
    mye_texture tex = mye_texture_from_image(world, "gen:pulse", atlas);

    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, MyePosition2D, { 400.0f, 300.0f });
    ecs_set(world, e, MyeSprite,
            { .texture = tex, .origin = { 12.0f, 12.0f }, .tint = WHITE });
    ecs_set(world, e, MyeSpriteAnim,
            { .first_frame = { 0.0f, 0.0f, 24.0f, 24.0f },  /* cell 0 */
              .columns = 4, .frame_count = 4, .fps = 8.0f,
              .loop = true, .playing = true });

    while (mye_running(world)) mye_progress(world, GetFrameTime());
    return mye_shutdown(world);
}
```

`MyeSprite` fields:

| Field | Meaning |
|---|---|
| `texture` | handle from §7 |
| `source` | sub-rectangle; zero width means the whole texture |
| `origin` | pivot **in source pixels** — rotation happens about this |
| `tint` | multiplied with the texture; `WHITE` = unchanged |
| `layer` | sort key, higher draws in front |

For a one-shot animation set `loop = false` and watch `finished`. It is set
once the last frame has had its *full display time*, not merely when it is
reached, so despawning on `finished` always shows every frame:

```c file
static void DespawnFinished(ecs_iter_t *it)
{
    const MyeSpriteAnim *anim = ecs_field(it, MyeSpriteAnim, 0);
    for (int i = 0; i < it->count; ++i) {
        if (anim[i].finished) ecs_delete(it->world, it->entities[i]);
    }
}
```

Add `MyeHidden` to skip an entity when drawing without deleting it.

### The 2D camera

Sprites are drawn in **world** space, inside raylib's `BeginMode2D`. Which
camera that uses is an entity with `MyeCamera2D`. With one camera that is
all there is to it. With several — a main view and a minimap — name the main
one in `MyeRenderConfig.camera`; the others are yours to draw with. With none,
the pass falls back to an identity view, so a game without a camera still
draws.

A camera is an ordinary entity in the transform hierarchy: **where it is comes
from its position**, and the component only says what a camera *is* — zoom,
rotation, and where on screen its position lands.

```c ctx
/* Centred on the middle of the level, at 1:1. The offset defaults to the
 * centre of the window, so the camera's position is what you see in the
 * middle of the screen. */
camera = mye_camera2d_spawn(world, (Vector2){ 500.0f, 320.0f }, 1.0f);
```

`MyeOnDrawUI` runs outside `BeginMode2D`, which is why a HUD registered there
stays put while the world scrolls underneath it.

### Following something

Add `MyeCameraFollow` — no system to write:

```c ctx
ecs_set(world, camera, MyeCameraFollow,
        { .target = player,
          /* 0 snaps exactly; ~8 lags gently; ~20 is nearly rigid. The lag is
           * framerate-independent, so it looks the same at 30 and 240 fps. */
          .stiffness = 8.0f });
```

The lag matters more than it sounds: a camera welded to the player makes the
*world* look like it is sliding around, because the one fixed thing on screen
is the thing you are steering. A little slack reads as the camera chasing.

Two things this does that are easy to get wrong by hand:

- It follows the player's **drawn** position, not the simulated one. With
  interpolation on (§5) those differ mid-step, and following the simulated
  position makes the whole world shimmer against a player who is perfectly
  smooth.
- It runs in the `MyeOnCamera` phase, after transforms are propagated and
  blended. A follow system in `EcsOnUpdate` sees the player's *stepped*
  position — which mid-step is not where the sprite is drawn.

Write your own camera logic in that same phase when it **reads other entities
and writes the camera's own position**. Screen shake is the classic: rather
than nudging the camera itself (a random walk that never comes back), parent
the camera to a rig and jitter the camera's *offset from the rig*, decaying
to zero. The rig follows the player; the shake stays bounded:

```c file
typedef struct Shake { float strength; } Shake;   /* on the camera */

static void ShakeCamera(ecs_iter_t *it)
{
    MyePosition2D *local = ecs_field(it, MyePosition2D, 0); /* offset from rig */
    Shake *shake = ecs_field(it, Shake, 1);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        float s = shake[i].strength;
        local[i].x = (float)GetRandomValue(-100, 100) * 0.01f * s;
        local[i].y = (float)GetRandomValue(-100, 100) * 0.01f * s;
        shake[i].strength = s * (1.0f - 4.0f * dt); /* dies out in ~¼ s */
        if (shake[i].strength < 0.1f) shake[i].strength = 0.0f;
    }
}

static void register_shake(ecs_world_t *world)
{
    ECS_SYSTEM(world, ShakeCamera, MyeOnCamera, MyePosition2D, Shake, MyeCamera2D);
}
```

One rule worth knowing before it bites: in `MyeOnCamera`, writes to the
camera's **own** position land this frame; writes to something the camera is
*parented to* land next frame, because transforms were already propagated.
Move rigs in `EcsOnUpdate`; move cameras in `MyeOnCamera`.

### One consequence of cameras being ordinary entities

A camera has `MyePosition2D`/`3D` and `MyeRotation2D`/`3D` like anything
else — so a system that queries "everything with a rotation" will happily
spin your camera too. That is not a bug in the engine; it is the query being
broader than you meant. Match on a tag of your own (`[none] Spins`) rather
than on the transform components, and the camera stays out of it. The
showcase example had exactly this bug: its minimap camera was being yawed to
the horizon sixty times a second by a system meant for a boombox.

### Rigid attachment is just parenting

For a camera that is genuinely welded to something — a first-person view, a
turret sight — parent it and write nothing at all:

```c ctx
mye_set_parent(world, camera, player);
```

It now moves *and turns* with the player, because the hierarchy composes
rotation (§9), not just position.

### Screen and world

Clicking on things needs the reverse of drawing:

```c ctx
Vector2 mouse = GetMousePosition();
Vector2 in_world = mye_screen_to_world_2d(world, mouse);

/* And back, to put a marker on screen over a world object. */
Vector2 on_screen = mye_world_to_screen_2d(world, in_world);
(void)on_screen;
```

The 3D equivalents are `mye_screen_ray` (what to intersect for picking) and
`mye_world_to_screen`. A zero `zoom` would show nothing, so the engine treats
it as 1 rather than silently drawing a blank screen.

### Canvases: rendering somewhere other than the window

Everything above draws to the window. A **canvas** is an off-screen surface a
camera can draw into instead, and whose result is an ordinary texture you can
draw *with* — on a sprite, in the HUD, on a mesh. A security monitor showing
another room, a minimap on a dashboard, a rear-view mirror, picture-in-picture:
all of them are "a camera renders into a canvas, something else displays the
canvas."

A canvas is an entity, so `canvas` below is an `ecs_entity_t` like any other —
the snippets in this section share it the way the earlier ones share `world`
and `player`:

```c ctx
canvas = mye_canvas_create(world, "minimap", 256, 256, BLACK);
if (canvas == 0) {
    return; /* the name was taken, or the size was refused; it says which */
}

/* Point a camera at it. That one field is the whole difference between a
 * camera that draws on screen and one that does not. */
ecs_entity_t eye = mye_camera3d_spawn(world, (Vector3){ 0.0f, 300.0f, 0.0f },
                                      (Vector3){ 0.0f, 0.0f, 0.0f }, 45.0f);
MyeCamera3D *c = ecs_get_mut(world, eye, MyeCamera3D);
c->target = canvas;
ecs_modified(world, eye, MyeCamera3D);
```

Now show it. As a sprite:

```c ctx
ecs_entity_t shown = mye_entity_new(world);
ecs_set(world, shown, MyePosition2D, { 0.0f, 0.0f });
ecs_set(world, shown, MyeSprite,
        { .texture = mye_canvas_texture(world, canvas),
          .source = mye_canvas_source_rect(world, canvas),
          .tint = WHITE });
```

That `source` matters. Render textures are stored bottom-up, so drawn naively
they come out upside down; `mye_canvas_source_rect` returns the rect that
draws it the right way up, and you never have to remember which way that is.

Or on a mesh, which is how you get a screen *inside* the world:

```c ctx
mye_model quad = mye_model_from_mesh(world, "monitor",
                                     GenMeshCube(8.0f, 8.0f, 0.5f), WHITE);
ecs_entity_t monitor = mye_mesh_spawn(world, quad,
                                      (Vector3){ 0.0f, 4.0f, 0.0f }, WHITE);
MyeMeshInstance *m = ecs_get_mut(world, monitor, MyeMeshInstance);
m->texture = mye_canvas_texture(world, canvas);
ecs_modified(world, monitor, MyeMeshInstance);
```

`MyeMeshInstance.texture` replaces the model's diffuse texture for that one
instance. Note the asymmetry with sprites: a source rect can flip a sprite,
but nothing here can flip a mesh's UVs, so **a canvas on a mesh is mirrored**
unless the mesh was authored for it. That is a real limitation and the
showcase example leaves it visible rather than hiding it.

There is one thing the engine decides for you here. The camera filling the
canvas draws the whole scene — including the monitor you just textured with
that canvas. Sampling a texture while drawing into it is a *feedback loop*:
GL leaves the result undefined, and WebGL logs an error for every such draw
call. So while rendering into a canvas, anything textured with that same
canvas is **skipped**. A monitor is simply absent from its own feed, and
draws normally everywhere else. The alternative was not a different picture
but an undefined one.

**Ordering is the part that would otherwise bite.** Canvases are drawn in the
`MyeOnDrawCanvases` phase, which runs *before* `MyeOnDraw3D` and
`MyeOnDraw2D`. So the canvas a sprite displays was filled in earlier in the
same frame — not last frame. You do not arrange this; the phase does.

Two more fields, both about not doing work you did not ask for:

```c ctx
MyeCanvas *canvas_cfg = ecs_get_mut(world, canvas, MyeCanvas);
canvas_cfg->active = false; /* stop rendering it; the texture keeps its
                             * last contents, which is exactly what a paused
                             * security feed should look like */
canvas_cfg->clear = false;  /* do not wipe between frames: draw over what is
                             * already there, for trails and paint effects */
ecs_modified(world, canvas, MyeCanvas);
```

A canvas is a normal entity, so deleting the scene deletes it. To remove one
by hand, `mye_canvas_destroy(world, canvas)` — which releases the texture
handle and frees the GPU surface. Sprites still pointing at the old handle
draw the placeholder rather than reading freed memory.

---

## 9. The transform hierarchy

**What it is.** Parenting, using flecs' own `EcsChildOf` relationship. A child's
position is an offset from its parent; the engine composes the chain into a
`MyeWorldTransform` every frame, in `EcsPostUpdate`.

**Why it is.** A turret on a tank, a shield orbiting a player, a lamp attached
to a car: you want to move the parent and have the children follow. Doing this
by hand means every child's update needs to know its parent, and needs to run
after it. Propagation is breadth-first (`EcsCascade`), so a parent's world
matrix is final before any child reads it, however deep the chain.

Parenting is ECS data, not something the engine invents on the side — which
means deleting a parent deletes its children, for free.

```c
/* hierarchy.c -- a shield orbiting a player, in 2D. */
#include "core/engine.h"
#include "render/render2d.h"
#include "scene/transform.h"

#include <raylib.h>
#include <raymath.h>

typedef struct Spin { float angle; } Spin;
ECS_COMPONENT_DECLARE(Spin);

/* The parent's Spin arrives as a term: `up ChildOf` walks the hierarchy, so
 * there is no lookup by name and no wondering whether the parent ran first. */
static void Orbit(ecs_iter_t *it)
{
    MyePosition2D *local = ecs_field(it, MyePosition2D, 0);
    const Spin *parent = ecs_field(it, Spin, 1);      /* one value: the parent's */

    for (int i = 0; i < it->count; ++i) {
        local[i].x = cosf(parent->angle) * 60.0f;      /* offset, not world pos */
        local[i].y = sinf(parent->angle) * 60.0f;
    }
}

static void Rotate(ecs_iter_t *it)
{
    Spin *s = ecs_field(it, Spin, 0);
    for (int i = 0; i < it->count; ++i) s[i].angle += 2.0f * (float)it->delta_time;
}

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .title = "hierarchy" });
    if (world == NULL) return 1;

    ECS_COMPONENT_DEFINE(world, Spin);
    ECS_SYSTEM(world, Rotate, MyeOnFixedUpdate, Spin);
    ECS_SYSTEM(world, Orbit, MyeOnFixedUpdate, MyePosition2D, [in] Spin(up ChildOf));

    Image disc = GenImageColor(32, 32, BLANK);
    ImageDrawCircle(&disc, 16, 16, 15, SKYBLUE);
    mye_texture tex = mye_texture_from_image(world, "gen:disc", disc);

    ecs_entity_t parent = mye_entity_new(world);
    ecs_set(world, parent, MyePosition2D, { 400.0f, 300.0f });
    ecs_set(world, parent, Spin, { 0.0f });
    ecs_set(world, parent, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, parent, MyeWorldTransform, { MatrixIdentity() });
    ecs_set(world, parent, MyeSprite, { .texture = tex, .origin = { 16, 16 }, .tint = WHITE });

    ecs_entity_t child = mye_entity_new(world);
    ecs_set(world, child, MyePosition2D, { 60.0f, 0.0f });   /* relative to parent */
    ecs_set(world, child, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, child, MyeWorldTransform, { MatrixIdentity() });
    ecs_set(world, child, MyeSprite, { .texture = tex, .origin = { 16, 16 }, .tint = GOLD });
    mye_set_parent(world, child, parent);

    while (mye_running(world)) mye_progress(world, GetFrameTime());
    return mye_shutdown(world);
}
```

What a parent gives a child is its **placement**: the child's drawn position
comes from the composed chain, including the parent's rotation and scale. The
child sprite's own on-screen angle and size still come from its own
`MyeRotation2D` and `MyeScale2D` — rotating a tank turns where its turret sits,
but not which way the turret sprite points. Rotate the child too if you want
both.

**Both transform components are required.** An entity takes part in the
hierarchy only if it has `MyeLocalTransform` and `MyeWorldTransform`; without
them its `MyePosition2D` is read as a world position and parenting does nothing.
For 3D, `mye_spawn_3d(world, position)` adds the whole set for you.

`mye_world_position(world, entity)` reads the composed result, and
`mye_set_parent(world, child, 0)` unparents.

---

## 10. Prefabs

**What it is.** A prefab is an entity marked `EcsPrefab`: a template that is
never itself drawn or simulated. Instantiating one with `EcsIsA` gives the new
entity the prefab's components.

**Why it is.** "Spawn an enemy" should be one line, and changing what an enemy
*is* should be one edit. Without prefabs, the enemy's stats are spread across
whatever function happened to create it, and a second spawn site is a second
copy waiting to drift out of sync.

Prefabs are excluded from queries automatically, so the template never shows up
as a live game object — that exclusion is the whole point of the `EcsPrefab`
tag.

```c
/* prefabs.c -- one template, many instances. */
#include "core/engine.h"
#include "render/render2d.h"

#include <raylib.h>

typedef struct Health { int hp; } Health;
ECS_COMPONENT_DECLARE(Health);

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .title = "prefabs" });
    if (world == NULL) return 1;

    ECS_COMPONENT_DEFINE(world, Health);

    Image img = GenImageColor(24, 24, BLANK);
    ImageDrawCircle(&img, 12, 12, 11, RED);
    mye_texture tex = mye_texture_from_image(world, "gen:enemy", img);

    /* The template. Everything an enemy IS, in one place. */
    ecs_entity_t enemy = ecs_entity(world, { .name = "EnemyPrefab",
                                             .add = ecs_ids(EcsPrefab) });
    ecs_set(world, enemy, Health, { 30 });
    ecs_set(world, enemy, MyeSprite,
            { .texture = tex, .origin = { 12.0f, 12.0f }, .tint = WHITE, .layer = 5 });

    /* Instances: shared definition, per-instance placement. */
    for (int i = 0; i < 8; ++i) {
        ecs_entity_t e = ecs_new_w_pair(world, EcsIsA, enemy);
        ecs_set(world, e, MyePosition2D, { 100.0f + (float)i * 80.0f, 300.0f });
    }

    while (mye_running(world)) mye_progress(world, GetFrameTime());
    return mye_shutdown(world);
}
```

By default a component is **copied** to the instance, so each enemy has its own
`Health` and taking damage affects one of them. That default is per component,
set with the `OnInstantiate` trait: `EcsInherit` makes instances share the
prefab's value (good for constants), `EcsDontInherit` withholds it entirely.

---

## 11. Scenes

**What it is.** A named set of entities and the assets they need, loaded and
unloaded as a unit: a menu, a level, a game-over screen.

**Why it is.** "Start a new game" must not leave the previous game's bullets
alive. Entities created through `mye_entity_new` are tagged with the scene that
owns them, so unloading deletes exactly that scene's entities and releases
exactly its assets — without you maintaining a list.

Switching is **deferred to a frame boundary**. Tearing down the world halfway
through a system that is iterating it would be a crash; the switch is recorded
and applied by `mye_progress` between frames.

```c
/* scenes.c -- two scenes and a key to move between them. */
#include "core/engine.h"
#include "input/input.h"
#include "render/render2d.h"
#include "scene/scene.h"

#include <raylib.h>
#include <string.h>

enum { ACT_CONFIRM };

static void menu_load(ecs_world_t *world, void *user)
{
    (void)world; (void)user;   /* the menu is one draw system; nothing to spawn */
}

static void play_load(ecs_world_t *world, void *user)
{
    (void)user;
    Image img = GenImageColor(24, 24, BLANK);
    ImageDrawCircle(&img, 12, 12, 11, LIME);
    mye_texture tex = mye_texture_from_image(world, "gen:orb", img);

    for (int i = 0; i < 12; ++i) {
        /* mye_entity_new tags these as owned by "play". */
        ecs_entity_t e = mye_entity_new(world);
        ecs_set(world, e, MyePosition2D,
                { (float)GetRandomValue(40, 760), (float)GetRandomValue(40, 560) });
        ecs_set(world, e, MyeSprite,
                { .texture = tex, .origin = { 12.0f, 12.0f }, .tint = WHITE });
    }
}

/* Draw systems live for the whole program, so they check the active scene. */
static void Draw(ecs_iter_t *it)
{
    const char *scene = mye_scene_current(it->world);
    if (scene == NULL) return;

    if (strcmp(scene, "menu") == 0) DrawText("ENTER to play", 260, 280, 28, RAYWHITE);
    else                            DrawText("ENTER for menu", 260, 20, 20, GRAY);
}

static void Switch(ecs_iter_t *it)
{
    ecs_world_t *world = it->world;
    if (!mye_action_pressed(world, ACT_CONFIRM)) return;

    const char *scene = mye_scene_current(world);
    mye_scene_switch(world, strcmp(scene, "menu") == 0 ? "play" : "menu");
}

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .width = 800, .height = 600,
                                                 .title = "scenes" });
    if (world == NULL) return 1;

    mye_input_bind_key(world, ACT_CONFIRM, KEY_ENTER);

    ECS_SYSTEM(world, Switch, EcsOnUpdate, MyeRenderConfig);
    ECS_SYSTEM(world, Draw, MyeOnDrawUI, MyeRenderConfig);

    mye_scene_register(world, &(mye_scene_desc){ .name = "menu", .load = menu_load });
    mye_scene_register(world, &(mye_scene_desc){ .name = "play", .load = play_load });
    mye_scene_switch(world, "menu");

    while (mye_running(world)) mye_progress(world, GetFrameTime());
    return mye_shutdown(world);
}
```

`mye_scene_reload` re-runs the current scene's load function on a clean slate —
that is your "restart level". `mye_scene_desc` also takes an optional `unload`
callback for anything the automatic teardown cannot know about.

**Register systems in `main`, not in a load function.** Systems are entities
too; created inside a scene load they would be owned by that scene and deleted
with it.

---

## 12. Input actions

**What it is.** Bind keys, mouse buttons, gamepad buttons and gamepad axes to
integer *actions*. Gameplay code asks about the action.

**Why it is.** `IsKeyDown(KEY_W)` scattered through gameplay makes rebinding
impossible, gamepad support a rewrite, and testing require synthetic key
events. With actions, a test writes the action state directly and the same
gameplay code runs headless.

Several bindings can drive one action, and axis values from all of them sum and
clamp to [-1, 1] — so WASD and arrows and a stick all work at once, for free.

```c ctx
enum { ACT_MOVE_X, ACT_MOVE_Y, ACT_JUMP };

mye_input_bind_axis_keys(world, ACT_MOVE_X, KEY_A, KEY_D);       /* -1 .. +1 */
mye_input_bind_axis_keys(world, ACT_MOVE_X, KEY_LEFT, KEY_RIGHT);
mye_input_bind_gamepad_axis(world, ACT_MOVE_X, 0, GAMEPAD_AXIS_LEFT_X, 0.2f);
mye_input_bind_key(world, ACT_JUMP, KEY_SPACE);
mye_input_bind_gamepad_button(world, ACT_JUMP, 0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);

/* In a system: */
float dx = mye_action_value(world, ACT_MOVE_X);   /* analog */
if (mye_action_pressed(world, ACT_JUMP)) { }      /* this frame only */
if (mye_action_down(world, ACT_JUMP))    { }      /* held */
```

`pressed` is the rising edge and is true for exactly one frame. It is computed
by comparing this frame's state to the previous one, from `IsKeyDown` — so a
tap that begins *and* ends between two polls is missed entirely. At 60 FPS that
takes a deliberate effort, but it is a real limitation rather than something
the engine papers over.

---

---

## 13. Audio

**What it is.** `mye_sound_play(world, handle)` from anywhere. Requests are
queued and played once per frame.

**Why it is.** Two things fall out of queueing rather than playing immediately.
First, a fixed-timestep system can run several times in one frame (§4), and
without a queue a collision detected on each of three steps would fire three
overlapping copies of the same sound. Second, and more audibly: twenty bullets
hitting in one frame become **one** playback, not twenty stacked copies at
twenty times the volume — duplicates in a frame collapse, and the loudest wins.

The audio device belongs to the main thread, like the window. **Do not call
`mye_sound_play` from a job or a multi-threaded system** — the queue it writes
to is an unsynchronised singleton (§17).

```c
/* audio.c -- a synthesised beep, no asset files. */
#include "asset/asset.h"
#include "audio/audio.h"
#include "core/engine.h"
#include "core/rl_alloc.h"
#include "input/input.h"

#include <math.h>
#include <raylib.h>

enum { ACT_FIRE };

typedef struct Sfx { mye_sound beep; } Sfx;
ECS_COMPONENT_DECLARE(Sfx);

static Wave beep_wave(void)
{
    unsigned int frames = 22050 / 8;             /* 0.125 s at 22.05 kHz */
    short *samples = (short *)mye_rl_malloc(frames * sizeof(short));
    if (samples == NULL) return (Wave){ 0 };

    for (unsigned int i = 0; i < frames; ++i) {
        float t = (float)i / (float)frames;
        samples[i] = (short)(sinf(2.0f * PI * 700.0f * ((float)i / 22050.0f)) *
                             expf(-5.0f * t) * 11000.0f);
    }
    return (Wave){ .frameCount = frames, .sampleRate = 22050,
                   .sampleSize = 16, .channels = 1, .data = samples };
}

static void Fire(ecs_iter_t *it)
{
    const Sfx *sfx = ecs_field(it, Sfx, 0);
    if (mye_action_pressed(it->world, ACT_FIRE)) {
        mye_sound_play_ex(it->world, sfx->beep, 0.5f, 1.0f);  /* volume, pitch */
    }
}

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .title = "audio" });
    if (world == NULL) return 1;

    ECS_COMPONENT_DEFINE(world, Sfx);
    ecs_add_id(world, ecs_id(Sfx), EcsSingleton);
    ecs_singleton_set(world, Sfx,
                      { mye_sound_from_wave(world, "gen:beep", beep_wave()) });

    mye_input_bind_key(world, ACT_FIRE, KEY_SPACE);
    ECS_SYSTEM(world, Fire, EcsOnUpdate, Sfx);

    while (mye_running(world)) mye_progress(world, GetFrameTime());
    return mye_shutdown(world);
}
```

`mye_audio_set_master_volume` and `mye_audio_set_muted` do what they say. The
queue holds `MYE_MAX_QUEUED_SOUNDS` (32) requests per frame and **drops** past
that rather than growing — a bug that requests a thousand sounds should get
quieter, not run you out of memory.

Note `mye_rl_malloc` above: the wave's samples are handed to raylib, which will
free them with its own allocator, so they must be allocated with the matching
one.

---

## 14. 3D rendering

**What it is.** A mesh instance is an entity with `MyeMeshInstance` and the
transform components. A camera is an entity with `MyeCamera3D`, a light an
entity with `MyeLight`. The 3D pass runs in `MyeOnDraw3D`, before sprites, so a
2D HUD composes on top.

**Why it is.** Same reason as everything else here: a "scene graph" is just
entities with components, and 3D differs from 2D in which components are on the
entity rather than in which code path runs.

```c
/* scene3d.c -- a lit, rotating cube with a ground grid. */
#include "asset/asset.h"
#include "core/engine.h"
#include "render/camera.h"
#include "render/render3d.h"
#include "scene/transform.h"

#include <raylib.h>
#include <raymath.h>

static void Spin(ecs_iter_t *it)
{
    MyeRotation3D *rot = ecs_field(it, MyeRotation3D, 0);
    for (int i = 0; i < it->count; ++i) {
        rot[i].q = QuaternionMultiply(
            rot[i].q, QuaternionFromAxisAngle((Vector3){ 0, 1, 0 },
                                              1.0f * (float)it->delta_time));
    }
}

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .title = "3d" });
    if (world == NULL) return 1;

    ECS_SYSTEM(world, Spin, MyeOnFixedUpdate, MyeRotation3D);

    mye_model cube = mye_model_from_mesh(world, "gen:cube",
                                         GenMeshCube(2.0f, 2.0f, 2.0f), WHITE);
    ecs_entity_t e = mye_mesh_spawn(world, cube, (Vector3){ 0, 1, 0 }, SKYBLUE);
    ecs_set(world, e, MyeRotation3D, { QuaternionIdentity() });

    mye_camera3d_spawn(world, (Vector3){ 6, 5, 6 }, (Vector3){ 0, 1, 0 }, 45.0f);

    ecs_entity_t sun = mye_entity_new(world);
    ecs_set(world, sun, MyeLight, { .direction = { -0.5f, -1.0f, -0.3f },
                                    .color = WHITE, .intensity = 1.0f,
                                    .enabled = true });

    MyeRender3dConfig *cfg = ecs_singleton_ensure(world, MyeRender3dConfig);
    cfg->ambient = (Color){ 40, 45, 60, 255 };
    cfg->draw_grid = true;
    ecs_singleton_modified(world, MyeRender3dConfig);

    while (mye_running(world)) mye_progress(world, GetFrameTime());
    return mye_shutdown(world);
}
```

`mye_mesh_spawn` and `mye_camera3d_spawn` are conveniences; both just add
components you could add yourself.

A 3D camera is placed by its transform, exactly like the 2D one (§8), so
everything there applies: `MyeCameraFollow` to chase something,
`mye_set_parent` to weld it on, and camera logic in the `MyeOnCamera` phase.
Two things are 3D-specific:

```c ctx
/* Aim it. The target is a WORLD point, converted into the camera's own space
 * internally -- so this stays correct for a camera parented to a moving,
 * turning vehicle. */
mye_camera_look_at(world, camera, (Vector3){ 0.0f, 1.0f, 0.0f });

/* Field of view, in degrees. Narrowing it is a zoom; widening it during a
 * sprint is the classic speed effect. Clamped, because a zero fov renders
 * nothing and looks like a crash. */
mye_camera_set_fov(world, camera, 45.0f);
```

An orbit camera is a camera parented to an invisible pivot: rotate the pivot
and the camera swings around it, with no trigonometry anywhere.
`examples/03_scene3d` does exactly that.

### Two shaders

`MyeRender3dConfig.use_pbr` picks between them:

- **Blinn-Phong** (default): a colour, a light direction, a highlight. Cheap,
  and it ignores every texture map a model carries beyond the base colour.
- **PBR**: Cook-Torrance with a GGX distribution, Smith geometry and Schlick
  Fresnel, lit in linear space and tone-mapped on the way out. It reads the
  metallic, roughness, normal, occlusion and emissive maps that glTF files
  actually ship, so a downloaded model looks the way its author intended.

```c ctx
cfg->use_pbr = true;    /* worth it the moment you load a real glTF */
```

Up to `MYE_MAX_LIGHTS` (4) directional lights are passed to the shader.
`ambient` keeps unlit faces from going pure black.

---

## 15. Skeletal animation

**What it is.** A rigged model carries a skeleton and animation clips.
`MyeModelAnimator` advances a clip; raylib interpolates between keyframes and
skins the mesh.

**Why it is.** It is how a character walks. The engine's part is small — a
frame counter with looping and speed — because raylib does the skinning.

```c ctx
/* Resolved against the executable as well as the working directory, so the
 * program finds its assets wherever it was started from. */
char path[1024];
mye_asset_path("assets/models/Fox.glb", path, sizeof path);

mye_model fox = mye_model_load(world, path);
ecs_entity_t e = mye_mesh_spawn(world, fox, (Vector3){ 0, 0, 0 }, WHITE);

ecs_set(world, e, MyeModelAnimator,
        { .animation = 1,      /* clip index: Fox has Survey, Walk, Run */
          .frame = 0.0f,
          .speed = 24.0f,      /* frames per second; a common glTF default */
          .loop = true, .playing = true });
```

`frame` is fractional and raylib interpolates, so a non-integer speed is fine.

**A limitation worth knowing before it bites you:** raylib stores the animated
pose *in the `Model`*, not per instance. Two entities sharing one model handle
cannot hold different poses — the last one to update each frame wins. Give each
animated character its own model handle. Static instances of the same model are
unaffected and still share freely.

`tools/fetch_sample_assets.sh` downloads two rigged/PBR glTF models to try this
with. Check `THIRD-PARTY-NOTICES.md` before shipping either: BoomBox is CC0, but
Fox's rig, animation and glTF conversion are CC-BY-4.0 and **require
attribution**.

---

## 16. Saving and loading

**What it is.** flecs can reflect over component types it has been given a
description of, and serialize the world to JSON.

**Why it is.** Save games, and — more useful day to day — a test that runs a
game for 100 frames, saves, reloads into a fresh world, and asserts the two
match. That catches whole classes of "I forgot to reset that" bugs.

A component is serializable only if you describe its layout:

```c
/* save.c -- describe, save, reload. */
#include "core/engine.h"
#include "scene/serialize.h"

#include <stdio.h>

typedef struct Stats { int32_t hp; float speed; } Stats;
ECS_COMPONENT_DECLARE(Stats);

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){ .headless = true });
    if (world == NULL) return 1;

    ECS_COMPONENT_DEFINE(world, Stats);

    /* Without this, Stats saves as an empty object. */
    ecs_struct(world, {
        .entity = ecs_id(Stats),
        .members = {
            { .name = "hp",    .type = ecs_id(ecs_i32_t) },
            { .name = "speed", .type = ecs_id(ecs_f32_t) },
        },
    });

    ecs_entity_t hero = ecs_entity(world, { .name = "hero" });
    ecs_set(world, hero, Stats, { .hp = 70, .speed = 3.5f });

    mye_world_save_json(world, "save.json");

    ecs_set(world, hero, Stats, { .hp = 1, .speed = 0.0f });   /* damage it */
    mye_world_load_json(world, "save.json");                   /* and undo that */

    printf("hp = %d\n", ecs_get(world, hero, Stats)->hp);      /* 70 */
    return mye_shutdown(world);
}
```

`mye_serialize_register_engine_components(world)` describes the engine's own
components (positions, sprites, transforms) so they round-trip too.
`mye_scene_to_json` serializes only the active scene; `mye_world_to_json` takes
everything. Both return a string you release with `mye_json_free`.

`mye_component_serializable(world, ecs_id(Stats))` tells you whether you
remembered the `ecs_struct` — worth asserting in a test, because forgetting it
fails silently with empty objects rather than loudly.

---

## 17. Concurrency

**What it is.** Three separate things, in increasing order of how much rope
they give you:

1. **Parallel systems** — flecs shards a system's matched tables across worker
   threads.
2. **A job pool** — submit a function and an argument, workers run it.
3. **Channels** — a bounded queue for passing values between threads.

**Why it is.** At 50,000 entities the stress example measures **10.9×** on this
machine. Below roughly 1,000 entities it is *slower* — thread coordination
costs more than the work. Parallelism is a tool for a measured problem, not a
default.

The policy the engine follows, and that you should too:

> **Mutex by default. Atomics on hot spots. Sometimes a hot spot can be
> lock-free instead — where there is a single writer, or where reading a stale
> value is harmless.**

Lock-free is the most restricted tier, not the aspirational one. The tracking
allocator's counters are atomic because every allocation touches them; the
channel uses a plain mutex because it is not hot.

### Parallel systems

```c fn
ecs_world_t *world = mye_init(&(mye_config){ .worker_threads = 4 });

/* ECS_SYSTEM does not take flags, so use the descriptor form. */
ecs_system(world, {
    .entity = ecs_entity(world, { .name = "Move",
                                  .add = ecs_ids(ecs_dependson(MyeOnFixedUpdate)) }),
    .query.terms = {{ .id = ecs_id(MyePosition2D) },
                    { .id = ecs_id(Velocity), .inout = EcsIn }},
    .callback = Move,
    .multi_threaded = true,
});
```

Two rules a multi-threaded system must obey, or it corrupts state in ways no
test reliably catches:

- **Touch nothing but its own query fields and read-only singletons.** No
  `ecs_get` on an unrelated entity, no globals.
- **Never call `mye_frame_allocator`.** It is a bump pointer with no
  synchronisation; concurrent use corrupts it.

Drawing is never parallel. OpenGL calls belong to one thread, so every draw
phase is main-thread only, by construction.

### Jobs and channels

For work that is not a system — decoding, generation, file I/O:

```c
/* jobs.c -- fan work out, collect results in order. */
#include "core/alloc.h"
#include "core/channel.h"
#include "core/jobs.h"

#include <stdio.h>

typedef struct Result { int index, value; } Result;

typedef struct Task { int index; mye_channel *out; } Task;

static void work(void *arg)
{
    Task *task = (Task *)arg;
    /* ... expensive ... */
    mye_channel_send(task->out, &(Result){ task->index, task->index * task->index });
}

int main(void)
{
    mye_allocator heap = mye_heap_allocator();

    /* Bounded: send blocks when full, which is backpressure rather than an
     * unbounded queue quietly eating memory. */
    mye_channel *results = mye_channel_create(heap, 64, sizeof(Result));
    mye_jobs *pool = mye_jobs_create(heap, 4, 128);

    Task tasks[16];
    for (int i = 0; i < 16; ++i) {
        tasks[i] = (Task){ .index = i, .out = results };
        mye_jobs_submit(pool, work, &tasks[i]);
    }
    mye_jobs_wait_idle(pool);

    Result r;
    while (mye_channel_recv(results, &r)) printf("%d -> %d\n", r.index, r.value);

    mye_jobs_destroy(pool);
    mye_channel_destroy(results);
    return 0;
}
```

The channel copies elements by value, so there is no question of who frees what.

Everything here is built and tested under ThreadSanitizer as a separate
configuration:

```sh
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DMYE_SANITIZER=thread
cmake --build build/tsan -j && ctest --test-dir build/tsan -LE render
```

Render tests are excluded because the GL driver's own threads produce noise you
cannot fix. TSan found real races in this engine that ordinary testing did not
— it is worth the separate build.

---

## 18. Logging

**What it is.** One sink for the engine, raylib and flecs. `mye_log_install_hooks`
redirects the other two into it.

**Why it is.** Three libraries with three log formats going to two streams makes
a crash log unreadable. One sink also means you can *count* — a test can assert
that a run produced zero errors, which is a cheap and surprisingly effective
smoke test.

```c ctx
int orbs = 8, mines = 5;

mye_log_set_level(MYE_LOG_INFO);
mye_log_info("play: %d orbs, %d mines", orbs, mines);
mye_log_warn("no controller found; keyboard only");

mye_log_counts counts = mye_log_get_counts();
if (counts.error > 0) return;             /* fail the test */
```

Point it wherever you like — a file, an in-game console, a test buffer:

```c file
static void to_stderr(mye_log_level level, const char *source, const char *msg,
                      void *user)
{
    (void)user;
    fprintf(stderr, "[%d %s] %s\n", (int)level, source, msg);
}

static void install_logging(void)
{
    mye_log_set_sink(to_stderr, NULL);
}
```

The counters are atomic: raylib logs from its asset worker threads, so they are
written from more than one thread.

---

## 19. The debug overlay and the flecs Explorer

**What it is.** Two ways of seeing inside a running game. The overlay is an
in-window panel — FPS, frame-time graph, entity and system counts, allocator
high-water marks — toggled with **F3**. The Explorer is a browser UI that
connects to a REST server the engine starts in Debug builds.

**Why it is.** "It is slow" and "the enemy is not moving" are both questions
about state you cannot see. The overlay answers the first; the Explorer answers
the second by showing you every entity, its component *values*, and every
system and query — live, and editable while the game runs. For learning an ECS
this is worth more than any amount of `printf`.

```c fn
ecs_world_t *world = mye_init(&(mye_config){
    .explorer = true,          /* on by default in Debug; ignored in Release */
    .explorer_port = 27750,
});
```

Then open <https://www.flecs.dev/explorer/?host=localhost:27750>.

Release ignores the flag entirely — a shipped game should not run an HTTP
server. Override either way with `MYE_EXPLORER=0` or `MYE_EXPLORER=1`.

---

## 20. Testing what you wrote

**What it is.** Two layers. **Unit tests** cover one module with no window.
**Integration tests** drive a whole feature — often the real game code — in a
headless world.

**Why it is.** `headless = true` gives you the entire ECS, every non-rendering
system, and no window or GL context. That means gameplay is testable at full
speed in CI-less, screen-less environments, and `mye_shutdown` returns non-zero
on a leak so memory bugs fail the same run.

The pattern for a feature test: build a headless world, run N frames, assert.

```c test
/* tests/integration/test_int_movement.c */
#include "mye_test.h"

#include "core/engine.h"
#include "render/render2d.h"

void game_setup(ecs_world_t *world);   /* your game, built as a library */

TEST(entities_move_at_a_fixed_rate)
{
    ecs_world_t *world = mye_init(&(mye_config){ .headless = true });
    ASSERT_TRUE(world != NULL);

    game_setup(world);                       /* the real game's setup */

    for (int i = 0; i < 60; ++i) mye_progress(world, 1.0f / 60.0f);

    ecs_entity_t player = ecs_lookup(world, "player");
    const MyePosition2D *p = ecs_get(world, player, MyePosition2D);
    ASSERT_NEAR(100.0f, p->x, 0.01f);

    ASSERT_EQ_INT(0, mye_shutdown(world));   /* also asserts no leaks */
}

TEST_MAIN(TEST_CASE(entities_move_at_a_fixed_rate))
```

Because the timestep is fixed, `ASSERT_NEAR` on an exact expected value is
legitimate — the number does not depend on how fast the test machine is.

Structure game code so this is possible: put the setup and systems in a library
and keep `main` to three lines. `examples/02_asteroids` does exactly this
(`asteroids.c` is a static library; `main.c` is the loop), which is why
`test_int_asteroids` can drive the real game.

For anything that must actually draw, run it as a program with a frame limit and
a screenshot, and label the test `render` so the TSan build can skip it:

```sh
MYE_MAX_FRAMES=60 MYE_SCREENSHOT=out.png ./build/debug/examples/example_03_scene3d
```

`tools/check.sh` builds and tests Debug, Release and TSan and runs every
example headless. Run it before you commit.

---

## 21. The web target

**What it is.** The same source, compiled to WebAssembly with Emscripten, plus
a development server that rebuilds and reloads the browser when you save.

**Why it is.** Sharing a game as a URL is the difference between someone playing
it and someone not. The engine builds for the web with no `#ifdef` in gameplay
code: the loop, the ECS and the systems are identical.

```sh
python3 tools/web_dev.py --example 06_tutorial --port 8080
```

Edit any `.c` or `.h` and save: rebuild takes about 2 seconds and the page
reloads itself.

Nothing about this is tied to `examples/`. `--example` is shorthand for the
`example_<name>` targets; your own game takes `--target` instead, and needs
only one line in its `CMakeLists.txt`:

```cmake
add_executable(mygame main.c)
target_link_libraries(mygame PRIVATE mye_flags engine)

if(EMSCRIPTEN)
    mye_web_configure(mygame)   # shell page, ASYNCIFY, growable heap
endif()
```

`python3 tools/web_dev.py --target mygame` then finds the page wherever CMake
put it and watches every source in the repository, so a game three directories
deep hot-reloads exactly like an example.

Two decisions worth knowing about:

- **ASYNCIFY**, so `while (mye_running(world))` works unchanged. The browser
  requires yielding to its event loop each frame; ASYNCIFY makes the blocking
  loop yield transparently, at a small size and speed cost, in exchange for one
  main loop instead of two.
- **WebGL 2 only.** GLSL differs between desktop GL and WebGL, so the shaders
  select their version with a preprocessor conditional rather than the web
  build dictating a lowest common denominator:

```c file
#if defined(PLATFORM_WEB)
#define MYE_GLSL_VERSION "#version 300 es\nprecision highp float;\n"
#else
#define MYE_GLSL_VERSION "#version 330\n"
#endif
```

The desktop build keeps its shaders exactly as they were. There is no WebGL 1
fallback; it would mean writing and maintaining a second, worse renderer.

Threads are the one real difference: without cross-origin isolation there is no
`SharedArrayBuffer`, so the thread layer compiles to a `MYE_THREADS_NONE`
backend where jobs run inline on the calling thread. Everything still works,
single-threaded.

---

## 22. Networking

**What it is.** One WebSocket connection, opened by the game, pumped by the
engine, carrying byte blobs in both directions. A native build can also
*listen*, so the server for your game is another build of your game.

**Why it is.** A web build cannot open a socket; WebSocket is the only
game-shaped channel a browser offers. Choosing it for desktop too means one
piece of game code instead of two. Two consequences come with it and are not
negotiable: delivery is reliable and ordered, so a lost packet stalls
everything behind it (fine for co-op, chat and lobbies; wrong for twitch
action), and a browser can only ever be a client.

Opening a connection is one call, and handing it to the engine is one more:

```c ctx
mye_net_conn *conn = mye_net_connect(mye_allocator_of(world),
                                     "ws://localhost:9010/", NULL);
if (conn != NULL) {
    mye_net_register(world, conn);   /* pumped from now on */
}

/* ... and at shutdown, in this order. Unregister first, or the pump will
 * service freed memory next frame. */
mye_net_unregister(world, conn);
mye_net_destroy(conn);
```

`mye_net_register` is the whole design of the module. The engine services the
connections you gave it and no others: an unregistered connection is never
touched, which is what makes a tool, a test, or a second connection on its own
schedule possible to write. The pump runs inside `mye_progress`, in the same
slot as input polling — *before* the fixed steps — so this frame's simulation
sees this frame's messages.

What it publishes is a singleton, `MyeNetStatus`: status, peer count, queue
depths, bytes in and out. The debug overlay (§19) shows a `net` line whenever
something is registered, and your HUD can read the same struct.

**Nothing here interprets a message.** The engine moves bytes; what they mean
is gameplay. So a game drains the queue itself, in its own system:

```c file
typedef struct Chat { int lines; } Chat;
ECS_COMPONENT_DECLARE(Chat);

/* Registered in MyeOnFixedUpdate: the pump has already run this frame. */
void ChatReceive(ecs_iter_t *it)
{
    Chat *chat = ecs_field(it, Chat, 0);
    const MyeNetStatus *net = ecs_singleton_get(it->world, MyeNetStatus);
    if (net == NULL || net->count == 0) {
        return;
    }

    unsigned char buffer[256];
    size_t size;
    while ((size = mye_net_recv(net->conns[0], buffer, sizeof buffer,
                                NULL)) > 0) {
        /* The first byte says what the rest means. Route on it before
         * parsing anything: an unknown kind is skipped, never guessed at. */
        switch (buffer[0]) {
        case 1: /* a position */    break;
        case 2: chat->lines += 1;   break;
        default:                    break;
        }
    }
}
```

That one-byte kind prefix is a convention, not an API — the engine ships no
serialization opinion at all. [examples/07_net/presence.h](examples/07_net/presence.h)
is the smallest honest version of it, written to be copied: kind byte first,
integers written least significant byte first (the web build is a different
compiler), and a decoder that returns `false` for anything it does not
understand completely. Malformed input is the normal case on a socket anyone
can connect to.

**Remote entities want `MyeInterpolate` (§5).** Positions arrive at the
network rate — fifteen times a second in the example — while the screen
refreshes sixty or a hundred and forty-four times a second. Interpolation is
exactly the tool for that gap. Give a remote entity the component, apply
arriving positions in a fixed-step system, and it moves smoothly at any
refresh rate.

**Reconnecting is yours, and the engine does not do it behind your back.** A
game that lost its server may want a lobby screen, a save, or a clean exit.
What the engine hands you is the arithmetic — wait a little, then longer, then
give up:

```c ctx
static mye_net_backoff backoff;
mye_net_backoff_init(&backoff, &(mye_net_backoff_config){
    .first_delay = 0.5, .max_delay = 5.0, .factor = 2.0, .jitter = 0.2 });

mye_net_conn *conn = NULL; /* whatever you are holding */
double dt = 1.0 / 60.0;

if (mye_net_status_of(conn) == MYE_NET_OPEN) {
    mye_net_backoff_connected(&backoff);          /* back to the first rung */
} else {
    mye_net_backoff_failed(&backoff);             /* begin waiting */
    if (mye_net_backoff_ready(&backoff, dt)) {    /* true once, when due */
        mye_net_unregister(world, conn);
        mye_net_destroy(conn);
        /* ... redial and register the new one ... */
    }
}
```

It is a pure state machine: no clock, no socket, `dt` from the caller. That is
what makes it testable without a network, and
[tests/unit/test_net_backoff.c](tests/unit/test_net_backoff.c) tests it that
way.

**Try it.** The presence example is a relay and its clients in one binary:

```sh
cmake --build build/debug -j --target example_07_net
./build/debug/examples/example_07_net --serve &   # the relay, headless
./build/debug/examples/example_07_net &           # one client
./build/debug/examples/example_07_net             # and another
```

Arrows move your dot; the other client's dot moves on your screen. Type and
press Enter to chat. The round-trip time is on screen from the first frame,
deliberately: with TCP underneath, the cost of a stall should be visible while
you build rather than discovered in a bug report.

**What it does not do yet.** No `wss://` on native (no TLS — `ws://` only, and
a `wss://` URL is refused rather than silently downgraded); no UDP-like
transport; no state synchronisation, interest management or matchmaking. See
[plan/12-networking.md](plan/12-networking.md).

---

## 23. Capstone: Orbit Collector

Every feature above, in one game.

**Play:** WASD or arrows to move. Collect the green orbs, avoid the red mines.
ENTER starts and restarts, F3 shows the overlay.

```sh
cmake --build build/debug -j --target example_06_tutorial
./build/debug/examples/example_06_tutorial
```

What it uses, and where to look in the listing:

| Feature | In this program |
|---|---|
| World and loop (§1) | `main` — three calls, unchanged from the hello example |
| Components and systems (§2) | `Velocity`, `Collider`, `Player`, `Orb`, `Mine`, `Shield` |
| Singletons (§2) | `Game` — score, lives, handles, prefab IDs |
| Phases (§3) | simulation in `MyeOnFixedUpdate`, HUD in `MyeOnDrawUI` |
| Fixed timestep (§4) | `PlayerControl`, `MineDrift`, `Collisions`, `Clock` |
| Interpolation (§5) | `MyeInterpolate` on the player and mines; `mye_transform_snap` on wrap |
| Frame allocator (§6) | `DrawHud` builds its string there |
| Assets (§7) | four generated textures, two generated sounds |
| Sprite animation (§8) | the orbs' 4-frame pulse |
| Hierarchy (§9) | the shield is a child of the player |
| Prefabs (§10) | `OrbPrefab`, `MinePrefab` |
| Scenes (§11) | `menu` and `play`, with reload as "restart" |
| Audio (§13) | pickup and hit beeps, synthesised |
| Input actions (§14's sibling) | `ACT_X`, `ACT_Y`, `ACT_CONFIRM`, keys and arrows both |
| Logging (§18) | scene transitions and game over |
| Overlay (§19) | F3 |

```c capstone
/* Orbit Collector -- the tutorial's capstone.
 *
 * Every engine feature TUTORIAL.md introduces, in one working game:
 * scenes, input actions, fixed timestep, prefabs, sprite animation, a
 * transform hierarchy, interpolation, audio, the frame allocator, logging.
 *
 * Play: WASD/arrows move. Collect orbs. Avoid mines. ENTER starts and
 * restarts, F3 shows the debug overlay.
 */
#include "asset/asset.h"
#include "audio/audio.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/rl_alloc.h"
#include "input/input.h"
#include "render/camera.h"
#include "render/render2d.h"
#include "scene/scene.h"
#include "scene/transform.h"

#include <raylib.h>
#include <raymath.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 1000
#define SCREEN_H 640

#define PLAYER_ACCEL 900.0f
#define PLAYER_DRAG 3.0f
#define PLAYER_MAX_SPEED 340.0f
#define PLAYER_RADIUS 14.0f

#define ORB_COUNT 8
#define MINE_COUNT 5
#define ORB_RADIUS 10.0f
#define MINE_RADIUS 12.0f
#define SHIELD_DISTANCE 34.0f

enum { ACT_X, ACT_Y, ACT_CONFIRM };

/* --- components ---------------------------------------------------------- */

typedef struct Velocity { float x, y; } Velocity;
typedef struct Collider { float radius; } Collider;
typedef struct Player { float shield_angle; } Player;
typedef struct Orb { char unused; } Orb;
typedef struct Mine { float drift; } Mine;
typedef struct Shield { char unused; } Shield;

/* One singleton for everything the game needs globally. */
typedef struct Game {
    int score;
    int lives;
    float elapsed;
    bool over;

    mye_texture tex_ship, tex_orb, tex_mine, tex_shield;
    mye_sound sfx_pickup, sfx_hit;
    ecs_entity_t prefab_orb, prefab_mine;
} Game;

ECS_COMPONENT_DECLARE(Velocity);
ECS_COMPONENT_DECLARE(Collider);
ECS_COMPONENT_DECLARE(Player);
ECS_COMPONENT_DECLARE(Orb);
ECS_COMPONENT_DECLARE(Mine);
ECS_COMPONENT_DECLARE(Shield);
ECS_COMPONENT_DECLARE(Game);

/* --- generated art and audio --------------------------------------------- */

static Image disc_image(int radius, Color fill, Color rim)
{
    Image img = GenImageColor(radius * 2, radius * 2, BLANK);
    ImageDrawCircle(&img, radius, radius, radius - 1, fill);
    ImageDrawCircleLines(&img, radius, radius, radius - 1, rim);
    return img;
}

/* 4-frame pulse, laid out left to right: the flipbook the orbs play. */
static Image orb_atlas_image(void)
{
    const int size = 24;
    Image atlas = GenImageColor(size * 4, size, BLANK);
    for (int f = 0; f < 4; ++f) {
        float t = (float)f / 3.0f;
        int r = (int)(6.0f + t * 5.0f);
        unsigned char a = (unsigned char)(255 - (int)(t * 90.0f));
        ImageDrawCircle(&atlas, f * size + size / 2, size / 2, r,
                        (Color){ 120, 230, 190, a });
    }
    return atlas;
}

static Wave beep_wave(float seconds, float from_hz, float to_hz, float decay)
{
    unsigned int frames = (unsigned int)(seconds * 22050.0f);
    short *samples = (short *)mye_rl_malloc(frames * sizeof(short));
    if (samples == NULL) return (Wave){ 0 };

    for (unsigned int i = 0; i < frames; ++i) {
        float t = (float)i / (float)frames;
        float hz = from_hz + (to_hz - from_hz) * t;
        float envelope = expf(-decay * t);
        samples[i] = (short)(sinf(2.0f * PI * hz * ((float)i / 22050.0f)) *
                             envelope * 11000.0f);
    }
    return (Wave){ .frameCount = frames, .sampleRate = 22050,
                   .sampleSize = 16, .channels = 1, .data = samples };
}

/* --- simulation (fixed timestep) ----------------------------------------- */

static void PlayerControl(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    Velocity *vel = ecs_field(it, Velocity, 1);
    Player *player = ecs_field(it, Player, 2);
    ecs_world_t *world = it->world;
    float dt = (float)it->delta_time;

    float ax = mye_action_value(world, ACT_X);
    float ay = mye_action_value(world, ACT_Y);

    for (int i = 0; i < it->count; ++i) {
        vel[i].x += ax * PLAYER_ACCEL * dt;
        vel[i].y += ay * PLAYER_ACCEL * dt;

        vel[i].x -= vel[i].x * PLAYER_DRAG * dt;
        vel[i].y -= vel[i].y * PLAYER_DRAG * dt;

        float speed = sqrtf(vel[i].x * vel[i].x + vel[i].y * vel[i].y);
        if (speed > PLAYER_MAX_SPEED) {
            vel[i].x = vel[i].x / speed * PLAYER_MAX_SPEED;
            vel[i].y = vel[i].y / speed * PLAYER_MAX_SPEED;
        }

        pos[i].x += vel[i].x * dt;
        pos[i].y += vel[i].y * dt;

        /* Bounce off the edges rather than teleporting: no snap needed. */
        if (pos[i].x < PLAYER_RADIUS) { pos[i].x = PLAYER_RADIUS; vel[i].x = -vel[i].x * 0.5f; }
        if (pos[i].x > SCREEN_W - PLAYER_RADIUS) { pos[i].x = SCREEN_W - PLAYER_RADIUS; vel[i].x = -vel[i].x * 0.5f; }
        if (pos[i].y < PLAYER_RADIUS) { pos[i].y = PLAYER_RADIUS; vel[i].y = -vel[i].y * 0.5f; }
        if (pos[i].y > SCREEN_H - PLAYER_RADIUS) { pos[i].y = SCREEN_H - PLAYER_RADIUS; vel[i].y = -vel[i].y * 0.5f; }

        player[i].shield_angle += 2.4f * dt;
    }
}

/* The shield is parented to the player, so its position is a local offset and
 * the parent's Player component arrives as a term -- no lookup by name. */
static void ShieldOrbit(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const Player *parent = ecs_field(it, Player, 2); /* one value, the parent's */

    for (int i = 0; i < it->count; ++i) {
        pos[i].x = cosf(parent->shield_angle) * SHIELD_DISTANCE;
        pos[i].y = sinf(parent->shield_angle) * SHIELD_DISTANCE;
    }
}

static void MineDrift(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const Velocity *vel = ecs_field(it, Velocity, 1);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        pos[i].x += vel[i].x * dt;
        pos[i].y += vel[i].y * dt;

        bool wrapped = false;
        if (pos[i].x < 0 || pos[i].x > SCREEN_W) {
            pos[i].x = pos[i].x < 0 ? SCREEN_W : 0;
            wrapped = true;
        }
        if (pos[i].y < 0 || pos[i].y > SCREEN_H) {
            pos[i].y = pos[i].y < 0 ? SCREEN_H : 0;
            wrapped = true;
        }
        /* Only on the teleport. Snapping every step would suppress every
         * blend and turn interpolation off for these entities entirely. */
        if (wrapped) {
            mye_transform_snap(it->world, it->entities[i]);
        }
    }
}

static bool overlaps(MyePosition2D a, float ra, MyePosition2D b, float rb)
{
    return CheckCollisionCircles((Vector2){ a.x, a.y }, ra,
                                 (Vector2){ b.x, b.y }, rb);
}

/* Built once in main: a query is a compiled plan, not something to rebuild
 * sixty times a second. */
static ecs_query_t *g_orbs;
static ecs_query_t *g_mines;

static void Collisions(ecs_iter_t *it)
{
    const MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    ecs_world_t *world = it->world;

    Game *game = ecs_singleton_ensure(world, Game);
    if (game == NULL || game->over) return;

    for (int p = 0; p < it->count; ++p) {
        ecs_iter_t orb_it = ecs_query_iter(world, g_orbs);
        while (ecs_query_next(&orb_it)) {
            const MyePosition2D *op = ecs_field(&orb_it, MyePosition2D, 0);
            const Collider *oc = ecs_field(&orb_it, Collider, 1);
            for (int o = 0; o < orb_it.count; ++o) {
                if (!overlaps(pos[p], PLAYER_RADIUS, op[o], oc[o].radius)) continue;
                game->score += 10;
                mye_sound_play_ex(world, game->sfx_pickup, 0.4f, 1.0f);
                ecs_delete(world, orb_it.entities[o]);
            }
        }

        ecs_iter_t mine_it = ecs_query_iter(world, g_mines);
        while (ecs_query_next(&mine_it)) {
            const MyePosition2D *mp = ecs_field(&mine_it, MyePosition2D, 0);
            const Collider *mc = ecs_field(&mine_it, Collider, 1);
            for (int m = 0; m < mine_it.count; ++m) {
                if (!overlaps(pos[p], PLAYER_RADIUS, mp[m], mc[m].radius)) continue;
                mye_sound_play_ex(world, game->sfx_hit, 0.6f, 1.0f);
                ecs_delete(world, mine_it.entities[m]);
                if (--game->lives <= 0) {
                    game->over = true;
                    mye_log_info("game over with %d points", game->score);
                }
            }
        }
    }
}

static void Clock(ecs_iter_t *it)
{
    Game *game = ecs_field(it, Game, 0);
    if (!game->over) game->elapsed += (float)it->delta_time;
}

/* --- presentation -------------------------------------------------------- */

/* Draw systems run every frame, so each checks whether its scene is the
 * active one. */
static bool scene_is(const ecs_world_t *world, const char *name)
{
    const char *current = mye_scene_current(world);
    return current != NULL && strcmp(current, name) == 0;
}

static void DrawHud(ecs_iter_t *it)
{
    const Game *game = ecs_field(it, Game, 0);
    ecs_world_t *world = it->world;
    if (!scene_is(world, "play")) return;

    /* Frame allocator: reclaimed next frame, so nothing to free. */
    char *line = MYE_NEW_ARRAY(mye_frame_allocator(world), char, 128);
    if (line != NULL) {
        snprintf(line, 128, "SCORE %d    LIVES %d    TIME %.1f", game->score,
                 game->lives, (double)game->elapsed);
        DrawText(line, 20, 18, 22, RAYWHITE);
    }

    if (game->over) {
        const char *over = "GAME OVER";
        const char *hint = "ENTER to play again";
        DrawText(over, SCREEN_W / 2 - MeasureText(over, 52) / 2, 250, 52, RED);
        DrawText(hint, SCREEN_W / 2 - MeasureText(hint, 20) / 2, 320, 20,
                 RAYWHITE);
    }
}

static void DrawMenu(ecs_iter_t *it)
{
    if (!scene_is(it->world, "menu")) return;
    const char *title = "ORBIT COLLECTOR";
    const char *hint = "ENTER to start    WASD to move    F3 for stats";
    DrawText(title, SCREEN_W / 2 - MeasureText(title, 48) / 2, 210, 48,
             (Color){ 120, 230, 190, 255 });
    DrawText(hint, SCREEN_W / 2 - MeasureText(hint, 20) / 2, 300, 20,
             (Color){ 170, 178, 195, 255 });
}

/* --- scenes -------------------------------------------------------------- */

static void SceneSwitcher(ecs_iter_t *it)
{
    ecs_world_t *world = it->world;
    if (!mye_action_pressed(world, ACT_CONFIRM)) return;

    const char *current = mye_scene_current(world);
    const Game *game = ecs_singleton_get(world, Game);

    if (current == NULL) return;
    if (strcmp(current, "menu") == 0) {
        mye_scene_switch(world, "play");
    } else if (game != NULL && game->over) {
        mye_scene_reload(world); /* fresh play scene */
    }
}

static void menu_load(ecs_world_t *world, void *user)
{
    (void)world;
    (void)user;
    mye_log_info("menu");
    /* Nothing to spawn: the menu is one draw system. Registered once in
     * main, not here, so it survives scene switches. */
}

static void play_load(ecs_world_t *world, void *user)
{
    (void)user;
    Game *game = ecs_singleton_ensure(world, Game);
    game->score = 0;
    game->lives = 3;
    game->elapsed = 0.0f;
    game->over = false;

    /* Player: interpolated, and parent of the shield. */
    ecs_entity_t player = mye_entity_new(world);
    ecs_set_name(world, player, "player");
    ecs_set(world, player, MyePosition2D, { SCREEN_W / 2.0f, SCREEN_H / 2.0f });
    ecs_set(world, player, Velocity, { 0.0f, 0.0f });
    ecs_set(world, player, Player, { 0.0f });
    ecs_set(world, player, Collider, { PLAYER_RADIUS });
    ecs_set(world, player, MyeInterpolate, { 0 });
    ecs_set(world, player, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, player, MyeWorldTransform, { MatrixIdentity() });
    ecs_set(world, player, MyeSprite,
            { .texture = game->tex_ship, .origin = { 16.0f, 16.0f },
              .tint = WHITE, .layer = 10 });

    /* Child: its position is relative to the player, and interpolated in its
     * own right -- the blend composes down the chain, so it stays attached. */
    ecs_entity_t shield = mye_entity_new(world);
    ecs_set(world, shield, MyePosition2D, { SHIELD_DISTANCE, 0.0f });
    ecs_set(world, shield, Shield, { 0 });
    ecs_set(world, shield, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, shield, MyeWorldTransform, { MatrixIdentity() });
    ecs_set(world, shield, MyeInterpolate, { 0 });
    ecs_set(world, shield, MyeSprite,
            { .texture = game->tex_shield, .origin = { 8.0f, 8.0f },
              .tint = WHITE, .layer = 9 });
    mye_set_parent(world, shield, player);

    /* A camera that follows the player rather than being parented to it, so
     * it can lag slightly -- rigid attachment reads as the world sliding
     * about. It tracks the player's DRAWN position, so the blend from
     * MyeInterpolate is what the camera sees. Zoomed in, so the view
     * genuinely scrolls. */
    ecs_entity_t camera = mye_camera2d_spawn(
        world, (Vector2){ SCREEN_W / 2.0f, SCREEN_H / 2.0f }, 1.4f);
    ecs_set(world, camera, MyeCameraFollow,
            { .target = player, .stiffness = 6.0f });

    for (int i = 0; i < ORB_COUNT; ++i) {
        ecs_entity_t orb = ecs_new_w_pair(world, EcsIsA, game->prefab_orb);
        ecs_set(world, orb, MyePosition2D,
                { (float)GetRandomValue(60, SCREEN_W - 60),
                  (float)GetRandomValue(60, SCREEN_H - 60) });
        ecs_set(world, orb, MyeSpriteAnim,
                { .first_frame = { 0.0f, 0.0f, 24.0f, 24.0f },
                  .columns = 4, .frame_count = 4, .fps = 8.0f,
                  .loop = true, .playing = true });
    }

    for (int i = 0; i < MINE_COUNT; ++i) {
        ecs_entity_t mine = ecs_new_w_pair(world, EcsIsA, game->prefab_mine);
        ecs_set(world, mine, MyePosition2D,
                { (float)GetRandomValue(0, SCREEN_W),
                  (float)GetRandomValue(0, SCREEN_H) });
        ecs_set(world, mine, Velocity,
                { (float)GetRandomValue(-70, 70),
                  (float)GetRandomValue(-70, 70) });
        ecs_set(world, mine, MyeInterpolate, { 0 });
    }

    mye_log_info("play: %d orbs, %d mines", ORB_COUNT, MINE_COUNT);
}

/* --- setup --------------------------------------------------------------- */

static void build_prefabs(ecs_world_t *world, Game *game)
{
    game->prefab_orb = ecs_entity(world, { .name = "OrbPrefab",
                                           .add = ecs_ids(EcsPrefab) });
    ecs_set(world, game->prefab_orb, Orb, { 0 });
    ecs_set(world, game->prefab_orb, Collider, { ORB_RADIUS });
    ecs_set(world, game->prefab_orb, MyeSprite,
            { .texture = game->tex_orb,
              .source = { 0.0f, 0.0f, 24.0f, 24.0f },
              .origin = { 12.0f, 12.0f }, .tint = WHITE, .layer = 5 });

    game->prefab_mine = ecs_entity(world, { .name = "MinePrefab",
                                            .add = ecs_ids(EcsPrefab) });
    ecs_set(world, game->prefab_mine, Mine, { 0.0f });
    ecs_set(world, game->prefab_mine, Collider, { MINE_RADIUS });
    ecs_set(world, game->prefab_mine, MyeSprite,
            { .texture = game->tex_mine, .origin = { 12.0f, 12.0f },
              .tint = WHITE, .layer = 6 });
}

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .width = SCREEN_W, .height = SCREEN_H,
        .title = "myecs -- orbit collector",
        .frame_arena_bytes = 128 * 1024,
    });
    if (world == NULL) return 1;

    ECS_COMPONENT_DEFINE(world, Velocity);
    ECS_COMPONENT_DEFINE(world, Collider);
    ECS_COMPONENT_DEFINE(world, Player);
    ECS_COMPONENT_DEFINE(world, Orb);
    ECS_COMPONENT_DEFINE(world, Mine);
    ECS_COMPONENT_DEFINE(world, Shield);
    ECS_COMPONENT_DEFINE(world, Game);
    ecs_add_id(world, ecs_id(Game), EcsSingleton);

    mye_input_bind_axis_keys(world, ACT_X, KEY_A, KEY_D);
    mye_input_bind_axis_keys(world, ACT_X, KEY_LEFT, KEY_RIGHT);
    mye_input_bind_axis_keys(world, ACT_Y, KEY_W, KEY_S);
    mye_input_bind_axis_keys(world, ACT_Y, KEY_UP, KEY_DOWN);
    mye_input_bind_key(world, ACT_CONFIRM, KEY_ENTER);

    ecs_singleton_set(world, Game, { .lives = 3 });
    Game *game = ecs_singleton_ensure(world, Game);

    game->tex_ship = mye_texture_from_image(world, "gen:ship",
        disc_image(16, (Color){ 90, 160, 240, 255 }, RAYWHITE));
    game->tex_orb = mye_texture_from_image(world, "gen:orb", orb_atlas_image());
    game->tex_mine = mye_texture_from_image(world, "gen:mine",
        disc_image(12, (Color){ 210, 90, 80, 255 }, (Color){ 255, 200, 190, 255 }));
    game->tex_shield = mye_texture_from_image(world, "gen:shield",
        disc_image(8, (Color){ 240, 220, 120, 255 }, RAYWHITE));

    game->sfx_pickup = mye_sound_from_wave(world, "gen:pickup",
                                           beep_wave(0.12f, 600.0f, 1200.0f, 5.0f));
    game->sfx_hit = mye_sound_from_wave(world, "gen:hit",
                                        beep_wave(0.30f, 300.0f, 70.0f, 4.0f));

    build_prefabs(world, game);

    g_orbs = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyePosition2D), .inout = EcsIn },
                  { .id = ecs_id(Collider), .inout = EcsIn },
                  { .id = ecs_id(Orb) }},
    });
    g_mines = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyePosition2D), .inout = EcsIn },
                  { .id = ecs_id(Collider), .inout = EcsIn },
                  { .id = ecs_id(Mine) }},
    });

    /* Simulation: fixed timestep, so behaviour is framerate independent. */
    ECS_SYSTEM(world, PlayerControl, MyeOnFixedUpdate, MyePosition2D, Velocity, Player);
    ECS_SYSTEM(world, ShieldOrbit, MyeOnFixedUpdate, MyePosition2D, Shield,
               [in] Player(up ChildOf));
    ECS_SYSTEM(world, MineDrift, MyeOnFixedUpdate, MyePosition2D, [in] Velocity, [in] Mine);
    ECS_SYSTEM(world, Collisions, MyeOnFixedUpdate, [in] MyePosition2D, [in] Player);
    ECS_SYSTEM(world, Clock, MyeOnFixedUpdate, Game);

    /* Presentation and flow: variable rate. */
    ECS_SYSTEM(world, SceneSwitcher, EcsOnUpdate, Game);
    ECS_SYSTEM(world, DrawHud, MyeOnDrawUI, [in] Game);
    ECS_SYSTEM(world, DrawMenu, MyeOnDrawUI, [none] Game);

    mye_scene_register(world, &(mye_scene_desc){ .name = "menu", .load = menu_load });
    mye_scene_register(world, &(mye_scene_desc){ .name = "play", .load = play_load });
    mye_scene_switch(world, getenv("MYE_START_SCENE") ? getenv("MYE_START_SCENE") : "menu");

    MyeRenderConfig *render = ecs_singleton_ensure(world, MyeRenderConfig);
    render->clear_color = (Color){ 16, 18, 26, 255 };
    ecs_singleton_modified(world, MyeRenderConfig);

    SetRandomSeed(20260816);
    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }
    ecs_query_fini(g_orbs);
    ecs_query_fini(g_mines);
    return mye_shutdown(world);
}
```

`MYE_START_SCENE` is the one line here that is not a game feature: it is a
testing seam, so a screenshot run can start directly in `play` instead of
needing someone to press ENTER. Every example in this repository has a hook
like that, because a feature nobody can verify without watching it is a feature
that quietly breaks.

---

## Where to go next

- `plan/` holds the design documents each of these features was built from —
  particularly `plan/04-memory.md` for allocators and `plan/05-concurrency.md`
  for the threading rules.
- `examples/02_asteroids` is a complete game split into a library plus a `main`,
  which is the shape to copy when you want your game to be testable.
- `examples/05_showcase` loads real glTF models with PBR and skeletal animation.
- `examples/04_stress` is where the 10.9× parallel figure comes from; run it
  with `worker_threads` set to different values and watch the crossover.
