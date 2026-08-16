/* Allocator interface and backends. See plan/04-memory.md.
 *
 * ENGINE RULE: every engine interface that allocates takes a `mye_allocator`
 * by value. There is no hidden global allocator, and no engine code calls
 * malloc/free directly -- it calls mye_alloc/mye_free on an allocator it was
 * given. This makes lifetimes explicit, allows arenas/pools to be swapped in
 * per subsystem, and makes leak tracking exhaustive.
 *
 *   mye_allocator a = mye_heap_allocator();      // or arena / pool / tracking
 *   Thing *t = MYE_NEW(a, Thing);
 *   MYE_DELETE(a, t);
 *
 * Sizes are passed back on free (Zig/Odin style). That is deliberate: it lets
 * arenas and pools store zero per-allocation metadata, and lets the tracking
 * allocator account bytes exactly without headers. Callers always know the
 * size -- it is sizeof(T) or the capacity they asked for.
 */
#ifndef MYE_CORE_ALLOC_H
#define MYE_CORE_ALLOC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------ interface -- */

typedef struct mye_allocator_vtable {
    /* Returns NULL on failure. `align` must be a power of two. */
    void *(*alloc)(void *ctx, size_t size, size_t align);
    /* Grow/shrink. Returns NULL on failure, in which case `ptr` is untouched
     * and still owned by the caller. */
    void *(*resize)(void *ctx, void *ptr, size_t old_size, size_t new_size,
                    size_t align);
    /* `ptr` may be NULL (no-op). `size` must match the allocated size. */
    void (*release)(void *ctx, void *ptr, size_t size);
} mye_allocator_vtable;

typedef struct mye_allocator {
    const mye_allocator_vtable *vt;
    void *ctx;
} mye_allocator;

#define MYE_DEFAULT_ALIGN (_Alignof(max_align_t))

void *mye_alloc(mye_allocator a, size_t size, size_t align);
void *mye_alloc_zeroed(mye_allocator a, size_t size, size_t align);
void *mye_resize(mye_allocator a, void *ptr, size_t old_size, size_t new_size,
                 size_t align);
void mye_free(mye_allocator a, void *ptr, size_t size);

/* True if the allocator is usable (has a vtable). */
bool mye_allocator_valid(mye_allocator a);

#define MYE_NEW(a_, T_) ((T_ *)mye_alloc_zeroed((a_), sizeof(T_), _Alignof(T_)))
#define MYE_NEW_ARRAY(a_, T_, n_)                                              \
    ((T_ *)mye_alloc_zeroed((a_), sizeof(T_) * (size_t)(n_), _Alignof(T_)))
#define MYE_DELETE(a_, p_) mye_free((a_), (p_), sizeof *(p_))
#define MYE_DELETE_ARRAY(a_, p_, n_)                                           \
    mye_free((a_), (p_), sizeof *(p_) * (size_t)(n_))

/* Round `value` up to a multiple of `align` (power of two). Saturates to
 * SIZE_MAX on overflow so callers can detect it. */
size_t mye_align_up(size_t value, size_t align);

/* ------------------------------------------------- third-party adapters -- */

/* malloc/free-shaped wrappers for libraries that free without telling us the
 * size (flecs `ecs_os_api`, raylib's RL_MALLOC macros). The block size is
 * stashed in a header ahead of the payload, so those allocations still route
 * through -- and are counted by -- a mye_allocator.
 *
 * Payloads keep MYE_DEFAULT_ALIGN alignment. Blocks from this family must be
 * freed with mye_free_hdr, never mye_free, and vice versa. */
void *mye_alloc_hdr(mye_allocator a, size_t size);
void *mye_alloc_hdr_zeroed(mye_allocator a, size_t size);
void *mye_resize_hdr(mye_allocator a, void *ptr, size_t new_size);
void mye_free_hdr(mye_allocator a, void *ptr);
/* Payload bytes originally requested for a mye_alloc_hdr block. */
size_t mye_hdr_payload_size(const void *ptr);

/* Points raylib's RL_MALLOC family (see core/rl_alloc.h) at this allocator.
 * Reset returns raylib to the plain heap -- call it before the allocator it
 * was given stops being valid. */
void mye_rl_alloc_set(mye_allocator allocator);
void mye_rl_alloc_reset(void);

/* ----------------------------------------------------------------- heap -- */

/* malloc/free-backed, always available. Startup and long-lived data. */
mye_allocator mye_heap_allocator(void);

/* ---------------------------------------------------------------- arena -- */

/* Bump allocator over one backing block. Individual frees are no-ops; the
 * whole thing is reclaimed with reset/deinit. Used for scene-lifetime data
 * and (reset every frame) as the frame allocator. */
typedef struct mye_arena {
    uint8_t *base;
    size_t capacity;
    size_t used;
    size_t high_water;
    mye_allocator backing;
} mye_arena;

bool mye_arena_init(mye_arena *arena, mye_allocator backing, size_t capacity);

/* Arena over memory the caller owns (a stack or static buffer). Nothing is
 * allocated and deinit frees nothing -- the buffer must outlive the arena.
 * Allocations honour their requested alignment regardless of where `buffer`
 * happens to start. */
bool mye_arena_init_from_buffer(mye_arena *arena, void *buffer, size_t size);

void mye_arena_deinit(mye_arena *arena);
mye_allocator mye_arena_allocator(mye_arena *arena);

/* Frees everything at once. The memory block itself is kept. */
void mye_arena_reset(mye_arena *arena);

size_t mye_arena_used(const mye_arena *arena);
size_t mye_arena_capacity(const mye_arena *arena);
size_t mye_arena_high_water(const mye_arena *arena);

/* Scratch usage: take a mark, allocate freely, rewind back to the mark. */
typedef size_t mye_arena_mark;
mye_arena_mark mye_arena_take_mark(const mye_arena *arena);
void mye_arena_rewind(mye_arena *arena, mye_arena_mark mark);

/* ----------------------------------------------------------------- pool -- */

/* Fixed-size block allocator with an intrusive free list. O(1) alloc/free,
 * no fragmentation. For churny same-size objects: particles, asset slots. */
typedef struct mye_pool {
    uint8_t *blocks;
    void *free_list;
    size_t stride;   /* bytes per block, >= elem_size, aligned */
    size_t capacity; /* number of blocks */
    size_t live;     /* blocks currently handed out */
    size_t peak;
    size_t align;
    mye_allocator backing;
} mye_pool;

bool mye_pool_init(mye_pool *pool, mye_allocator backing, size_t elem_size,
                   size_t elem_align, size_t capacity);
void mye_pool_deinit(mye_pool *pool);
mye_allocator mye_pool_allocator(mye_pool *pool);

void *mye_pool_alloc(mye_pool *pool); /* NULL when exhausted */
void mye_pool_free(mye_pool *pool, void *ptr);
void mye_pool_reset(mye_pool *pool); /* frees every block at once */

size_t mye_pool_live(const mye_pool *pool);
size_t mye_pool_capacity(const mye_pool *pool);
size_t mye_pool_peak(const mye_pool *pool);
/* True if `ptr` points at a block owned by this pool and correctly aligned. */
bool mye_pool_owns(const mye_pool *pool, const void *ptr);

/* ------------------------------------------------------------- tracking -- */

/* Decorator counting allocations of any backing allocator. Debug builds wrap
 * engine allocators in this so shutdown can report leaks. */
/* Counters are atomic because allocation is genuinely a hot spot touched from
 * several threads: asset workers allocate and free through the same allocator
 * as the main thread, and flecs' worker pipeline will too (M7). A mutex here
 * would serialise every allocation in the engine; relaxed atomic counters
 * cost an interlocked add. See the primitive policy in
 * plan/05-concurrency.md. */
typedef struct mye_tracking {
    mye_allocator backing;
    _Atomic size_t live_bytes;
    _Atomic size_t live_count;
    _Atomic size_t peak_bytes;
    _Atomic size_t total_allocs;
    _Atomic size_t total_frees;
    _Atomic size_t failed_allocs;
} mye_tracking;

void mye_tracking_init(mye_tracking *t, mye_allocator backing);
mye_allocator mye_tracking_allocator(mye_tracking *t);
/* Non-zero live_count/live_bytes at shutdown means a leak. */
bool mye_tracking_has_leaks(const mye_tracking *t);
void mye_tracking_report(const mye_tracking *t, const char *label);

#endif /* MYE_CORE_ALLOC_H */
