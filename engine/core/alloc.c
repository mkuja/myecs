#include "core/alloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <malloc.h> /* _aligned_malloc / _aligned_free */
#endif

/* ------------------------------------------------------------- helpers -- */

static bool is_power_of_two(size_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

size_t mye_align_up(size_t value, size_t align)
{
    assert(is_power_of_two(align));
    size_t mask = align - 1;
    if (value > SIZE_MAX - mask) {
        return SIZE_MAX; /* saturate: caller detects the overflow */
    }
    return (value + mask) & ~mask;
}

bool mye_allocator_valid(mye_allocator a)
{
    return a.vt != NULL && a.vt->alloc != NULL && a.vt->release != NULL;
}

/* ----------------------------------------------------------- interface -- */

void *mye_alloc(mye_allocator a, size_t size, size_t align)
{
    if (!mye_allocator_valid(a) || size == 0 || !is_power_of_two(align)) {
        return NULL;
    }
    return a.vt->alloc(a.ctx, size, align);
}

void *mye_alloc_zeroed(mye_allocator a, size_t size, size_t align)
{
    void *p = mye_alloc(a, size, align);
    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

void *mye_resize(mye_allocator a, void *ptr, size_t old_size, size_t new_size,
                 size_t align)
{
    if (!mye_allocator_valid(a) || !is_power_of_two(align)) {
        return NULL;
    }
    if (ptr == NULL) {
        return mye_alloc(a, new_size, align);
    }
    if (old_size == 0) {
        /* A live block cannot be zero-sized; allocating fresh here would
         * silently abandon `ptr`. Refuse instead: NULL, ptr untouched. */
        return NULL;
    }
    if (new_size == 0) {
        mye_free(a, ptr, old_size);
        return NULL;
    }
    if (a.vt->resize != NULL) {
        return a.vt->resize(a.ctx, ptr, old_size, new_size, align);
    }
    /* Generic fallback for backends without a native resize. */
    void *fresh = mye_alloc(a, new_size, align);
    if (fresh == NULL) {
        return NULL; /* ptr still owned by the caller */
    }
    memcpy(fresh, ptr, old_size < new_size ? old_size : new_size);
    mye_free(a, ptr, old_size);
    return fresh;
}

void mye_free(mye_allocator a, void *ptr, size_t size)
{
    if (ptr == NULL || !mye_allocator_valid(a)) {
        return;
    }
    a.vt->release(a.ctx, ptr, size);
}

static bool array_size(size_t elem_size, size_t count, size_t *out_size)
{
    /* elem_size 0 is refused on both paths: the alloc could only return
     * NULL anyway, and a size-0 free would desync tracking's byte count. */
    if (elem_size == 0 || count > SIZE_MAX / elem_size) {
        return false;
    }
    *out_size = elem_size * count;
    return true;
}

void *mye_alloc_array_zeroed(mye_allocator a, size_t elem_size, size_t count,
                             size_t align)
{
    size_t size = 0;
    if (!array_size(elem_size, count, &size)) {
        return NULL;
    }
    return mye_alloc_zeroed(a, size, align);
}

void mye_free_array(mye_allocator a, void *ptr, size_t elem_size, size_t count)
{
    size_t size = 0;
    if (!array_size(elem_size, count, &size)) {
        return; /* the checked alloc never handed out such a block */
    }
    mye_free(a, ptr, size);
}

/* ---------------------------------------------------- header adapters -- */

/* One header slot, sized to preserve payload alignment. */
#define MYE_HDR_BYTES ((size_t)MYE_DEFAULT_ALIGN)

typedef struct mye_hdr {
    size_t payload_size;
} mye_hdr;

_Static_assert(MYE_HDR_BYTES >= sizeof(mye_hdr),
               "header slot must fit the stored size");

static uint8_t *hdr_block_of(const void *payload)
{
    /* Cast away const: the caller owns this block and is about to mutate or
     * free it; only the lookup is read-only. */
    return (uint8_t *)(uintptr_t)payload - MYE_HDR_BYTES;
}

void *mye_alloc_hdr(mye_allocator a, size_t size)
{
    if (size == 0 || size > SIZE_MAX - MYE_HDR_BYTES) {
        return NULL;
    }
    uint8_t *block = (uint8_t *)mye_alloc(a, size + MYE_HDR_BYTES,
                                          MYE_DEFAULT_ALIGN);
    if (block == NULL) {
        return NULL;
    }
    mye_hdr hdr = { .payload_size = size };
    memcpy(block, &hdr, sizeof hdr);
    return block + MYE_HDR_BYTES;
}

void *mye_alloc_hdr_zeroed(mye_allocator a, size_t size)
{
    void *p = mye_alloc_hdr(a, size);
    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

size_t mye_hdr_payload_size(const void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }
    mye_hdr hdr;
    memcpy(&hdr, hdr_block_of(ptr), sizeof hdr);
    return hdr.payload_size;
}

void *mye_resize_hdr(mye_allocator a, void *ptr, size_t new_size)
{
    if (ptr == NULL) {
        return mye_alloc_hdr(a, new_size);
    }
    if (new_size == 0) {
        mye_free_hdr(a, ptr);
        return NULL;
    }
    if (new_size > SIZE_MAX - MYE_HDR_BYTES) {
        return NULL;
    }

    size_t old_size = mye_hdr_payload_size(ptr);
    uint8_t *block = hdr_block_of(ptr);
    uint8_t *fresh = (uint8_t *)mye_resize(a, block, old_size + MYE_HDR_BYTES,
                                           new_size + MYE_HDR_BYTES,
                                           MYE_DEFAULT_ALIGN);
    if (fresh == NULL) {
        return NULL; /* original block still owned by the caller */
    }
    mye_hdr hdr = { .payload_size = new_size };
    memcpy(fresh, &hdr, sizeof hdr);
    return fresh + MYE_HDR_BYTES;
}

void mye_free_hdr(mye_allocator a, void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    uint8_t *block = hdr_block_of(ptr);
    mye_free(a, block, mye_hdr_payload_size(ptr) + MYE_HDR_BYTES);
}

/* ---------------------------------------------------------------- heap -- */

/* On Windows, MSVC's UCRT has no C11 aligned_alloc -- its free() cannot
 * release aligned blocks -- so the _aligned_* pair must be used TOGETHER:
 * swapping only one side corrupts the heap. That is why alloc and release
 * switch on the same condition. (Untested here: no Windows toolchain in the
 * build matrix; see plan/08-build.md.) */
static void *heap_alloc(void *ctx, size_t size, size_t align)
{
    (void)ctx;
    /* C11 aligned_alloc requires size to be a multiple of alignment. */
    size_t padded = mye_align_up(size, align);
    if (padded == SIZE_MAX) {
        return NULL;
    }
#if defined(_WIN32)
    return _aligned_malloc(padded, align);
#else
    return aligned_alloc(align, padded);
#endif
}

static void heap_release(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    (void)size;
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

static void *heap_resize(void *ctx, void *ptr, size_t old_size, size_t new_size,
                         size_t align)
{
    /* realloc() cannot honour an over-aligned request, so do it by hand. */
    void *fresh = heap_alloc(ctx, new_size, align);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, old_size < new_size ? old_size : new_size);
    heap_release(ctx, ptr, old_size);
    return fresh;
}

static const mye_allocator_vtable heap_vtable = {
    .alloc = heap_alloc,
    .resize = heap_resize,
    .release = heap_release,
};

mye_allocator mye_heap_allocator(void)
{
    return (mye_allocator){ .vt = &heap_vtable, .ctx = NULL };
}

/* --------------------------------------------------------------- arena -- */

/* True when [ptr, ptr+size) is the topmost live block, i.e. it ends exactly at
 * the bump pointer. Derived from the pointer rather than remembered, so LIFO
 * frees and resizes cascade correctly. Also returns the block's offset. */
static bool arena_is_top(const mye_arena *arena, const uint8_t *p, size_t size,
                         size_t *out_offset)
{
    /* Compare as integers: `p` may be a foreign pointer, and relational
     * pointer comparison is only defined within one object. */
    uintptr_t up = (uintptr_t)p;
    uintptr_t base = (uintptr_t)arena->base;
    if (arena->base == NULL || up < base || up > base + arena->used) {
        return false;
    }
    size_t offset = (size_t)(up - base); /* offset <= used */
    *out_offset = offset;
    return arena->used - offset == size;
}

static void arena_bump_used(mye_arena *arena, size_t used)
{
    arena->used = used;
    if (used > arena->high_water) {
        arena->high_water = used;
    }
}

static void *arena_alloc(void *ctx, size_t size, size_t align)
{
    mye_arena *arena = (mye_arena *)ctx;
    if (arena->base == NULL) {
        return NULL;
    }

    /* Align the ABSOLUTE address, not the offset: the arena's own base is
     * only guaranteed to meet MYE_DEFAULT_ALIGN (and may be a borrowed
     * buffer at any address), so aligning the offset would hand back a
     * misaligned pointer for any stricter request. */
    uintptr_t base = (uintptr_t)arena->base;
    uintptr_t cursor = base + (uintptr_t)arena->used;
    uintptr_t mask = (uintptr_t)align - 1;
    if (mask > UINTPTR_MAX - cursor) {
        return NULL; /* address-space overflow */
    }
    size_t offset = (size_t)(((cursor + mask) & ~mask) - base);

    if (offset > arena->capacity || size > arena->capacity - offset) {
        return NULL; /* exhausted */
    }
    arena_bump_used(arena, offset + size);
    return arena->base + offset;
}

static void *arena_resize(void *ctx, void *ptr, size_t old_size,
                          size_t new_size, size_t align)
{
    mye_arena *arena = (mye_arena *)ctx;
    const uint8_t *p = (const uint8_t *)ptr;
    size_t offset = 0;

    /* Growing the topmost allocation extends in place -- the common case for
     * a dynamic array built up inside a scratch region. Only when the block
     * already sits on the requested alignment: a resize may ask for stricter
     * alignment than the original allocation had, and handing the old
     * address back would violate it. */
    if (arena_is_top(arena, p, old_size, &offset) &&
        ((uintptr_t)ptr & ((uintptr_t)align - 1)) == 0) {
        if (new_size > arena->capacity - offset) {
            return NULL;
        }
        arena_bump_used(arena, offset + new_size);
        return ptr;
    }

    void *fresh = arena_alloc(ctx, new_size, align);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, old_size < new_size ? old_size : new_size);
    return fresh;
}

static void arena_release(void *ctx, void *ptr, size_t size)
{
    mye_arena *arena = (mye_arena *)ctx;
    size_t offset = 0;

    /* Freeing the topmost block rolls the bump pointer back, so freeing in
     * reverse order reclaims everything. Any other free is a no-op -- that is
     * the arena bargain. */
    if (arena_is_top(arena, (const uint8_t *)ptr, size, &offset)) {
        arena->used = offset;
    }
}

static const mye_allocator_vtable arena_vtable = {
    .alloc = arena_alloc,
    .resize = arena_resize,
    .release = arena_release,
};

bool mye_arena_init(mye_arena *arena, mye_allocator backing, size_t capacity)
{
    assert(arena != NULL);
    if (capacity == 0 || !mye_allocator_valid(backing)) {
        return false;
    }

    void *block = mye_alloc(backing, capacity, MYE_DEFAULT_ALIGN);
    if (block == NULL) {
        return false;
    }

    *arena = (mye_arena){
        .base = (uint8_t *)block,
        .capacity = capacity,
        .used = 0,
        .high_water = 0,
        .backing = backing,
    };
    return true;
}

bool mye_arena_init_from_buffer(mye_arena *arena, void *buffer, size_t size)
{
    assert(arena != NULL);
    if (buffer == NULL || size == 0) {
        return false;
    }

    /* An all-zero backing allocator is invalid, and mye_free() ignores those,
     * so deinit will not try to free memory the arena does not own. */
    *arena = (mye_arena){
        .base = (uint8_t *)buffer,
        .capacity = size,
        .used = 0,
        .high_water = 0,
        .backing = (mye_allocator){ 0 },
    };
    return true;
}

void mye_arena_deinit(mye_arena *arena)
{
    assert(arena != NULL);
    if (arena->base != NULL) {
        /* No-op for borrowed buffers: their backing allocator is invalid. */
        mye_free(arena->backing, arena->base, arena->capacity);
    }
    *arena = (mye_arena){ 0 };
}

mye_allocator mye_arena_allocator(mye_arena *arena)
{
    return (mye_allocator){ .vt = &arena_vtable, .ctx = arena };
}

void mye_arena_reset(mye_arena *arena)
{
    assert(arena != NULL);
    arena->used = 0;
}

size_t mye_arena_used(const mye_arena *arena) { return arena->used; }
size_t mye_arena_capacity(const mye_arena *arena) { return arena->capacity; }
size_t mye_arena_high_water(const mye_arena *arena) { return arena->high_water; }

mye_arena_mark mye_arena_take_mark(const mye_arena *arena)
{
    return arena->used;
}

void mye_arena_rewind(mye_arena *arena, mye_arena_mark mark)
{
    assert(arena != NULL);
    if (mark <= arena->used) {
        arena->used = mark;
    }
}

/* ---------------------------------------------------------------- pool -- */

static void *pool_alloc_vt(void *ctx, size_t size, size_t align)
{
    mye_pool *pool = (mye_pool *)ctx;
    if (size > pool->stride || align > pool->align) {
        return NULL; /* pool blocks cannot satisfy this request */
    }
    return mye_pool_alloc(pool);
}

static void pool_release_vt(void *ctx, void *ptr, size_t size)
{
    (void)size;
    mye_pool_free((mye_pool *)ctx, ptr);
}

static const mye_allocator_vtable pool_vtable = {
    .alloc = pool_alloc_vt,
    .resize = NULL, /* fixed-size blocks cannot be resized */
    .release = pool_release_vt,
};

bool mye_pool_init(mye_pool *pool, mye_allocator backing, size_t elem_size,
                   size_t elem_align, size_t capacity)
{
    assert(pool != NULL);
    if (elem_size == 0 || capacity == 0 || !is_power_of_two(elem_align) ||
        !mye_allocator_valid(backing)) {
        return false;
    }

    /* Blocks hold a free-list pointer while free, so they must fit one. */
    size_t align = elem_align < _Alignof(void *) ? _Alignof(void *) : elem_align;
    size_t size = elem_size < sizeof(void *) ? sizeof(void *) : elem_size;
    size_t stride = mye_align_up(size, align);
    if (stride == SIZE_MAX || stride > SIZE_MAX / capacity) {
        return false; /* total size would overflow */
    }

    void *blocks = mye_alloc(backing, stride * capacity, align);
    if (blocks == NULL) {
        return false;
    }
    /* One liveness bit per block, so a double free is detected instead of
     * silently corrupting the free list. capacity <= SIZE_MAX / stride, so
     * the + 7 cannot wrap. */
    uint8_t *live_bits =
        (uint8_t *)mye_alloc_zeroed(backing, (capacity + 7) / 8, 1);
    if (live_bits == NULL) {
        mye_free(backing, blocks, stride * capacity);
        return false;
    }

    *pool = (mye_pool){
        .blocks = (uint8_t *)blocks,
        .free_list = NULL,
        .live_bits = live_bits,
        .double_frees = 0,
        .stride = stride,
        .capacity = capacity,
        .live = 0,
        .peak = 0,
        .align = align,
        .backing = backing,
    };
    mye_pool_reset(pool);
    return true;
}

void mye_pool_deinit(mye_pool *pool)
{
    assert(pool != NULL);
    if (pool->blocks != NULL) {
        /* Reverse allocation order: live_bits was allocated after blocks,
         * and a LIFO backing (an arena) only reclaims the topmost block. */
        mye_free(pool->backing, pool->live_bits, (pool->capacity + 7) / 8);
        mye_free(pool->backing, pool->blocks, pool->stride * pool->capacity);
    }
    *pool = (mye_pool){ 0 };
}

mye_allocator mye_pool_allocator(mye_pool *pool)
{
    return (mye_allocator){ .vt = &pool_vtable, .ctx = pool };
}

void mye_pool_reset(mye_pool *pool)
{
    assert(pool != NULL);
    /* Thread every block onto the free list, in order, so the first
     * allocations come back in address order (friendlier to the cache). */
    pool->free_list = NULL;
    for (size_t i = pool->capacity; i > 0; --i) {
        uint8_t *block = pool->blocks + (i - 1) * pool->stride;
        memcpy(block, &pool->free_list, sizeof(void *));
        pool->free_list = block;
    }
    if (pool->capacity != 0) {
        memset(pool->live_bits, 0, (pool->capacity + 7) / 8);
    }
    pool->live = 0;
}

void *mye_pool_alloc(mye_pool *pool)
{
    assert(pool != NULL);
    if (pool->free_list == NULL) {
        return NULL;
    }
    void *block = pool->free_list;
    void *next = NULL;
    memcpy(&next, block, sizeof(void *));
    pool->free_list = next;
    size_t index = (size_t)((uint8_t *)block - pool->blocks) / pool->stride;
    /* Bounded, so a free list poisoned by a caller's use-after-free write
     * cannot make the allocator itself scribble outside live_bits. */
    if (index < pool->capacity) {
        pool->live_bits[index / 8] |= (uint8_t)(1u << (index % 8));
    }
    ++pool->live;
    if (pool->live > pool->peak) {
        pool->peak = pool->live;
    }
    return block;
}

void mye_pool_free(mye_pool *pool, void *ptr)
{
    assert(pool != NULL);
    if (ptr == NULL) {
        return;
    }
    /* A real check rather than an assert: threading a foreign pointer onto
     * the free list would corrupt the pool and blow up somewhere unrelated.
     * Refusing is recoverable, corruption is not, and the check must hold in
     * release builds where asserts are gone. */
    if (!mye_pool_owns(pool, ptr)) {
        return;
    }
    size_t index = (size_t)((uint8_t *)ptr - pool->blocks) / pool->stride;
    uint8_t mask = (uint8_t)(1u << (index % 8));
    if ((pool->live_bits[index / 8] & mask) == 0) {
        /* Double free (or free of a never-allocated slot): pushing it again
         * would thread a cycle into the free list and hand one block to two
         * callers. Refuse and count, so tests can fail on the count the way
         * they fail on tracking leaks. */
        ++pool->double_frees;
        return;
    }
    pool->live_bits[index / 8] &= (uint8_t)~mask;
    memcpy(ptr, &pool->free_list, sizeof(void *));
    pool->free_list = ptr;
    --pool->live;
}

size_t mye_pool_live(const mye_pool *pool) { return pool->live; }
size_t mye_pool_capacity(const mye_pool *pool) { return pool->capacity; }
size_t mye_pool_peak(const mye_pool *pool) { return pool->peak; }
size_t mye_pool_double_frees(const mye_pool *pool) { return pool->double_frees; }

bool mye_pool_owns(const mye_pool *pool, const void *ptr)
{
    if (pool->blocks == NULL || ptr == NULL) {
        return false;
    }
    /* Compare as integers: `ptr` may be a foreign pointer, and relational
     * pointer comparison is only defined within one object. */
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t begin = (uintptr_t)pool->blocks;
    uintptr_t end = begin + pool->stride * pool->capacity;
    if (p < begin || p >= end) {
        return false;
    }
    return (size_t)(p - begin) % pool->stride == 0;
}

/* ------------------------------------------------------------ tracking -- */

/* Raises peak_bytes to `live` if it is currently lower. The loop is needed
 * because another thread may raise it between our load and our store. */
static void track_peak(mye_tracking *t, size_t live)
{
    size_t peak = atomic_load_explicit(&t->peak_bytes, memory_order_relaxed);
    while (live > peak &&
           !atomic_compare_exchange_weak_explicit(&t->peak_bytes, &peak, live,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
        /* peak was reloaded by the exchange; retry. */
    }
}

static void *tracking_alloc(void *ctx, size_t size, size_t align)
{
    mye_tracking *t = (mye_tracking *)ctx;
    void *p = mye_alloc(t->backing, size, align);
    if (p == NULL) {
        atomic_fetch_add_explicit(&t->failed_allocs, 1, memory_order_relaxed);
        return NULL;
    }
    atomic_fetch_add_explicit(&t->total_allocs, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->live_count, 1, memory_order_relaxed);
    size_t live = atomic_fetch_add_explicit(&t->live_bytes, size,
                                            memory_order_relaxed) + size;
    track_peak(t, live);
    return p;
}

static void *tracking_resize(void *ctx, void *ptr, size_t old_size,
                             size_t new_size, size_t align)
{
    mye_tracking *t = (mye_tracking *)ctx;
    void *p = mye_resize(t->backing, ptr, old_size, new_size, align);
    if (p == NULL) {
        atomic_fetch_add_explicit(&t->failed_allocs, 1, memory_order_relaxed);
        return NULL;
    }
    /* new_size - old_size deliberately wraps when shrinking: adding the
     * wrapped value is exact modular subtraction. Correct ONLY while these
     * counters stay unsigned. */
    size_t live = atomic_fetch_add_explicit(&t->live_bytes,
                                            new_size - old_size,
                                            memory_order_relaxed) +
                  new_size - old_size;
    track_peak(t, live);
    return p;
}

static void tracking_release(void *ctx, void *ptr, size_t size)
{
    mye_tracking *t = (mye_tracking *)ctx;
    mye_free(t->backing, ptr, size);
    atomic_fetch_add_explicit(&t->total_frees, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&t->live_count, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&t->live_bytes, size, memory_order_relaxed);
}

static const mye_allocator_vtable tracking_vtable = {
    .alloc = tracking_alloc,
    .resize = tracking_resize,
    .release = tracking_release,
};

void mye_tracking_init(mye_tracking *t, mye_allocator backing)
{
    assert(t != NULL);
    t->backing = backing;
    atomic_init(&t->live_bytes, 0);
    atomic_init(&t->live_count, 0);
    atomic_init(&t->peak_bytes, 0);
    atomic_init(&t->total_allocs, 0);
    atomic_init(&t->total_frees, 0);
    atomic_init(&t->failed_allocs, 0);
}

mye_allocator mye_tracking_allocator(mye_tracking *t)
{
    return (mye_allocator){ .vt = &tracking_vtable, .ctx = t };
}

bool mye_tracking_has_leaks(const mye_tracking *t)
{
    return t->live_count != 0 || t->live_bytes != 0;
}

/* Note: reading an _Atomic lvalue performs an atomic load, so the plain
 * field accesses above and in the report below are race-free. */

void mye_tracking_report(const mye_tracking *t, const char *label)
{
    fprintf(stderr,
            "[mye] %s: live %zu allocs / %zu bytes | peak %zu | total %zu "
            "allocs, %zu frees, %zu failed\n",
            label != NULL ? label : "alloc", t->live_count, t->live_bytes,
            t->peak_bytes, t->total_allocs, t->total_frees, t->failed_allocs);
}
