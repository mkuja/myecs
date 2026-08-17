# myecs

A 2D and 3D game engine in strict C11, built on
[raylib](https://github.com/raysan5/raylib) for the platform layer and
[flecs](https://github.com/SanderMertens/flecs) for the ECS.

No C++ anywhere: the CMake project declares `C` as its only language, so a
stray `.cpp` will not even configure.

## Build and run

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure

./build/debug/examples/example_02_asteroids
```

Dependencies are fetched and pinned by CMake (raylib 6.0, flecs v4.1.6);
nothing needs installing first.

| Configuration | Command |
|---|---|
| Debug (ASan + UBSan) | `cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug` |
| Release | `cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release` |
| ThreadSanitizer | `cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DMYE_SANITIZE_THREAD=ON` |

TSan cannot share a build with ASan, and its runs exclude the GPU test:
`ctest --test-dir build/tsan -LE render`.

```sh
tools/check.sh            # all three configurations plus the examples
tools/check.sh --quick    # debug only, for a fast inner loop
```

There is no CI. That script is the whole verification story, and each
configuration has caught bugs the others missed.

## Examples

| Example | Shows |
|---|---|
| `example_00_hello` | bare raylib window -- the toolchain check |
| `example_01_bounce` | 1000 entities moving through ECS systems |
| `example_02_asteroids` | a complete game: input, collision, audio, animation, prefabs |
| `example_03_scene3d` | 3D transform hierarchy, lighting, 2D HUD over a 3D scene |
| `example_04_stress` | benchmark for entity counts and worker threads |
| `example_06_tutorial` | every feature in one game -- the [tutorial's](TUTORIAL.md) capstone |

Every example honours two environment variables, which is how they are
verified automatically:

```sh
MYE_MAX_FRAMES=120 ./build/debug/examples/example_02_asteroids   # exit after N frames
MYE_SCREENSHOT=shot.png MYE_MAX_FRAMES=60 ./build/debug/examples/example_03_scene3d
```

A bounded run exits through `mye_shutdown`, which reports allocator leaks, so
`echo $?` after one is a real check rather than a formality.

Debug builds also serve the world to the **flecs Explorer** -- a live view of
every entity, component and system, editable while the game runs:

```
https://www.flecs.dev/explorer/?host=localhost:27750
```

and **F3** toggles an on-screen overlay with frame times, memory, asset counts
and warning totals (`MYE_OVERLAY=1` to start with it open).

## Web build

All examples compile to WebAssembly. One command builds, serves and reloads
the browser when a source file changes:

```sh
source ~/emsdk/emsdk_env.sh                 # once per shell
tools/web_dev.py --example 02_asteroids     # http://localhost:8080
tools/web_dev.py --target mygame            # any target, in or out of examples/
```

Editing any `.c` or `.h` rebuilds (~2 s) and reloads the page. The build is
single-threaded and needs no asset packaging, because the examples generate
their textures and audio in code.

```sh
emcmake cmake -S . -B build/web -DCMAKE_BUILD_TYPE=Release
cmake --build build/web -j
```

## The shape of a game

```c
ecs_world_t *world = mye_init(&(mye_config){ .width = 1280, .height = 720 });

ECS_COMPONENT_DEFINE(world, Velocity);
ECS_SYSTEM(world, Move, MyeOnFixedUpdate, MyePosition2D, Velocity);

while (mye_running(world)) {
    mye_progress(world, GetFrameTime());
}
return mye_shutdown(world);
```

`mye_progress` owns the frame: it applies scene switches, polls input, runs
fixed-timestep systems N times, then runs the main pipeline including
rendering. Use it rather than `ecs_progress`.

## Modules

| Module | What it provides |
|---|---|
| `engine/core/alloc` | allocator interface: heap, arena, pool, tracking |
| `engine/core/engine` | lifecycle, frame loop, fixed timestep, render phases |
| `engine/core/channel`, `jobs`, `thread` | mutex-backed queue, worker pool, threading shim |
| `engine/input` | actions rather than key codes; replayable |
| `engine/asset` | handle-based registry, scene-scoped loading |
| `engine/audio` | sound queue, deduplicated per frame |
| `engine/render/render2d` | sprites, animation, camera, layer/y sorting |
| `engine/render/render3d` | meshes, camera, directional lights |
| `engine/scene/transform` | parent-child hierarchy shared by 2D and 3D |
| `engine/scene/scene` | load, unload, switch; scene-owned entities and assets |
| `engine/scene/serialize` | world and scene state to and from JSON |

## Rules worth knowing before writing engine code

1. **Every interface that allocates takes a `mye_allocator`.** No hidden
   global; no direct `malloc`. All of flecs' and raylib's allocations are
   routed through it too, so `mye_shutdown()` returning 0 is a whole-process
   leak check.
2. **Create entities with `mye_entity_new()`**, not `ecs_new()`. Only the
   former is owned by the active scene and cleaned up when it unloads.
3. **All rendering is main-thread only.** raylib's GL context belongs to the
   thread that opened the window.
4. **Concurrency: mutex by default, atomics on hot spots**, lock-free only at
   a hot spot with a single writer or harmless stale reads.
5. **A system marked `multi_threaded` must touch only its own query fields**
   and must never use `mye_frame_allocator()` -- the frame arena is an
   unsynchronised bump pointer.

## Performance

`example_04_stress`, release build, 16-core machine, headless:

| Entities | 1 thread | 8 threads | 16 threads |
|---|---|---|---|
| 50,000 | 0.77 ms | 0.10 ms | 0.07 ms |
| 200,000 | 2.85 ms | 0.39 ms | 0.27 ms |

Worker threads are **off by default** and worth enabling above roughly 1,000
entities per system; below that the synchronisation costs more than it saves
(at 100 entities, 8 threads is 4x *slower*).

## Documentation

**[TUTORIAL.md](TUTORIAL.md) is the place to start.** It covers every feature
the engine has -- what each one is, why it exists, and a working program using
it -- ending with one game that uses all of them
([examples/06_tutorial](examples/06_tutorial)).

Design documents live in [plan/](plan/) — architecture, ECS usage, rendering,
memory, concurrency, assets, roadmap, build, testing, and a planned
WebAssembly target. [plan/07-roadmap.md](plan/07-roadmap.md) is the honest
record of what is done and what each milestone taught.

## Licence

Third-party licences are collected in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), regenerated by
`tools/gen_third_party_notices.sh`. All are permissive; none is copyleft.

**This project has no licence of its own yet.**
