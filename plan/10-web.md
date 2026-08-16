# 10 — WebAssembly (browser) support

> **Status: planned, not started.** Scheduled as **M8**, after M7. See
> [07-roadmap.md](07-roadmap.md).

## Verdict: feasible, and both foundations already support it

- **raylib** ships a first-class `Web` platform (`PLATFORM=Web` is one of its
  supported CMake values, and it auto-selects it under `emcmake`). It renders
  through WebGL via GLES2/GLES3 and its GLFW layer maps to emscripten's.
- **flecs** compiles for emscripten (it has `__EMSCRIPTEN__` handling in its
  platform layer) and is pure C99 with no OS dependencies beyond the
  overridable `ecs_os_api`.
- **Our engine** is portable C11. Nothing in it is inherently desktop-only.

Toolchain: **emscripten** (`emcc`, `emcmake`) — not currently installed on
this machine, so this milestone starts with installing the SDK.

## The four things that actually need work

### 1. The main loop must invert (the big one)

A browser cannot let a program block. This does not work on the web:

```c
while (mye_running(world)) { mye_progress(world, GetFrameTime()); }
```

Emscripten requires the frame to be a callback it drives:

```c
static void web_frame(void *arg) { mye_progress((ecs_world_t *)arg, GetFrameTime()); }
...
emscripten_set_main_loop_arg(web_frame, world, 0, 1);
```

**Design implication:** the engine should own this, not each game. Add
`mye_run(world, frame_fn)` which is a plain `while` loop on desktop and an
`emscripten_set_main_loop_arg` on web, so a game's `main()` is identical on
both. Worth doing *before* there are many examples to convert.

**Revised**: `-sASYNCIFY` keeps the `while` loop unmodified at the cost of
binary size and some runtime overhead. That is now the recommended *starting*
point -- it gets a browser build working with zero engine changes, and the
inversion can follow once its cost is measured. See
[11-web-dev-loop.md](11-web-dev-loop.md).

### 2. Threads are cross-origin-isolated or absent

Emscripten pthreads need `-pthread`, `SharedArrayBuffer`, and the server must
send COOP/COEP headers. That is a deployment constraint, not just a build flag.

We are already well placed: `mye_config.asset_workers = -1` disables the pool
and makes every load synchronous, and `mye_texture_load_async` falls back
transparently. **First web build should ship single-threaded**, with threads
as a later opt-in once the hosting story is settled.

flecs' worker pipeline (M7) is subject to the same constraint — the web build
should keep `ecs_set_threads` at 1.

### 3. File I/O is a virtual filesystem

`fopen` works, but only against emscripten's in-memory FS, so assets must be
either **preloaded** (`--preload-file assets@/assets`, baked into a `.data`
file) or fetched asynchronously over the network. Since Asteroids generates
all its art and audio in code, the first web build needs no asset packaging at
all — a genuine accident of good fortune.

Longer term the async loader's *shape* is right for the web: the "decode
elsewhere, upload on the main thread" split maps onto `fetch` + main-thread
GPU upload, just with a different backend behind `mye_texture_load_async`.

### 4. Smaller things

| Item | Note |
|---|---|
| GL version | **WebGL 2** (`OPENGL_VERSION="ES 3.0"`), decided. GLSL ES 300 is near-identical to the desktop 330 shader, so only the prologue differs. WebGL 1 is out of scope. |
| Audio | miniaudio supports Web Audio; playback usually requires a user gesture first |
| `aligned_alloc` | available in emscripten's libc; our allocator is fine |
| C11 atomics | fine; only meaningful with `-pthread` |
| Exit | `mye_shutdown` never runs in the callback model — leak reporting needs an explicit "quit" path or `emscripten_cancel_main_loop` |
| Window size | canvas-driven; `SetWindowSize` and fullscreen behave differently |

## Proposed milestone

**M8 — Web build**, after M7:

1. Install emscripten SDK; add a `cmake/MyeWeb.cmake` toolchain path and
   document `emcmake cmake -S . -B build/web`.
2. Introduce `mye_run(world, frame_fn)` and convert all examples to it
   (desktop behaviour unchanged, verified by the existing suites).
3. Build `example_02_asteroids` for the web, single-threaded, no preloaded
   assets. Serve it locally and confirm it plays.
4. Add a script that at minimum *compiles* the web target, so desktop work
   cannot silently break it. (No CI -- see [09-testing.md](09-testing.md).)
5. Document the COOP/COEP requirement if and when threads are enabled.

**Definition of done:** Asteroids runs in a browser at 60 fps with sound, from
a single `emcmake` build, and the desktop suites are untouched and green.

## What this constrains today

Little, but two habits are worth keeping from now on:

- **Do not add a second blocking loop** anywhere. The frame belongs to
  `mye_progress`, called by whatever drives it.
- **Keep the synchronous fallbacks working.** Every async path should behave
  correctly with zero workers, since that is the first web configuration.
