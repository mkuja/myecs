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

## What a game engine actually needs from concurrency

C's standard library gives you threads and atomics and nothing above them.
A game does not need a general async I/O runtime -- that solves the server
problem, thousands of concurrent connections. A game needs three things:

1. **Data-parallel systems** — run one system's entity iteration across
   cores.
2. **Background jobs** — file I/O and decoding off the main thread.
3. **Channels** — safe messages between threads.

We get (1) for free from flecs, and use small pure-C pieces for (2) and (3).
Nothing here is C++; nothing here is hand-rolled lock-free code.

## 1. Parallel systems — flecs pipeline workers

flecs has a built-in worker-thread pipeline:

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
worker pool** (N = cores−1, min 1) behind `engine/core/thread.h`. The
original plan said C11 `<threads.h>` with a pthreads fallback; the shipped
arrangement is the inverse -- **pthreads on POSIX, C11 threads elsewhere** --
because TSan cannot instrument glibc's `<threads.h>` (DEADLYSIGNAL under gcc
and clang both), and a thread layer the sanitizer cannot see is worse than a
wrapper.

```c
typedef void (*mye_job_fn)(void *arg);
bool mye_jobs_submit(mye_job_fn fn, void *arg);   // enqueue, wakes a worker
```

Jobs must be self-contained: they own their inputs, produce a result message,
and push it into a channel. They never touch the ECS world (flecs worlds are
not thread-safe from outside `ecs_progress`) and never call raylib GPU/window
functions. If later demos need a parallel-for over raw arrays, extend this
pool — but flecs already covers the common case.

## 3. Channels

For thread↔thread messaging: `engine/core/channel.h`, a **mutex-protected
ring buffer**. Concurrency Kit's `ck_ring` was the original candidate here
and was never adopted -- the primitive policy above decided it: a handful of
messages per frame is not a hot spot, and the channel is multi-producer with
messages that must not be lost, which is exactly the "mutex by default" case.
No third-party dependency was added.

The real signatures (the allocator comes first, like every allocating engine
interface):

```c
typedef struct mye_channel mye_channel;          // fixed capacity
mye_channel *mye_channel_create(mye_allocator a, size_t capacity,
                                size_t elem_size);
bool mye_channel_send(mye_channel *c, const void *msg);   // false = full
bool mye_channel_recv(mye_channel *c, void *out);         // false = empty
```

Non-blocking by design: producers handle "full" (retry/drop/grow policy per
call site), the main thread drains with `while (recv())` in a system — never
blocks the frame.

**The shape it was designed for** (this pipeline was built, then removed --
assets load at scene boundaries now; see [06-assets.md](06-assets.md). It is
kept here because it is the pattern the channel exists for. Be aware of the
consequence: with asset workers gone, **nothing in the engine consumes
`mye_channel` or `mye_jobs` today** -- the WebSocket transport is
deliberately single-threaded and pump-per-frame, not channel-fed. They are
the documented pattern for a game's own threads (TUTORIAL §17), kept tested;
whether they stay is an open decision recorded in
[15-gaps.md](15-gaps.md)):

```text
main thread                          worker pool
───────────                          ───────────
submit(work) ─────────────req──────▶  do the CPU-only part off the frame
                                      │
a system, once per frame  ◀────done───┘  {handle, result}
  drains the channel and finishes the
  part that must be on the main thread
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
- ThreadSanitizer gets its own build directory, since it cannot share one
  with ASan (see [08-build.md](08-build.md)). Run `tools/check.sh` whenever
  the channel, the job pool, logging or flecs workers change — TSan has
  caught a race in every one of those, including one introduced the same day.
- Integration tests for the async loader and channels are deterministic:
  bounded queues, explicit drain loops, no sleeps-as-synchronization
  ([09-testing.md](09-testing.md)).
