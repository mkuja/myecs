# 05 — Concurrency: Parallel Runtime & Channels

## Choosing a primitive (project policy)

1. **Mutex by default.**
2. **Atomics on hot spots** — the normal escalation once contention is real.
3. **Lock-free sometimes, and only at a hot spot** — additionally gated on
   there being a single writer, or on a stale read being harmless.

Lock-free is the *most* restricted tier, not a middle ground. Its correctness
cannot be shown by testing alone on x86, whose strong memory model forgives
missing acquire/release that ARM would not, so the review burden is permanent
while the benefit only exists where contention was measured.

Worked examples in this codebase:

| Site | Primitive | Why |
|---|---|---|
| `mye_jobs` worker sleep/wake | mutex + condvar | not hot; workers must actually sleep, not spin |
| `mye_jobs.pending`, `.stopping` | atomics | single writer per transition, stale read is harmless |
| `mye_channel` | mutex + condvar | a handful of messages per frame is not a hot spot, and it is multi-producer with messages that must not be lost — which fails the lock-free gate outright |

## Honest framing (Tokio? Rayon?)

C's standard library has no equivalent of Tokio or Rayon. But a game engine
doesn't want an async I/O runtime (Tokio solves massive concurrent I/O —
servers). What a game wants is:

1. **Data-parallel systems** (the Rayon-shaped need) — run one system's
   entity iteration across cores.
2. **Background jobs** — file I/O and decoding off the main thread.
3. **Channels** — safe messages between threads.

We get (1) for free from flecs, and use small pure-C pieces for (2) and (3).
Nothing here is C++; nothing here is hand-rolled lock-free code.

## 1. Parallel systems — flecs pipeline workers

flecs has a built-in worker-thread pipeline (its docs call it exactly the
Rayon-style model):

```c
ecs_set_threads(world, 8);              // spawn workers (uses OS threads)

ecs_system(world, {
    .entity = ecs_entity(world, { .add = ecs_ids(ecs_dependson(EcsOnUpdate)) }),
    .query.terms = {{ ecs_id(MyePosition2D) }, { ecs_id(MyeVelocity2D) }},
    .callback = Move,
    .multi_threaded = true,             // ← shard matched tables across workers
});
```

`ecs_progress()` splits each `multi_threaded` system's matched entities across
the workers and joins before the next sync point. Systems not marked
`multi_threaded` (all render systems!) run on the main thread. Structural
changes made inside systems are deferred per-thread and merged at sync points
— that's the safety model.

**Policy:**

- Ship everything single-threaded first (`ecs_set_threads(world, 1)` /
  default). Turn `multi_threaded` on per-system in **M7**, after profiling,
  starting with pure data-transform systems (movement, animation timers,
  transform propagation).
- A system is eligible for `multi_threaded` only if its callback touches
  nothing but its own query fields and read-only singletons. No raylib calls,
  no globals, no allocation from shared non-thread-safe allocators.

## 2. Background jobs — a small worker pool

For asset I/O (M4) a full job system is overkill. Design: **one internal
worker pool** (N = cores−1, min 1) built on **C11 `<threads.h>`**
(`thrd_create`, `mtx_t`, `cnd_t`; glibc has supported this since 2.28 —
fallback shim to pthreads only if another platform ever needs it).

```c
typedef void (*mye_job_fn)(void *arg);
bool mye_jobs_submit(mye_job_fn fn, void *arg);   // enqueue, wakes a worker
```

Jobs must be self-contained: they own their inputs, produce a result message,
and push it into a channel. They never touch the ECS world (flecs worlds are
not thread-safe from outside `ecs_progress`) and never call raylib GPU/window
functions. If later demos need Rayon-style `parallel_for` over raw arrays,
extend this pool — but flecs already covers the common case.

## 3. Channels — Concurrency Kit

For thread↔thread messaging we use **Concurrency Kit**
(`ck`, pure C, BSD license, in Arch/Debian repos and buildable via
FetchContent): specifically **`ck_ring`**, a fixed-size lock-free ring buffer
with SPSC/MPSC/SPMC/MPMC operation modes.

Wrapped in `engine/core/channel.h` so call sites stay simple and the backing
library is swappable:

```c
typedef struct mye_channel mye_channel;          // fixed capacity, pow2
mye_channel *mye_channel_create(size_t capacity, size_t elem_size);
bool mye_channel_send(mye_channel *c, const void *msg);   // false = full
bool mye_channel_recv(mye_channel *c, void *out);         // false = empty
```

Non-blocking by design: producers handle "full" (retry/drop/grow policy per
call site), the main thread drains with `while (recv())` in a system — never
blocks the frame.

**Primary use — async asset loading (M4):**

```text
main thread                          worker pool
───────────                          ───────────
mye_asset_load_async(path) ──req──▶  read file, decode image/mesh (CPU only)
                                     │
AssetUploadSystem (EcsPreStore) ◀─done┘  {handle, pixels}
  drains channel, LoadTextureFromImage  (GPU upload, main thread only)
  marks handle LOADED
```

"Entities communicating between threads" from the original wish list maps to
this pattern: threads exchange *messages* (plain structs, possibly containing
entity IDs), never shared mutable component data. An `ecs_entity_t` is just a
64-bit ID and is safe to put in a message; the receiving side touches the
world only from the main thread / inside systems.

## Thread-ownership rules (the contract)

| Resource | Owner | Others may |
|---|---|---|
| Window, OpenGL context, all draw/GPU-upload calls | main thread | never touch |
| Audio device | main thread | never touch |
| `ecs_world_t` | main thread (`ecs_progress`) + flecs' own workers inside it | never touch from job threads |
| Files on disk | any job thread | — |
| Channels | shared | send/recv per declared mode |

## Measured results (M7)

Worker threads were enabled and benchmarked rather than assumed. Release
build, 16 cores, headless, `examples/04_stress`:

| Entities | 1 thread | 8 threads | 16 threads | speedup |
|---|---|---|---|---|
| 50,000 | 0.77 ms | 0.10 ms | 0.07 ms | 10.9x |
| 200,000 | 2.85 ms | 0.39 ms | 0.27 ms | 10.6x |

The crossover is the practical finding: **below roughly 1,000 entities per
system, threading costs more than it saves** -- at 100 entities, 8 threads
runs 4x slower than one. Workers are therefore off by default
(`mye_config.worker_threads`), and a system is only sharded when it is
explicitly marked `.multi_threaded = true`.

Determinism holds: `test_int_workers.c` compares a threaded run against a
serial one and requires bit-identical results, because per-entity arithmetic
does not depend on visit order.

## Resolved hazard: the tracking allocator (was not thread-safe)

Fixed in M4, before workers existed: the counters are `_Atomic` with relaxed
ordering and a CAS loop for the peak. TSan found 30 races there the moment
asset workers began allocating. Enabling flecs workers in M7 was TSan-clean
precisely because this had already been dealt with.

## Historical note: the hazard as originally written

flecs allocates through our allocator (`ecs_os_api`, wired in M2), and once
`ecs_set_threads()` is used it will do so **from worker threads**. The
tracking decorator's counters (`live_bytes`, `live_count`, …) are plain
`size_t` increments — a data race the moment workers exist.

Before enabling workers in M7, one of:

1. make the tracking counters `_Atomic` (simple; costs a little on every
   allocation, only in builds that use tracking), or
2. give each thread its own counters and sum them on report, or
3. bypass tracking for the flecs bridge and accept losing that visibility.

Option 1 is the default choice. The hazard is recorded in a comment on
`g_flecs_allocator` in `engine/core/engine.c`; TSan is expected to flag this
immediately if it is forgotten.

## Beginner guardrails

- **Don't parallelize until it's slow.** Every milestone through M6 runs
  single-threaded; M7 turns flecs workers on as a measured experiment.
- Debug builds run with ThreadSanitizer in a dedicated CI job once threads
  exist (TSan and ASan are mutually exclusive — separate build config,
  see [08-build.md](08-build.md)).
- Integration tests for the async loader and channels are deterministic:
  bounded queues, explicit drain loops, no sleeps-as-synchronization
  ([09-testing.md](09-testing.md)).
