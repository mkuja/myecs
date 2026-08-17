# 11 — Web development loop (build, serve, hot-reload)

> **Status: planned, not started.** Part of **M8**. The target itself is
> described in [10-web.md](10-web.md); this document is about the *workflow*:
> one command that builds to WebAssembly, serves it, and reloads the browser
> when a source file changes.

## Verdict: not complicated

The whole thing is roughly **half a day**, and most of it is configuration
rather than engineering:

| Piece | Effort | Why it is small |
|---|---|---|
| WASM build | ~30 lines of CMake | emscripten and raylib already support it |
| HTML page | ~80 lines | a canvas, a status line, and a reload hook |
| Dev server | ~80 lines of Python | Python is already required by emsdk |
| File watcher | ~40 lines of Python | mtime polling; no platform APIs |
| Main-loop inversion | ~20 lines of engine | see below -- the one real change |

Cross-platform comes free: `emcmake`, `cmake`, and Python all behave the same
on Linux, macOS and Windows, and nothing here shells out to `inotify`,
`fswatch` or platform-specific tooling.

The genuine work is not the tooling. It is the four gotchas below.

## The four gotchas

### 1. The main loop: start with ASYNCIFY, invert it later

A browser cannot let a program block, so `while (mye_running(world))` needs
help. There are two ways, and the cheap one should come first.

**Option A -- `-sASYNCIFY` (start here).** Emscripten rewrites the compiled
code so a blocking loop can yield to the browser. The existing loop works
unmodified: **zero engine changes, zero example changes**, running in a
browser the same day.

**Option B -- invert the loop.** `emscripten_set_main_loop_arg` drives the
frame through a callback. The engine owns the difference so a game's `main()`
stays identical on both platforms:

```c
/* engine/core/engine.h */
void mye_run(ecs_world_t *world, void (*frame)(ecs_world_t *, void *),
             void *user);
```

...which is a plain `while` loop on desktop and `emscripten_set_main_loop_arg`
on web -- **one conditional inside the engine rather than one per example**.
Note that this does not avoid restructuring: the callback is required either
way, so a per-example `#ifdef` would add divergent code paths on top of the
same work.

| | ASYNCIFY | Inverted loop |
|---|---|---|
| Code change | **none** | callback extraction, all examples |
| Binary size | +30-50% | none |
| Runtime cost | instrumentation on every call that may yield | none |
| Debugging | harder -- rewritten stacks | normal |
| Time to a browser | **minutes** | after the refactor |

**Decision: ASYNCIFY first.** Prove the target works and find out what
actually breaks before paying for a refactor. Invert the loop when the size
or the frame cost is measured to matter -- and measure it, the way M7
measured threading rather than assuming.

A caveat specific to hot reload: with ASYNCIFY the program is *inside* a
blocking loop when a rebuild lands, so tier-2 snapshotting has to happen at a
frame boundary. In practice that means the snapshot hook lives at the top of
the frame, checking a flag JS sets -- which is where it would sit anyway.

### 2. The lighting shader is desktop-only (a real bug, found while planning)

raylib's web build uses `GRAPHICS_API_OPENGL_ES2`, and
`engine/render/render3d.c` hard-codes `#version 330` -- desktop GLSL. It will
not compile in a browser.

**The desktop shader must not change.** The web build is not allowed to cost
anything visually on desktop, so the fix is a preprocessor conditional that
leaves the current `#version 330` source byte-for-byte identical.

The choice of WebGL version decides how much of a fork this really is:

| Target | GLSL version | Divergence from the current shader |
|---|---|---|
| Desktop | `#version 330` | none -- unchanged |
| Web, **WebGL 2** | `#version 300 es` | **prologue only** |
| ~~Web, WebGL 1~~ | ~~`#version 100`~~ | **out of scope** -- see below |

GLSL ES 300 and GLSL 330 are near-identical languages: both use `in`/`out`,
`texture()` and a user-declared output. So targeting **WebGL 2** means only
the version line and a precision qualifier differ, and the lighting maths
stays a single shared string:

```c
#if defined(PLATFORM_WEB)
#define MYE_GLSL_PROLOGUE "#version 300 es\nprecision highp float;\n"
#else
#define MYE_GLSL_PROLOGUE "#version 330\n"
#endif

static const char *fragment_shader_src =
    MYE_GLSL_PROLOGUE
    "in vec3 fragPosition;\n"          /* one body, both targets */
    ...;
```

raylib supports this: its `OPENGL_VERSION` option accepts `ES 3.0`, so the web
build is configured with `-DOPENGL_VERSION="ES 3.0"` rather than the default
`ES 2.0`.

Two notes on quality:

- Use **`highp`**, not `mediump`. Precision qualifiers are mandatory in ES and
  merely ignored on desktop; `mediump` can band on lighting gradients, while
  `highp` on any desktop-class GPU is what desktop GL does anyway.
- The maths itself is unchanged -- dot products, `pow`, `normalize` all exist
  identically in ES. There is no visual compromise, only a syntax one.

**Decided: WebGL 2 only, no WebGL 1 fallback.** That audience does not
matter for this project, and the decision removes the one genuinely ongoing
cost the web target would otherwise carry -- a second shader, in a second
dialect, to keep in step with every future change to the lighting.

Consequences of the decision:

- The shader is **one body with two prologues**. Nothing to keep in sync.
- The web build configures raylib with `-DOPENGL_VERSION="ES 3.0"`.
- Anything requiring WebGL 1 is out of scope; a browser without WebGL 2 shows
  the shell page's unsupported message rather than a broken canvas.

**Worth fixing regardless of the web**: 2D-only games never touch this shader,
so the breakage would first surface the day someone opens a 3D scene in a
browser -- long after the cause was introduced.

### 3. Threads are absent, and the fallback already exists

Emscripten pthreads need `-pthread` plus cross-origin isolation (COOP/COEP
headers). The first web build should be single-threaded.

`engine/core/thread.h` currently has two backends (pthreads, C11 threads). Add
a third:

```c
#elif defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define MYE_THREADS_NONE 1
```

where `mye_thread_create` returns `false`. Nothing else changes, because the
graceful path is already built: `mye_jobs_create` returns NULL and a caller
that wanted a pool runs its work inline. Asset loading is synchronous on
every target, so it has no thread to lose.

The dev server sends COOP/COEP headers anyway, so enabling threads later is a
build-flag change rather than a re-architecture.

### 4. Audio needs a user gesture

Browsers refuse to start audio until the user interacts with the page. The
shell HTML should show a "click to start" overlay and only then call `main()`,
or accept that the first sounds are silent.

## Architecture of the dev loop

```
  tools/web_dev.py
        |
        +-- watcher thread ---- polls mtimes of engine/, examples/, plan-free
        |                       source globs every 300 ms
        |                            |
        |                            v
        |                       cmake --build build/web
        |                            |
        |                            v
        |                       build_id += 1
        |
        +-- http server ------- serves build/web/ + web/shell.html
                                 GET /build-id  -> current build id
                                 sends COOP/COEP headers

  browser: polls /build-id every 500 ms; on change, location.reload()
```

**Why polling rather than a WebSocket:** no dependencies, ~10 lines on each
side, and the latency that matters (rebuild time) dwarfs the 500 ms poll.

**Why our own server rather than `emrun`:** `emrun` is fine for a one-shot run
and usefully pipes the app's stdout to the terminal, but it cannot serve the
build-id endpoint or the isolation headers. Keep an `emrun` target as well --
it is the better tool for debugging a single run.

## Hot reload, in two tiers

The word means different things; both are worth having, and the second is
unusually cheap here because the serializer already exists.

### Tier 1 — live reload (state is lost)

Rebuild, refresh, back to the start of the scene. This is what "hot reload"
means in most web tooling, and it is what the loop above delivers. For a game,
losing state each rebuild is tolerable when a rebuild is a few seconds and
scenes load fast.

### Tier 2 — state-preserving reload

Before reloading, snapshot the world; after reloading, restore it. **M6 built
exactly this** ([serialize.h](../engine/scene/serialize.h)):

```
1. page receives "build changed"
2. calls exported mye_web_snapshot()  -> mye_world_to_json()
3. stores the string in sessionStorage
4. location.reload()
5. new module boots, finds the snapshot, calls mye_world_from_json()
```

Both C functions are exported with `EMSCRIPTEN_KEEPALIVE` and called from JS.
The cost is a few dozen lines.

Two honest limits:

- **Only reflected components survive.** Anything without an `ecs_struct`
  description is silently absent -- the same trap M6 documented, but now with
  visible consequences. `mye_component_serializable()` exists to audit it.
- **Asset handles do not survive** a module reload; they index into a registry
  the new module rebuilds from scratch. Scenes must reload their assets, which
  they already do, so this works as long as snapshots restore *entity state*
  rather than handles. A scene that reloads its assets and then applies a
  snapshot of positions and gameplay values is the working pattern.

Tier 2 is a genuine payoff from decisions already made, not new machinery.

## Commands

```sh
# one-time
git clone https://github.com/emscripten-core/emsdk && ./emsdk install latest
./emsdk activate latest && source ./emsdk_env.sh

# build
emcmake cmake -S . -B build/web -DCMAKE_BUILD_TYPE=Release
cmake --build build/web -j

# develop: builds, serves, watches, reloads
python3 tools/web_dev.py --example 02_asteroids --port 8080
python3 tools/web_dev.py --target mygame                    # any CMake target

# one-shot run with stdout piped to the terminal
emrun --port 8080 build/web/examples/example_02_asteroids.html
```

## Build configuration sketch

```cmake
if(EMSCRIPTEN)
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
    target_link_options(example_02_asteroids PRIVATE
        --shell-file ${CMAKE_SOURCE_DIR}/web/shell.html
        -sUSE_GLFW=3
        -sASYNCIFY                # start here; drop it if the loop is inverted
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']
        -sEXPORTED_FUNCTIONS=['_main','_mye_web_snapshot','_mye_web_restore'])
endif()
```

Sanitizers, the TSan configuration and the render-labelled tests are all
desktop-only and simply do not apply to this build.

## Order of work

1. **Build target with ASYNCIFY**: `emcmake`, shell HTML, Asteroids running
   in a browser. No engine changes at all -- this is deliberately first, so
   everything after it is informed by a working build rather than a guess.
2. **GLES shader variants** in `render3d.c` (needed for the 3D example; the
   2D game does not touch that shader).
3. **`MYE_THREADS_NONE`** backend in `thread.h`.
4. **`tools/web_dev.py`**: serve, watch, rebuild, reload (tier 1).
5. **Tier 2 snapshot/restore** on top of the existing serializer.
6. **A build-check script** that compiles the web target, so desktop work
   does not silently break it. There is no CI; this is one more command in
   the by-hand verification sequence.
7. **`mye_run()` and loop inversion** -- last, and only if ASYNCIFY's size or
   speed cost is measured to matter.

**Definition of done:** `python3 tools/web_dev.py` builds Asteroids to WASM,
serves it, and editing a `.c` file rebuilds and reloads the browser within a
few seconds -- on Linux, macOS and Windows alike.

## What this constrains today

- Do not add a second blocking loop anywhere; the frame belongs to
  `mye_progress`, called by whatever drives it.
- Keep every async path working with zero workers.
- **Give new components `ecs_struct` reflection** if they hold state worth
  surviving a reload. Tier 2 makes that discipline visible: an unreflected
  component silently resets on every rebuild.
