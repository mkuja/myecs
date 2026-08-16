# 09 — Testing Policy

Two layers, both mandatory:

- **Unit tests** — one module, no window, fast, exhaustive on edge cases.
- **Integration tests** — one *complete feature*, driven through the real
  engine loop, asserting the behavior a player/programmer would observe.

Everything is registered with CTest, so `ctest` is the single command that
tells you whether the engine works ([08-build.md](08-build.md)).

## Test harness

A minimal pure-C harness, `tests/mye_test.h` (~100 LOC, written in-house —
keeps the strict-C rule and has zero build friction; modeled on
greatest.h/µnit):

```c
#include "core/alloc.h"
#include "mye_test.h"

TEST(arena_alignment)
{
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 1024));
    void *p = mye_alloc(mye_arena_allocator(&arena), 3, 16);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(0, (uintptr_t)p % 16);
    mye_arena_deinit(&arena);
}

TEST_MAIN(TEST_CASE(arena_alignment), TEST_CASE(arena_exhaustion))
```

Provides: `TEST`, `TEST_MAIN`, `ASSERT_TRUE/FALSE`, `ASSERT_EQ_INT/U64/PTR`,
`ASSERT_NEAR` (float epsilon), `ASSERT_STR_EQ`, `SKIP()`. Failures print
`file:line`, expected vs actual, and the process exits non-zero (CTest reads
the exit code). No mocking framework: C tests use real structs, fake payload
types, and function-pointer seams where isolation is needed.

## Unit testing policy

**Rule: every non-rendering engine module has `tests/unit/test_<module>.c`.**

| Module | What must be covered |
|---|---|
| `core/alloc` | alignment, exhaustion→`NULL`, arena reset, pool alloc/free/reuse, tracking counts, leak report |
| `core/channel` | send/recv order, full/empty behavior, wraparound, capacity edge cases |
| `core/math` | transform compose/decompose, matrix helpers, lerp/slerp edge cases |
| `asset` | handle generation & staleness, path dedupe, refcount release, placeholder on invalid handle |
| `input` | action binding, press/hold/release edges, axis composition, rebinding |
| `scene` transform | parent→child propagation, deep chains, reparenting, orphan cleanup |
| render *logic* | draw-list sorting order, visibility filtering (pure functions, no GL) |
| ECS glue | module import registers expected components/systems; system logic over a bare world |

Rules of engagement:

1. **A module without unit tests is not done** — tests land in the same
   milestone as the module ([07-roadmap.md](07-roadmap.md) DoDs enforce this).
2. **Bug ⇒ regression test first.** Reproduce in a failing test, then fix.
3. **Unit tests never open a window** and never call raylib GPU functions.
   Logic that "needs" a window is usually logic that should be extracted into
   a pure function — do that instead.
4. ECS logic is unit-testable headlessly: `ecs_init()` → import module → spawn
   entities → `ecs_progress(world, dt)` N times → assert component values →
   `ecs_fini()`.
5. Tests run under ASan/UBSan in Debug; a sanitizer report fails the test.
6. Deterministic only: fixed dt, seeded RNG, no wall-clock, no sleeps.

## Integration testing policy

**Rule: every completed feature gets an integration test that exercises it
end-to-end through the engine, and each milestone's definition of done names
its integration test.**

Location `tests/integration/test_int_<feature>.c`. Two flavors:

### Headless integration tests (default, run everywhere)

Build a real world with real engine modules, step it, assert observable
outcomes. No window needed. Examples mapped to milestones:

| Milestone | Integration test |
|---|---|
| M1 | 1000 movers: step 100 frames, assert all in bounds and moved |
| M2 | gameplay loop: scripted input sequence → bullet spawns → rock hit → score increments → game-over state |
| M2 | **determinism**: seeded world, 600 fixed steps, run twice, byte-compare resulting component state |
| M3 | full game run under allocator tracking → zero leaks, bounded high-water mark |
| M4 | async asset pipeline: submit loads → drain channel → assert EMPTY→LOADING→LOADED, all handles valid, no TSan reports |
| M5 | transform hierarchy: multi-level rig, move root, assert child world matrices; reparent mid-run |
| M6 | scene lifecycle: load A → switch B → back to A; assert entity counts, asset refcounts, zero leaks |
| M6 | serialization round-trip: save world → load into fresh world → compare |
| M7 | workers on: same determinism/state assertions as single-threaded baseline |

### Render smoke tests (labeled `render`)

For features whose output is pixels. They open a real window with
`SetConfigFlags(FLAG_WINDOW_HIDDEN)` before `InitWindow`, render a handful of
frames, and assert the engine survives and state is consistent — plus optional
`TakeScreenshot()` output for manual/golden comparison.

```cmake
add_test(NAME int_render_sprites COMMAND test_int_render_sprites)
set_tests_properties(int_render_sprites PROPERTIES LABELS "render" TIMEOUT 30)
```

- `ctest` runs everything locally; `ctest -LE render` on a headless box.
- Examples double as smoke tests: `MYE_MAX_FRAMES=N ./example_01_bounce`
  stops after N frames and exits through `mye_shutdown`, so the run is
  leak-checked and ASan-checked like any test. A non-zero exit means leaks.
- Golden-image comparison is *manual review* at first (screenshots written to
  the build dir); automated pixel-diff only if flakiness proves tolerable —
  GPU/driver differences make strict pixel equality a bad gate.

## What is NOT tested

- raylib and flecs internals (upstream's job).
- Exact pixel output across drivers.
- Frame-rate performance as a pass/fail gate — performance is tracked as
  recorded numbers in the M7 profiling pass, not an assertion that flakes on
  a busy machine.

## Verification, in practice

There is **no CI**, by decision -- this is a local project with no host. The
three configurations are run by hand, and the whole suite is fast enough that
this is not a burden:

```sh
cmake --build build/debug   -j && ctest --test-dir build/debug
cmake --build build/release -j && ctest --test-dir build/release
cmake --build build/tsan    -j && ctest --test-dir build/tsan -LE render
```

Debug catches memory errors and leaks (ASan/UBSan), Release catches what only
the optimiser diagnoses -- strict-aliasing violations, for one, which `-O0`
does not analyse -- and TSan catches races. All three matter: each has caught
a bug the others missed.

Windowed examples are checked the same way, since a bounded run exits through
`mye_shutdown` and reports leaks:

```sh
MYE_MAX_FRAMES=150 ./build/debug/examples/example_02_asteroids; echo $?
```

If this is ever hosted, that shell sequence is the whole CI job.
