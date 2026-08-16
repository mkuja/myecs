# 04 — Memory & Allocators

> **Status: implemented** (M1) — `engine/core/alloc.[ch]`, 18 unit tests in
> `tests/unit/test_alloc.c`.

## The engine rule

**Every engine interface that allocates takes a `mye_allocator` by value.**
No hidden global allocator; no engine code calls `malloc`/`free` directly. A
subsystem allocates only from the allocator it was handed.

```c
bool mye_thing_init(mye_thing *t, mye_allocator a, size_t capacity);
```

This is why allocators were built before any other subsystem — the rule costs
nothing to follow from the first line of engine code and is expensive to
retrofit. It buys: explicit lifetimes, per-subsystem arenas/pools swappable
without touching call sites, and leak tracking that misses nothing.

## Strategy in one paragraph

Write the small, game-specific allocators in-house (they are each ~100–300
lines of C and there is no good standalone C library for them), and use
existing pure-C libraries only where a real library exists: **TLSF** for an
O(1) real-time heap if we ever need one, **mimalloc** as an optional drop-in
`malloc` replacement for release builds. flecs and raylib keep managing their
own internals, but both expose allocator hooks we plug into so *all*
allocations are visible to our tracking layer.

## In-house allocators (`engine/core/alloc.[ch]`)

| Allocator | Use case | API sketch |
|---|---|---|
| **Heap** | Startup and long-lived data. `aligned_alloc`/`free` behind the interface. | `mye_heap_allocator()` |
| **Arena (bump)** | Scene lifetime data: load a scene into an arena, drop the whole arena on unload. Doubles as the frame allocator when reset each frame. | `mye_arena_init(&a, backing, bytes)` · `mye_arena_reset` · `mye_arena_take_mark`/`mye_arena_rewind` · `mye_arena_allocator(&a)` |
| **Pool** | Many same-size objects with churn (particles, audio voices, asset slots). O(1) alloc/free via an intrusive free list. | `mye_pool_init(&p, backing, elem_size, elem_align, count)` · `mye_pool_alloc`/`_free` · `mye_pool_owns` |
| **Tracking** | Debug decorator over any of the above: counts, peak, leak detection. | `mye_tracking_init(&t, backing)` · `mye_tracking_has_leaks` · `mye_tracking_report` |

Every backend exposes itself as the same by-value handle, so code takes "an
allocator" without caring which — and backends compose (tracking over arena
over heap):

```c
typedef struct mye_allocator {
    const mye_allocator_vtable *vt;   /* alloc / resize / release */
    void *ctx;
} mye_allocator;
```

Rules:

- Alignment-correct by construction (`align` parameter, power of two;
  `MYE_DEFAULT_ALIGN` = `_Alignof(max_align_t)`).
- **Sizes are passed back on free** (`mye_free(a, ptr, size)`, Zig/Odin
  style). Arenas and pools then need zero per-allocation metadata, and
  tracking accounts bytes exactly without headers. Callers always know the
  size — it is `sizeof(T)` or the capacity they asked for, and the
  `MYE_DELETE`/`MYE_DELETE_ARRAY` macros supply it.
- Out-of-memory returns `NULL`; callers decide (engine startup treats it as
  fatal, gameplay paths must handle it). A failed `resize` leaves the original
  block owned by the caller.
- Arena frees are no-ops **except** for the topmost block, which rolls the
  bump pointer back — so freeing in reverse order reclaims everything, and
  growing the most recent allocation extends in place.
- No hidden global allocator: modules receive or own their allocators
  explicitly.

### Adapting to third-party APIs

Libraries that free without a size (flecs `ecs_os_api`, raylib `RL_MALLOC`)
cannot use the interface directly. A sized-header adapter bridges them —
allocate `size + header`, stash the size, hand back the offset pointer:

```c
void *mye_alloc_hdr(mye_allocator a, size_t size);   /* malloc-shaped */
void  mye_free_hdr(mye_allocator a, void *ptr);      /* free-shaped   */
```

This keeps third-party allocations inside our tracking. Implemented in M2:
the flecs `ecs_os_api` routes through it, so `mye_shutdown()` can report a
true zero.

## Debug tracking wrapper

A `mye_allocator` decorator used in Debug builds around any allocator
(including plain `malloc`):

- counts live allocations and bytes, records the high-water mark, and counts
  failed allocations (useful when an arena runs dry);
- follows `resize` so byte counts stay exact;
- `mye_tracking_report(&t, "assets")` at shutdown; non-zero live count is a
  leak and fails the test suite;
- ASan already catches overruns and leaks in Debug, so the tracking allocator
  earns its keep mainly by attributing memory *per subsystem* and by working
  in release-with-tracking builds. Per-allocation file/line capture can be
  layered on later if a leak proves hard to place.

## Libraries

- **TLSF** (two-level segregated fit, pure C, BSD): O(1) malloc/free with low,
  bounded fragmentation inside a fixed memory block. Adopt only when a
  general-purpose sub-heap is genuinely needed (e.g. an asset heap with
  unpredictable sizes and lifetimes). Wrapped as a `mye_allocator`.
- **mimalloc** (pure C, MIT): optional global `malloc` replacement for release
  performance. Not a design dependency — flipping it on/off must change
  nothing but speed.

## Plugging into flecs and raylib

Both foundations route their allocations through overridable hooks — we point
them at the tracked allocator so the debug report covers everything:

```c
// flecs: before ecs_init()
ecs_os_set_api_defaults();
ecs_os_api_t api = ecs_os_get_api();
api.malloc_ = mye_os_malloc;  api.free_ = mye_os_free;
api.calloc_ = mye_os_calloc;  api.realloc_ = mye_os_realloc;
ecs_os_set_api(&api);

// raylib: the RL_* macros are redefined in engine/core/rl_alloc.h, which is
// force-included into every raylib translation unit (-include) so it lands
// ahead of raylib.h/rlgl.h/raudio.c, whose own definitions are #ifndef-guarded.
```

Two traps, both hit during implementation and both now handled in
`cmake/MyeDependencies.cmake`:

- **CMake does not reliably pass function-like macros** through
  `target_compile_definitions`, so `-DRL_MALLOC(sz)=...` silently did nothing.
  The definitions live in the force-included header instead.
- **A force-included header is not a tracked dependency**, so editing it left
  raylib's objects stale — reverting to plain `malloc` while every flag still
  looked right. `OBJECT_DEPENDS` now declares it explicitly.

`tests/unit/test_rl_alloc.c` guards both: it asserts a `GenImageColor` shows up
in the tracking allocator's byte count, which fails if the redirection is not
actually in force.

## Ownership rules

| Data | Owner / lifetime | Allocator |
|---|---|---|
| ECS storage (tables, components) | flecs, world lifetime | flecs via hooked OS API |
| Asset CPU data (decoded images, model buffers) | asset registry; ref-counted, freed on unload | asset arena or TLSF heap |
| Asset GPU objects (textures, meshes) | asset registry; freed on unload via raylib `Unload*` | GPU (driver) |
| Scene entities & scene-scoped allocations | scene; freed on scene unload | scene arena |
| Per-frame temporaries (draw lists, scratch) | one frame | frame allocator |
| Engine singletons, registries | engine init → shutdown | startup arena or malloc |

## Milestone & testing

Allocators land in **M3** ([07-roadmap.md](07-roadmap.md)) with full unit
tests: alignment, exhaustion → `NULL`, reset semantics, pool reuse patterns,
tracking counts, leak detection — all headless, all under ASan/UBSan
([09-testing.md](09-testing.md)). Until M3, engine code uses plain
`malloc`/`free` behind the `mye_allocator` interface so the swap is painless.
