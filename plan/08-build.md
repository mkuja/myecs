# 08 — Build System & Dependencies

## Toolchain

- **CMake ≥ 3.24** (FetchContent with `FIND_PACKAGE_ARGS`, `SYSTEM` on
  `FetchContent_Declare`).
- **C11**, no GNU extensions in first-party code:
  `set(CMAKE_C_STANDARD 11)`, `CMAKE_C_STANDARD_REQUIRED ON`,
  `CMAKE_C_EXTENSIONS OFF`.
- Compilers: GCC/Clang on Linux (primary), MSVC kept possible by avoiding
  POSIX-only APIs outside a thin layer.
- **No C++ anywhere**: the project declares `project(myecs C)` — a stray `.cpp`
  won't even configure. All chosen dependencies are pure C.

## Warnings are errors

All **first-party** targets (`engine`, `examples/*`, `tests/*`) compile with:

```cmake
add_library(mye_flags INTERFACE)
target_compile_options(mye_flags INTERFACE
  $<$<C_COMPILER_ID:GNU,Clang>:
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes -Wdouble-promotion
    -Wvla -Wcast-qual -Wpointer-arith
    -Werror>
  $<$<C_COMPILER_ID:MSVC>: /W4 /WX>)
```

Any warning fails the build — this is the policy, not a suggestion. Two
consequences to respect:

- **Third-party code is exempt.** raylib/flecs/ck/TLSF are added via
  FetchContent with `SYSTEM` (their headers become `-isystem`, so their
  warnings never reach us) and `-Werror` is never applied to their targets.
- `-Wconversion` is strict about float/int mixing — expect explicit casts when
  interfacing with raylib's `float`-heavy API. That is the point: the casts
  become intentional and visible.

Escape hatch, used sparingly and always with a comment explaining why:
`#pragma GCC diagnostic push/ignored/pop` around a specific line.

## Dependencies (FetchContent)

Pinned in `cmake/MyeDependencies.cmake`: **raylib 6.0**, **flecs v4.1.6**
(link target `flecs::flecs_static`).

```cmake
FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG        6.0     GIT_SHALLOW TRUE  SYSTEM)
FetchContent_Declare(flecs
  GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
  GIT_TAG        v4.1.6  GIT_SHALLOW TRUE  SYSTEM)
FetchContent_MakeAvailable(raylib flecs)
```

**Do not put `cmake/` on `CMAKE_MODULE_PATH`.** Dependencies `include()` their
own modules by bare name, and our directory would shadow them — raylib does
`include(CompilerFlags)` and picked up ours, failing the configure. Our
modules are therefore prefixed (`MyeCompilerFlags.cmake`,
`MyeDependencies.cmake`) and included by full path.

- Pin exact tags (never a branch) so builds are reproducible; bump
  deliberately.
- raylib options: `BUILD_EXAMPLES OFF`, `BUILD_GAMES OFF`, static lib.
- flecs options: build the static target; enable `FLECS_REST` +
  `FLECS_STATS` in Debug for the Explorer, off in Release.
- Added later, at their milestone: **Concurrency Kit** (M4, channels — system
  package or FetchContent), **TLSF** (only if [04-memory.md](04-memory.md)
  says we need it), **mimalloc** (optional, Release).

## Targets

| Target | Type | Notes |
|---|---|---|
| `engine` | STATIC lib | links raylib + flecs PUBLIC; `mye_flags` PRIVATE; headers under `engine/` exported as include dir |
| `example_00_hello` … | executables | one per `examples/*`; link `engine` (except 00_hello, which links raylib alone as a toolchain proof) |
| `test_unit_<module>` | executables | one per unit test file, `tests/unit/test_<module>.c` |
| `test_int_<feature>` | executables | one per integration test, `tests/integration/` |

All test targets are registered with CTest; `ctest` runs the suite, and
render-dependent tests carry the `render` label so headless machines can skip
them (`ctest -LE render`). Details in [09-testing.md](09-testing.md).

## Build configurations

| Config | Flags |
|---|---|
| **Debug** | `-O0 -g3`, `-fsanitize=address,undefined -fno-omit-frame-pointer`, `MYE_DEBUG` defined, flecs REST/stats on, allocator tracking on |
| **RelWithDebInfo** | `-O2 -g`, no sanitizers — for profiling |
| **Release** | `-O2` (`-O3` only if measured better), `NDEBUG`, asserts off, tracking off |
| **TSan** (M7+) | `-O1 -g -fsanitize=thread` — separate config; **cannot** be combined with ASan |

Sanitizer flags are applied to first-party targets and the final links; the
suite is expected to be clean under them (`ASAN_OPTIONS`/`UBSAN_OPTIONS` with
`halt_on_error=1` in CTest environment).

## Developer workflow

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure
./build/debug/examples/example_02_asteroids
```

`compile_commands.json` is exported (`CMAKE_EXPORT_COMPILE_COMMANDS ON`) for
clangd. A `.clang-format` (C, 4 spaces, 100 cols) and `clang-tidy` config
(bugprone/cert/readability subsets) are optional extras, not yet added.
There is no CI: the three build configurations are run by hand
(see [09-testing.md](09-testing.md)).
