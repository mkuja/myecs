/* Unit tests for the allocator interface and its backends.
 * See plan/04-memory.md and plan/09-testing.md. Headless, no raylib. */
#include "core/alloc.h"
#include "core/thread.h"
#include "mye_test.h"

/* ----------------------------------------------------------- interface -- */

static void *dummy_alloc(void *ctx, size_t size, size_t align)
{
    (void)ctx;
    (void)size;
    (void)align;
    return NULL;
}

TEST(interface_rejects_bad_input)
{
    mye_allocator heap = mye_heap_allocator();
    mye_allocator invalid = { 0 };

    ASSERT_TRUE(mye_allocator_valid(heap));
    ASSERT_FALSE(mye_allocator_valid(invalid));

    /* A vtable missing release is invalid too: it could allocate memory
     * that nothing can ever give back. */
    const mye_allocator_vtable no_release = { .alloc = dummy_alloc,
                                              .resize = NULL,
                                              .release = NULL };
    mye_allocator half = { .vt = &no_release, .ctx = NULL };
    ASSERT_FALSE(mye_allocator_valid(half));
    ASSERT_NULL(mye_alloc(half, 16, 8));

    ASSERT_NULL(mye_alloc(heap, 0, 8));    /* zero size */
    ASSERT_NULL(mye_alloc(heap, 16, 0));   /* align not a power of two */
    ASSERT_NULL(mye_alloc(heap, 16, 3));
    ASSERT_NULL(mye_alloc(invalid, 16, 8));

    mye_free(heap, NULL, 0); /* freeing NULL is a no-op, not a crash */
}

TEST(align_up)
{
    ASSERT_EQ_U64(0, mye_align_up(0, 8));
    ASSERT_EQ_U64(8, mye_align_up(1, 8));
    ASSERT_EQ_U64(8, mye_align_up(8, 8));
    ASSERT_EQ_U64(16, mye_align_up(9, 8));
    ASSERT_EQ_U64(64, mye_align_up(33, 64));
    ASSERT_EQ_U64(5, mye_align_up(5, 1)); /* align 1 is the identity */
    /* Saturates instead of wrapping. */
    ASSERT_EQ_U64(SIZE_MAX, mye_align_up(SIZE_MAX, 16));
}

TEST(heap_alloc_free)
{
    mye_allocator heap = mye_heap_allocator();

    int *p = MYE_NEW(heap, int);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(0, *p); /* MYE_NEW zeroes */
    *p = 42;
    ASSERT_EQ_INT(42, *p);
    MYE_DELETE(heap, p);

    void *over = mye_alloc(heap, 100, 64);
    ASSERT_NOT_NULL(over);
    ASSERT_EQ_U64(0, (uintptr_t)over % 64);
    mye_free(heap, over, 100);
}

TEST(heap_resize_preserves_contents)
{
    mye_allocator heap = mye_heap_allocator();

    uint8_t *buf = MYE_NEW_ARRAY(heap, uint8_t, 16);
    ASSERT_NOT_NULL(buf);
    for (uint8_t i = 0; i < 16; ++i) {
        buf[i] = (uint8_t)(i + 1);
    }

    uint8_t *grown = (uint8_t *)mye_resize(heap, buf, 16, 64, 1);
    ASSERT_NOT_NULL(grown);
    for (uint8_t i = 0; i < 16; ++i) {
        ASSERT_EQ_INT(i + 1, grown[i]);
    }

    /* Resizing to zero frees and yields NULL. */
    ASSERT_NULL(mye_resize(heap, grown, 64, 0, 1));
}

TEST(alloc_zeroed_zeroes_every_byte)
{
    mye_allocator heap = mye_heap_allocator();

    uint8_t *p = (uint8_t *)mye_alloc_zeroed(heap, 200, 16);
    ASSERT_NOT_NULL(p);
    for (size_t i = 0; i < 200; ++i) {
        ASSERT_EQ_INT(0, p[i]);
    }
    mye_free(heap, p, 200);
}

TEST(resize_with_null_ptr_acts_like_alloc)
{
    /* Observed through tracking so the shortcut is visible: the NULL-ptr
     * path must land in alloc (counted in total_allocs) rather than reach
     * the backend's resize with a NULL source. */
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_allocator a = mye_tracking_allocator(&t);

    uint8_t *p = (uint8_t *)mye_resize(a, NULL, 0, 32, 8);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(0, (uintptr_t)p % 8);
    ASSERT_EQ_U64(1, t.total_allocs);
    ASSERT_EQ_U64(1, t.live_count);

    p[0] = 5;
    p[31] = 6;
    mye_free(a, p, 32);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
}

TEST(resize_rejects_zero_old_size_for_live_blocks)
{
    /* A live block cannot be zero-sized; treating old_size == 0 as
     * "allocate fresh" would silently abandon `ptr`. The call is refused
     * instead: NULL back, block untouched, no counter moves. */
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_allocator a = mye_tracking_allocator(&t);

    uint8_t *p = (uint8_t *)mye_alloc(a, 32, 8);
    ASSERT_NOT_NULL(p);
    p[0] = 9;

    ASSERT_NULL(mye_resize(a, p, 0, 64, 8));
    ASSERT_EQ_INT(9, p[0]);
    ASSERT_EQ_U64(1, t.total_allocs);
    ASSERT_EQ_U64(0, t.failed_allocs);

    mye_free(a, p, 32);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
}

TEST(allocation_overflow_requests_fail_cleanly)
{
    mye_allocator heap = mye_heap_allocator();

    /* Adding the header slot to these would wrap. */
    ASSERT_NULL(mye_alloc_hdr(heap, SIZE_MAX));
    ASSERT_NULL(mye_alloc_hdr(heap, SIZE_MAX - 1));

    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 64));
    ASSERT_NOT_NULL(mye_alloc(mye_arena_allocator(&arena), 8, 8));
    size_t used = mye_arena_used(&arena);
    ASSERT_NULL(mye_alloc(mye_arena_allocator(&arena), SIZE_MAX, 8));
    ASSERT_EQ_U64(used, mye_arena_used(&arena)); /* nothing consumed */
    mye_arena_deinit(&arena);

    /* stride * capacity would overflow: refused before any allocation. */
    mye_pool pool;
    ASSERT_FALSE(mye_pool_init(&pool, heap, 64, 8, SIZE_MAX / 8));
}

/* --------------------------------------------------------------- arena -- */

TEST(arena_alignment_and_bump)
{
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 1024));
    mye_allocator a = mye_arena_allocator(&arena);

    ASSERT_EQ_U64(0, mye_arena_used(&arena));
    ASSERT_EQ_U64(1024, mye_arena_capacity(&arena));

    void *p1 = mye_alloc(a, 3, 1);
    ASSERT_NOT_NULL(p1);
    void *p2 = mye_alloc(a, 8, 16);
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ_U64(0, (uintptr_t)p2 % 16); /* padded to the requested align */
    ASSERT_TRUE((uintptr_t)p2 > (uintptr_t)p1);
    ASSERT_TRUE(mye_arena_used(&arena) >= 11);

    mye_arena_deinit(&arena);
}

TEST(arena_honours_alignment_from_a_misaligned_base)
{
    /* Regression: the arena used to align the OFFSET rather than the absolute
     * address, so any request stricter than the base's own alignment came
     * back misaligned. A deliberately odd base makes that deterministic. */
    static uint8_t raw[512];
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init_from_buffer(&arena, raw + 1, sizeof raw - 1));
    mye_allocator a = mye_arena_allocator(&arena);

    for (size_t align = 1; align <= 64; align *= 2) {
        void *p = mye_alloc(a, 8, align);
        ASSERT_NOT_NULL(p);
        ASSERT_EQ_U64(0, (uintptr_t)p % align);
    }

    mye_arena_deinit(&arena);
}

TEST(arena_from_buffer_does_not_free_borrowed_memory)
{
    uint8_t raw[128];
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init_from_buffer(&arena, raw, sizeof raw));
    ASSERT_EQ_U64(128, mye_arena_capacity(&arena));

    void *p = mye_alloc(mye_arena_allocator(&arena), 64, 8);
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE((uint8_t *)p >= raw && (uint8_t *)p < raw + sizeof raw);

    /* Must not attempt to free stack memory -- ASan would abort if it did. */
    mye_arena_deinit(&arena);

    ASSERT_FALSE(mye_arena_init_from_buffer(&arena, NULL, 16));
    ASSERT_FALSE(mye_arena_init_from_buffer(&arena, raw, 0));
}

TEST(arena_exhaustion_returns_null)
{
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 64));
    mye_allocator a = mye_arena_allocator(&arena);

    ASSERT_NOT_NULL(mye_alloc(a, 64, 1)); /* exactly fits */
    ASSERT_NULL(mye_alloc(a, 1, 1));      /* one byte too far */

    /* An over-large request fails without corrupting the arena. */
    size_t used = mye_arena_used(&arena);
    ASSERT_NULL(mye_alloc(a, 1024, 1));
    ASSERT_EQ_U64(used, mye_arena_used(&arena));

    mye_arena_deinit(&arena);
}

TEST(arena_init_rejects_bad_input)
{
    mye_arena arena;
    ASSERT_FALSE(mye_arena_init(&arena, mye_heap_allocator(), 0));
    ASSERT_FALSE(mye_arena_init(&arena, (mye_allocator){ 0 }, 128));
}

TEST(arena_reset_reuses_memory)
{
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 256));
    mye_allocator a = mye_arena_allocator(&arena);

    void *first = mye_alloc(a, 128, 8);
    ASSERT_NOT_NULL(first);
    ASSERT_EQ_U64(128, mye_arena_used(&arena));

    mye_arena_reset(&arena);
    ASSERT_EQ_U64(0, mye_arena_used(&arena));
    /* High-water survives the reset -- that is the point of tracking it. */
    ASSERT_EQ_U64(128, mye_arena_high_water(&arena));

    void *again = mye_alloc(a, 128, 8);
    ASSERT_EQ_PTR(first, again); /* same memory handed out again */
    ASSERT_EQ_U64(128, mye_arena_high_water(&arena));

    mye_arena_deinit(&arena);
}

TEST(arena_mark_rewind)
{
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 256));
    mye_allocator a = mye_arena_allocator(&arena);

    ASSERT_NOT_NULL(mye_alloc(a, 32, 8));
    mye_arena_mark mark = mye_arena_take_mark(&arena);

    void *scratch = mye_alloc(a, 64, 8);
    ASSERT_NOT_NULL(scratch);
    ASSERT_TRUE(mye_arena_used(&arena) > mark);

    mye_arena_rewind(&arena, mark);
    ASSERT_EQ_U64(mark, mye_arena_used(&arena));
    ASSERT_EQ_PTR(scratch, mye_alloc(a, 64, 8)); /* scratch space recycled */

    mye_arena_deinit(&arena);
}

TEST(arena_resize_extends_last_allocation_in_place)
{
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 256));
    mye_allocator a = mye_arena_allocator(&arena);

    uint8_t *buf = (uint8_t *)mye_alloc(a, 16, 1);
    ASSERT_NOT_NULL(buf);
    buf[0] = 7;

    uint8_t *grown = (uint8_t *)mye_resize(a, buf, 16, 32, 1);
    ASSERT_EQ_PTR(buf, grown); /* extended in place, no copy */
    ASSERT_EQ_INT(7, grown[0]);
    ASSERT_EQ_U64(32, mye_arena_used(&arena));

    /* Not the most recent allocation any more: falls back to alloc + copy. */
    ASSERT_NOT_NULL(mye_alloc(a, 8, 1));
    uint8_t *moved = (uint8_t *)mye_resize(a, grown, 32, 48, 1);
    ASSERT_NOT_NULL(moved);
    ASSERT_TRUE(moved != grown);
    ASSERT_EQ_INT(7, moved[0]);

    mye_arena_deinit(&arena);
}

TEST(arena_free_rolls_back_only_last_allocation)
{
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 256));
    mye_allocator a = mye_arena_allocator(&arena);

    void *first = mye_alloc(a, 32, 8);
    void *second = mye_alloc(a, 32, 8);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    ASSERT_EQ_U64(64, mye_arena_used(&arena));

    mye_free(a, second, 32); /* most recent: rolls back */
    ASSERT_EQ_U64(32, mye_arena_used(&arena));

    mye_free(a, first, 32); /* also most recent now */
    ASSERT_EQ_U64(0, mye_arena_used(&arena));

    void *a1 = mye_alloc(a, 32, 8);
    void *a2 = mye_alloc(a, 32, 8);
    ASSERT_NOT_NULL(a2);
    mye_free(a, a1, 32); /* NOT the most recent: no-op, by design */
    ASSERT_EQ_U64(64, mye_arena_used(&arena));

    mye_arena_deinit(&arena);
}

TEST(resize_failure_leaves_original_intact)
{
    /* The documented contract: a failed resize returns NULL and the caller
     * still owns the original block, unmoved and untouched. An arena makes
     * the failure deterministic. */
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 64));
    mye_allocator a = mye_arena_allocator(&arena);

    uint8_t *p1 = (uint8_t *)mye_alloc(a, 16, 8);
    ASSERT_NOT_NULL(p1);
    memset(p1, 0xAB, 16);
    ASSERT_NOT_NULL(mye_alloc(a, 16, 8)); /* p1 is no longer the top block */

    size_t used = mye_arena_used(&arena);
    ASSERT_NULL(mye_resize(a, p1, 16, 48, 8)); /* needs 48 fresh bytes: full */
    ASSERT_EQ_U64(used, mye_arena_used(&arena));
    for (size_t i = 0; i < 16; ++i) {
        ASSERT_EQ_INT(0xAB, p1[i]);
    }

    mye_arena_deinit(&arena);
}

TEST(arena_rewind_ignores_stale_mark)
{
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 128));
    mye_allocator a = mye_arena_allocator(&arena);

    ASSERT_NOT_NULL(mye_alloc(a, 64, 8));
    mye_arena_mark mark = mye_arena_take_mark(&arena);

    mye_arena_reset(&arena);
    /* The mark now points past `used`; rewinding to it must not move the
     * bump pointer FORWARD over space that was already handed back. */
    mye_arena_rewind(&arena, mark);
    ASSERT_EQ_U64(0, mye_arena_used(&arena));
    ASSERT_EQ_U64(64, mye_arena_high_water(&arena)); /* peak still recorded */

    mye_arena_deinit(&arena);
}

TEST(arena_resize_shrinks_top_block_in_place)
{
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 128));
    mye_allocator a = mye_arena_allocator(&arena);

    uint8_t *p = (uint8_t *)mye_alloc(a, 96, 8);
    ASSERT_NOT_NULL(p);
    p[0] = 3;
    ASSERT_EQ_U64(96, mye_arena_used(&arena));

    uint8_t *shrunk = (uint8_t *)mye_resize(a, p, 96, 24, 8);
    ASSERT_EQ_PTR(p, shrunk); /* in place, no copy */
    ASSERT_EQ_INT(3, shrunk[0]);
    ASSERT_EQ_U64(24, mye_arena_used(&arena)); /* space handed back */
    ASSERT_EQ_U64(96, mye_arena_high_water(&arena)); /* peak sticks */

    mye_arena_deinit(&arena);
}

TEST(arena_resize_moves_when_alignment_tightens)
{
    /* A resize may ask for stricter alignment than the block was allocated
     * with; the in-place path must not hand back a misaligned pointer. */
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 256));
    mye_allocator a = mye_arena_allocator(&arena);

    ASSERT_NOT_NULL(mye_alloc(a, 8, 8));
    /* Top block at offset 8: 8-aligned but never 64-aligned. */
    uint8_t *top = (uint8_t *)mye_alloc(a, 8, 8);
    ASSERT_NOT_NULL(top);
    top[0] = 4;

    uint8_t *moved = (uint8_t *)mye_resize(a, top, 8, 16, 64);
    ASSERT_NOT_NULL(moved);
    ASSERT_EQ_U64(0, (uintptr_t)moved % 64);
    ASSERT_EQ_INT(4, moved[0]);
    ASSERT_TRUE(moved != top);

    /* Same alignment still extends in place. */
    ASSERT_EQ_PTR(moved, mye_resize(a, moved, 16, 32, 64));

    mye_arena_deinit(&arena);
}

/* ---------------------------------------------------------------- pool -- */

typedef struct pool_item {
    uint64_t id;
    double value;
} pool_item;

TEST(pool_alloc_free_reuse)
{
    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), sizeof(pool_item),
                              _Alignof(pool_item), 4));

    ASSERT_EQ_U64(4, mye_pool_capacity(&pool));
    ASSERT_EQ_U64(0, mye_pool_live(&pool));

    pool_item *items[4];
    for (size_t i = 0; i < 4; ++i) {
        items[i] = (pool_item *)mye_pool_alloc(&pool);
        ASSERT_NOT_NULL(items[i]);
        ASSERT_EQ_U64(0, (uintptr_t)items[i] % _Alignof(pool_item));
        items[i]->id = i;
    }
    ASSERT_EQ_U64(4, mye_pool_live(&pool));
    ASSERT_NULL(mye_pool_alloc(&pool)); /* exhausted */

    /* Values are undisturbed while live. */
    for (uint64_t i = 0; i < 4; ++i) {
        ASSERT_EQ_U64(i, items[i]->id);
    }

    mye_pool_free(&pool, items[1]);
    ASSERT_EQ_U64(3, mye_pool_live(&pool));

    pool_item *recycled = (pool_item *)mye_pool_alloc(&pool);
    ASSERT_EQ_PTR(items[1], recycled); /* freed block comes back */
    ASSERT_EQ_U64(4, mye_pool_live(&pool));
    ASSERT_EQ_U64(4, mye_pool_peak(&pool));

    mye_pool_reset(&pool);
    ASSERT_EQ_U64(0, mye_pool_live(&pool));
    ASSERT_NOT_NULL(mye_pool_alloc(&pool));

    mye_pool_deinit(&pool);
}

TEST(pool_ownership_check)
{
    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), sizeof(pool_item),
                              _Alignof(pool_item), 4));

    void *block = mye_pool_alloc(&pool);
    ASSERT_TRUE(mye_pool_owns(&pool, block));
    ASSERT_FALSE(mye_pool_owns(&pool, NULL));

    int stack_var = 0;
    ASSERT_FALSE(mye_pool_owns(&pool, &stack_var));
    /* Interior pointers are not block starts. */
    ASSERT_FALSE(mye_pool_owns(&pool, (uint8_t *)block + 1));

    /* Freeing a foreign pointer is refused rather than corrupting the free
     * list -- checked in release builds too, where the assert is gone. */
    size_t live_before = mye_pool_live(&pool);
    mye_pool_free(&pool, &stack_var);
    ASSERT_EQ_U64(live_before, mye_pool_live(&pool));
    /* The pool still hands out every remaining block afterwards. */
    for (size_t i = live_before; i < mye_pool_capacity(&pool); ++i) {
        ASSERT_NOT_NULL(mye_pool_alloc(&pool));
    }

    mye_pool_deinit(&pool);
}

TEST(pool_double_free_is_detected_and_refused)
{
    /* The second free of a live block is the classic silent pool corruption:
     * the liveness bit refuses it -- free list intact -- and counts it, so a
     * test can fail on the count the way it fails on tracking leaks. */
    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), sizeof(pool_item),
                              _Alignof(pool_item), 4));

    void *a = mye_pool_alloc(&pool);
    void *b = mye_pool_alloc(&pool);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ_U64(0, mye_pool_double_frees(&pool));

    mye_pool_free(&pool, a);
    mye_pool_free(&pool, a); /* double free: refused, counted */
    ASSERT_EQ_U64(1, mye_pool_double_frees(&pool));
    ASSERT_EQ_U64(1, mye_pool_live(&pool)); /* not driven negative */

    /* The free list is intact: every remaining slot comes out exactly once
     * and nothing is handed to two callers. */
    void *again[3];
    for (size_t i = 0; i < 3; ++i) {
        again[i] = mye_pool_alloc(&pool);
        ASSERT_NOT_NULL(again[i]);
        ASSERT_TRUE(again[i] != b);
        for (size_t j = 0; j < i; ++j) {
            ASSERT_TRUE(again[i] != again[j]);
        }
    }
    ASSERT_NULL(mye_pool_alloc(&pool)); /* all four are live: b + three */

    /* The counter is a lifetime statistic: reset does not clear it. */
    mye_pool_reset(&pool);
    ASSERT_EQ_U64(1, mye_pool_double_frees(&pool));

    mye_pool_deinit(&pool);
}

TEST(pool_init_rejects_bad_input)
{
    mye_pool pool;
    mye_allocator heap = mye_heap_allocator();
    ASSERT_FALSE(mye_pool_init(&pool, heap, 0, 8, 4));   /* zero elem size */
    ASSERT_FALSE(mye_pool_init(&pool, heap, 8, 8, 0));   /* zero capacity */
    ASSERT_FALSE(mye_pool_init(&pool, heap, 8, 3, 4));   /* align not pow2 */
    ASSERT_FALSE(mye_pool_init(&pool, (mye_allocator){ 0 }, 8, 8, 4));
    /* Tiny elements are widened to hold the free-list pointer. */
    ASSERT_TRUE(mye_pool_init(&pool, heap, 1, 1, 2));
    ASSERT_NOT_NULL(mye_pool_alloc(&pool));
    mye_pool_deinit(&pool);
}

TEST(pool_as_generic_allocator)
{
    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), sizeof(pool_item),
                              _Alignof(pool_item), 2));
    mye_allocator a = mye_pool_allocator(&pool);

    pool_item *p = MYE_NEW(a, pool_item);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(0, p->id); /* MYE_NEW zeroes through the pool too */
    ASSERT_EQ_U64(1, mye_pool_live(&pool));

    /* Requests a pool block cannot satisfy are refused, not silently wrong. */
    ASSERT_NULL(mye_alloc(a, sizeof(pool_item) * 4, _Alignof(pool_item)));
    ASSERT_NULL(mye_alloc(a, sizeof(pool_item), 256));

    MYE_DELETE(a, p);
    ASSERT_EQ_U64(0, mye_pool_live(&pool));

    mye_pool_deinit(&pool);
}

TEST(pool_blocks_do_not_overlap)
{
    /* Elements whose size is not a multiple of the effective alignment: fill
     * every byte of every block, then verify nothing bled into a neighbour
     * and no block was handed out twice. */
    enum { COUNT = 8, ELEM = 12 };
    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), ELEM, 4, COUNT));

    uint8_t *blocks[COUNT];
    for (size_t i = 0; i < COUNT; ++i) {
        blocks[i] = (uint8_t *)mye_pool_alloc(&pool);
        ASSERT_NOT_NULL(blocks[i]);
        ASSERT_EQ_U64(0, (uintptr_t)blocks[i] % 4);
        memset(blocks[i], (int)(i + 1), ELEM);
    }

    for (size_t i = 0; i < COUNT; ++i) {
        if (i + 1 < COUNT) {
            /* Fresh blocks come back in address order, a full element
             * apart or more. */
            ASSERT_TRUE(blocks[i + 1] - blocks[i] >= ELEM);
        }
        for (size_t k = 0; k < ELEM; ++k) {
            ASSERT_EQ_INT((int)(i + 1), blocks[i][k]);
        }
    }

    mye_pool_deinit(&pool);
}

TEST(pool_honours_strict_alignment)
{
    /* Alignment stricter than the free-list pointer's own: every block must
     * land on it, which forces the stride up to the alignment as well --
     * 12-byte elements at align 64 must sit exactly 64 apart. */
    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), 12, 64, 4));

    uint8_t *prev = NULL;
    for (size_t i = 0; i < 4; ++i) {
        uint8_t *p = (uint8_t *)mye_pool_alloc(&pool);
        ASSERT_NOT_NULL(p);
        ASSERT_EQ_U64(0, (uintptr_t)p % 64);
        if (prev != NULL) {
            ASSERT_EQ_U64(64, (size_t)(p - prev));
        }
        prev = p;
    }

    mye_pool_deinit(&pool);
}

TEST(resize_fallback_serves_backends_without_native_resize)
{
    /* The pool has no native resize (fixed blocks), so mye_resize takes the
     * generic alloc+copy+free fallback -- the one path where a bug would
     * free the original before copying it. */
    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), 32, 8, 4));
    mye_allocator a = mye_pool_allocator(&pool);

    uint8_t *p = (uint8_t *)mye_alloc(a, 16, 8);
    ASSERT_NOT_NULL(p);
    memset(p, 0xC3, 16);

    uint8_t *grown = (uint8_t *)mye_resize(a, p, 16, 24, 8);
    ASSERT_NOT_NULL(grown); /* still fits a 32-byte block */
    ASSERT_EQ_U64(1, mye_pool_live(&pool)); /* old block freed, new live */
    for (size_t i = 0; i < 16; ++i) {
        ASSERT_EQ_INT(0xC3, grown[i]);
    }

    /* Past the stride no block can serve it: NULL, original untouched. */
    ASSERT_NULL(mye_resize(a, grown, 24, 64, 8));
    ASSERT_EQ_U64(1, mye_pool_live(&pool));
    ASSERT_EQ_INT(0xC3, grown[0]);

    mye_free(a, grown, 24);
    ASSERT_EQ_U64(0, mye_pool_live(&pool));
    mye_pool_deinit(&pool);
}

TEST(new_returns_null_when_the_backend_is_exhausted)
{
    /* MYE_NEW zeroes through memset after allocating; on failure there must
     * be no allocation and no memset -- just NULL. */
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 16));
    mye_allocator a = mye_arena_allocator(&arena);
    ASSERT_NOT_NULL(mye_alloc(a, 16, 1));
    ASSERT_NULL(MYE_NEW(a, pool_item)); /* arena full */
    mye_arena_deinit(&arena);

    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), sizeof(pool_item),
                              _Alignof(pool_item), 1));
    ASSERT_NOT_NULL(mye_pool_alloc(&pool));
    ASSERT_NULL(MYE_NEW(mye_pool_allocator(&pool), pool_item)); /* exhausted */
    mye_pool_deinit(&pool);
}

TEST(pool_reset_returns_every_block)
{
    enum { CAP = 4 };
    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), 16, 8, CAP));

    /* Chew through the pool in a mixed pattern, then reset. */
    void *a = mye_pool_alloc(&pool);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(mye_pool_alloc(&pool));
    mye_pool_free(&pool, a);
    mye_pool_reset(&pool);
    ASSERT_EQ_U64(0, mye_pool_live(&pool));
    ASSERT_EQ_U64(2, mye_pool_peak(&pool)); /* peak survives the reset */

    /* Every slot is allocatable again, and they are all distinct. */
    void *blocks[CAP];
    for (size_t i = 0; i < CAP; ++i) {
        blocks[i] = mye_pool_alloc(&pool);
        ASSERT_NOT_NULL(blocks[i]);
        for (size_t j = 0; j < i; ++j) {
            ASSERT_TRUE(blocks[i] != blocks[j]);
        }
    }
    ASSERT_NULL(mye_pool_alloc(&pool)); /* and no phantom extras */

    mye_pool_deinit(&pool);
}

TEST(arena_and_pool_survive_use_after_deinit)
{
    /* Deinit zeroes the struct, so a stale handle degrades to NULL instead
     * of touching freed memory. */
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 64));
    mye_allocator aa = mye_arena_allocator(&arena);
    mye_arena_deinit(&arena);
    ASSERT_NULL(mye_alloc(aa, 8, 8));

    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), 8, 8, 2));
    mye_allocator pa = mye_pool_allocator(&pool);
    void *block = mye_pool_alloc(&pool);
    ASSERT_NOT_NULL(block);
    mye_pool_deinit(&pool);
    ASSERT_NULL(mye_pool_alloc(&pool));
    ASSERT_NULL(mye_alloc(pa, 8, 8));
    ASSERT_FALSE(mye_pool_owns(&pool, block));
    mye_pool_free(&pool, block); /* refused: no longer owned */
}

/* ------------------------------------------------- third-party adapters -- */

TEST(hdr_roundtrip_and_alignment)
{
    mye_allocator heap = mye_heap_allocator();

    ASSERT_NULL(mye_alloc_hdr(heap, 0));

    uint8_t *p = (uint8_t *)mye_alloc_hdr(heap, 100);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(0, (uintptr_t)p % MYE_DEFAULT_ALIGN);
    ASSERT_EQ_U64(100, mye_hdr_payload_size(p));

    /* The whole payload is writable -- i.e. the header really is outside it. */
    for (size_t i = 0; i < 100; ++i) {
        p[i] = (uint8_t)(i & 0xFF);
    }
    for (size_t i = 0; i < 100; ++i) {
        ASSERT_EQ_INT((int)(i & 0xFF), p[i]);
    }

    mye_free_hdr(heap, p);
    mye_free_hdr(heap, NULL); /* no-op, not a crash */
    ASSERT_EQ_U64(0, mye_hdr_payload_size(NULL));
}

TEST(hdr_zeroed_and_resize)
{
    mye_allocator heap = mye_heap_allocator();

    uint8_t *z = (uint8_t *)mye_alloc_hdr_zeroed(heap, 64);
    ASSERT_NOT_NULL(z);
    for (size_t i = 0; i < 64; ++i) {
        ASSERT_EQ_INT(0, z[i]);
    }
    z[0] = 9;
    z[63] = 7;

    uint8_t *grown = (uint8_t *)mye_resize_hdr(heap, z, 256);
    ASSERT_NOT_NULL(grown);
    ASSERT_EQ_U64(256, mye_hdr_payload_size(grown));
    ASSERT_EQ_INT(9, grown[0]);
    ASSERT_EQ_INT(7, grown[63]);

    uint8_t *shrunk = (uint8_t *)mye_resize_hdr(heap, grown, 32);
    ASSERT_NOT_NULL(shrunk);
    ASSERT_EQ_U64(32, mye_hdr_payload_size(shrunk));
    ASSERT_EQ_INT(9, shrunk[0]);

    /* NULL in behaves like alloc; zero size behaves like free. */
    uint8_t *fresh = (uint8_t *)mye_resize_hdr(heap, NULL, 16);
    ASSERT_NOT_NULL(fresh);
    ASSERT_EQ_U64(16, mye_hdr_payload_size(fresh));
    ASSERT_NULL(mye_resize_hdr(heap, fresh, 0));

    mye_free_hdr(heap, shrunk);
}

TEST(hdr_allocations_balance_in_tracking)
{
    /* This is what makes the flecs bridge trustworthy: the header bytes must
     * be accounted on the way out exactly as they were on the way in, or the
     * engine's leak report would drift on every world it creates. */
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_allocator a = mye_tracking_allocator(&t);

    void *blocks[8];
    for (size_t i = 0; i < 8; ++i) {
        blocks[i] = mye_alloc_hdr(a, (i + 1) * 17);
        ASSERT_NOT_NULL(blocks[i]);
    }
    ASSERT_EQ_U64(8, t.live_count);

    blocks[3] = mye_resize_hdr(a, blocks[3], 4096);
    ASSERT_NOT_NULL(blocks[3]);
    blocks[5] = mye_resize_hdr(a, blocks[5], 2);
    ASSERT_NOT_NULL(blocks[5]);

    for (size_t i = 0; i < 8; ++i) {
        mye_free_hdr(a, blocks[i]);
    }

    ASSERT_EQ_U64(0, t.live_count);
    ASSERT_EQ_U64(0, t.live_bytes);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
}

TEST(hdr_adapter_composes_with_an_arena)
{
    /* The malloc-shaped adapter is not heap-only: over an arena it keeps the
     * same LIFO rollback the arena gives everything else. */
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 256));
    mye_allocator a = mye_arena_allocator(&arena);

    uint8_t *p = (uint8_t *)mye_alloc_hdr(a, 40);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(0, (uintptr_t)p % MYE_DEFAULT_ALIGN);
    ASSERT_EQ_U64(40, mye_hdr_payload_size(p));
    memset(p, 0x5A, 40);

    /* Growing the topmost hdr block extends in place, contents intact. */
    uint8_t *grown = (uint8_t *)mye_resize_hdr(a, p, 80);
    ASSERT_EQ_PTR(p, grown);
    ASSERT_EQ_INT(0x5A, grown[39]);
    ASSERT_EQ_U64(80, mye_hdr_payload_size(grown));

    mye_free_hdr(a, grown); /* topmost block: arena rolls all the way back */
    ASSERT_EQ_U64(0, mye_arena_used(&arena));

    mye_arena_deinit(&arena);
}

TEST(hdr_adapter_over_a_pool)
{
    /* The adapter always requests MYE_DEFAULT_ALIGN, so a pool must be built
     * with at least that alignment to serve it -- worth knowing before
     * routing a C library at one. */
    mye_pool small;
    ASSERT_TRUE(mye_pool_init(&small, mye_heap_allocator(), 64, 8, 2));
    if (MYE_DEFAULT_ALIGN > 8) {
        ASSERT_NULL(mye_alloc_hdr(mye_pool_allocator(&small), 16));
    }
    mye_pool_deinit(&small);

    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_heap_allocator(), 64,
                              MYE_DEFAULT_ALIGN, 2));
    uint8_t *p = (uint8_t *)mye_alloc_hdr(mye_pool_allocator(&pool), 16);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(16, mye_hdr_payload_size(p));
    ASSERT_EQ_U64(1, mye_pool_live(&pool));
    mye_free_hdr(mye_pool_allocator(&pool), p);
    ASSERT_EQ_U64(0, mye_pool_live(&pool));
    mye_pool_deinit(&pool);
}

TEST(hdr_adapter_over_a_borrowed_misaligned_buffer)
{
    /* Payload alignment must survive an arena whose base is deliberately
     * odd; the rollback then returns to the block's aligned start. */
    static uint8_t raw[256];
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init_from_buffer(&arena, raw + 1, sizeof raw - 1));
    mye_allocator a = mye_arena_allocator(&arena);

    uint8_t *p = (uint8_t *)mye_alloc_hdr(a, 32);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(0, (uintptr_t)p % MYE_DEFAULT_ALIGN);
    memset(p, 0x77, 32);

    /* used = alignment padding + header + payload; the padding stays
     * consumed after the free, the block itself is rolled back. */
    size_t padding = mye_arena_used(&arena) - (32 + MYE_DEFAULT_ALIGN);
    mye_free_hdr(a, p);
    ASSERT_EQ_U64(padding, mye_arena_used(&arena));

    mye_arena_deinit(&arena);
}

/* ------------------------------------------------------------ tracking -- */

TEST(tracking_counts_and_detects_leaks)
{
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_allocator a = mye_tracking_allocator(&t);

    ASSERT_FALSE(mye_tracking_has_leaks(&t));

    void *p = mye_alloc(a, 100, 8);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(1, t.live_count);
    ASSERT_EQ_U64(100, t.live_bytes);
    ASSERT_EQ_U64(100, t.peak_bytes);
    ASSERT_TRUE(mye_tracking_has_leaks(&t)); /* live allocation == a "leak" */

    void *q = mye_alloc(a, 40, 8);
    ASSERT_NOT_NULL(q);
    ASSERT_EQ_U64(140, t.live_bytes);
    ASSERT_EQ_U64(140, t.peak_bytes);

    mye_free(a, q, 40);
    ASSERT_EQ_U64(100, t.live_bytes);
    ASSERT_EQ_U64(140, t.peak_bytes); /* peak is sticky */

    mye_free(a, p, 100);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
    ASSERT_EQ_U64(2, t.total_allocs);
    ASSERT_EQ_U64(2, t.total_frees);
    ASSERT_EQ_U64(0, t.failed_allocs);
}

TEST(tracking_follows_resize)
{
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_allocator a = mye_tracking_allocator(&t);

    void *p = mye_alloc(a, 32, 8);
    ASSERT_NOT_NULL(p);
    p = mye_resize(a, p, 32, 128, 8);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(128, t.live_bytes);
    ASSERT_EQ_U64(128, t.peak_bytes);

    p = mye_resize(a, p, 128, 16, 8);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(16, t.live_bytes);

    mye_free(a, p, 16);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
}

TEST(tracking_wraps_an_arena)
{
    /* Tracking composes with any backend, arenas included. */
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 256));

    mye_tracking t;
    mye_tracking_init(&t, mye_arena_allocator(&arena));
    mye_allocator a = mye_tracking_allocator(&t);

    void *p = mye_alloc(a, 64, 8);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U64(64, t.live_bytes);

    ASSERT_NULL(mye_alloc(a, 4096, 8)); /* exceeds the arena */
    ASSERT_EQ_U64(1, t.failed_allocs);
    ASSERT_EQ_U64(64, t.live_bytes);

    mye_free(a, p, 64);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));

    mye_arena_deinit(&arena);
}

TEST(tracking_counts_failed_resize)
{
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 128));

    mye_tracking t;
    mye_tracking_init(&t, mye_arena_allocator(&arena));
    mye_allocator a = mye_tracking_allocator(&t);

    void *p = mye_alloc(a, 32, 8);
    void *q = mye_alloc(a, 96, 8); /* the arena is now exactly full */
    ASSERT_NOT_NULL(p);
    ASSERT_NOT_NULL(q);

    /* p is not the top block and the arena has no room: the resize fails,
     * the failure is counted, and the byte count is unchanged. */
    ASSERT_NULL(mye_resize(a, p, 32, 64, 8));
    ASSERT_EQ_U64(1, t.failed_allocs);
    ASSERT_EQ_U64(128, t.live_bytes);

    mye_free(a, q, 96);
    mye_free(a, p, 32);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
    ASSERT_EQ_U64(2, t.total_allocs); /* the failed resize allocated nothing */

    mye_arena_deinit(&arena);
}

TEST(tracking_resize_is_not_a_second_alloc)
{
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_allocator a = mye_tracking_allocator(&t);

    /* Input-rejected calls never reach the backend: not a "failure". */
    ASSERT_NULL(mye_alloc(a, 0, 8));
    ASSERT_EQ_U64(0, t.failed_allocs);

    void *p = mye_alloc(a, 32, 8);
    ASSERT_NOT_NULL(p);
    p = mye_resize(a, p, 32, 128, 8);
    ASSERT_NOT_NULL(p);
    ASSERT_NULL(mye_resize(a, p, 128, 0, 8)); /* resize to zero = free */

    ASSERT_EQ_U64(1, t.total_allocs); /* the resize was not a second alloc */
    ASSERT_EQ_U64(1, t.total_frees);  /* and the zero-resize was the free */
    ASSERT_EQ_U64(0, t.live_count);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
}

TEST(array_macros_balance_in_tracking)
{
    /* The size-on-free design rests on NEW_ARRAY and DELETE_ARRAY computing
     * the same byte count. Tracking notices if they ever disagree. */
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_allocator a = mye_tracking_allocator(&t);

    pool_item *items = MYE_NEW_ARRAY(a, pool_item, 17);
    ASSERT_NOT_NULL(items);
    ASSERT_EQ_U64(17 * sizeof(pool_item), t.live_bytes);
    ASSERT_EQ_U64(0, items[16].id); /* zeroed to the last element */

    MYE_DELETE_ARRAY(a, items, 17);
    ASSERT_EQ_U64(0, t.live_bytes);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
}

TEST(array_macros_refuse_overflowing_counts)
{
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_allocator a = mye_tracking_allocator(&t);

    /* sizeof(pool_item) * this wraps: refused outright, not under-allocated,
     * and not counted as a backend failure (it never reaches one). */
    ASSERT_NULL(MYE_NEW_ARRAY(a, pool_item, SIZE_MAX / 8));
    /* The dangerous shape: a count whose product wraps to a SMALL size.
     * Unchecked, this would under-allocate 2 * sizeof(pool_item) bytes and
     * "succeed". */
    ASSERT_NULL(MYE_NEW_ARRAY(a, pool_item,
                              SIZE_MAX / sizeof(pool_item) + 2));
    ASSERT_EQ_U64(0, t.total_allocs);
    ASSERT_EQ_U64(0, t.failed_allocs);

    /* A wrapped count on delete is a no-op rather than a wrong-sized free. */
    pool_item *items = MYE_NEW_ARRAY(a, pool_item, 4);
    ASSERT_NOT_NULL(items);
    MYE_DELETE_ARRAY(a, items, SIZE_MAX / 8);
    ASSERT_EQ_U64(1, t.live_count); /* refused: still live */
    /* elem_size 0 is refused symmetrically: a size-0 release would desync
     * tracking's byte count. */
    mye_free_array(a, items, 0, 4);
    ASSERT_EQ_U64(1, t.live_count);
    MYE_DELETE_ARRAY(a, items, 4);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
}

TEST(pool_over_an_arena_returns_all_memory_on_deinit)
{
    /* The pool makes TWO backing allocations (blocks, then live_bits); over
     * a LIFO backing they must be freed in reverse order or the blocks free
     * is a no-op and the storage is stranded until the arena resets. */
    mye_arena arena;
    ASSERT_TRUE(mye_arena_init(&arena, mye_heap_allocator(), 4096));

    mye_pool pool;
    ASSERT_TRUE(mye_pool_init(&pool, mye_arena_allocator(&arena), 16, 8, 32));
    ASSERT_TRUE(mye_arena_used(&arena) > 0);

    void *p = mye_pool_alloc(&pool);
    ASSERT_NOT_NULL(p);
    mye_pool_free(&pool, p);

    mye_pool_deinit(&pool);
    ASSERT_EQ_U64(0, mye_arena_used(&arena)); /* everything came back */

    mye_arena_deinit(&arena);
}

/* --------------------------------------------------- tracking, threaded -- */

/* 79 = 1 + 6 * 13: the largest base size the worker loop below produces. */
enum { THREAD_ALLOC_RESIZE_BUMP = 32,
       THREAD_ALLOC_MAX_LIVE = 79 + THREAD_ALLOC_RESIZE_BUMP };

typedef struct thread_alloc_ctx {
    mye_allocator a;
    int iterations;
    _Atomic int failures;
} thread_alloc_ctx;

static MYE_THREAD_RETURN thread_alloc_main(void *arg)
{
    thread_alloc_ctx *ctx = (thread_alloc_ctx *)arg;
    for (int i = 0; i < ctx->iterations; ++i) {
        size_t size = 1 + ((size_t)i % 7) * 13;
        uint8_t *p = (uint8_t *)mye_alloc(ctx->a, size, 8);
        if (p == NULL) {
            atomic_fetch_add(&ctx->failures, 1);
            continue;
        }
        p[0] = (uint8_t)i;
        p[size - 1] = (uint8_t)i;
        if (i % 8 == 0) {
            uint8_t *grown = (uint8_t *)mye_resize(
                ctx->a, p, size, size + THREAD_ALLOC_RESIZE_BUMP, 8);
            if (grown == NULL) {
                atomic_fetch_add(&ctx->failures, 1);
                mye_free(ctx->a, p, size);
                continue;
            }
            p = grown;
            size += THREAD_ALLOC_RESIZE_BUMP;
        }
        mye_free(ctx->a, p, size);
    }
    return MYE_THREAD_RESULT;
}

TEST(tracking_counters_balance_across_threads)
{
    /* The tracking counters are the engine's one atomics hot spot (see
     * plan/05-concurrency.md): several threads allocate through the same
     * tracking allocator in real use, so this drives exactly that from
     * several threads at once. TSan runs it too -- it is precisely the code
     * the TSan configuration exists for. */
    enum { THREADS = 4, ITERATIONS = 4000 };

    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());

    thread_alloc_ctx ctx = {
        .a = mye_tracking_allocator(&t),
        .iterations = ITERATIONS,
    };
    atomic_init(&ctx.failures, 0);

    mye_thread threads[THREADS];
    int started = 0;
    for (int i = 0; i < THREADS; ++i) {
        if (!mye_thread_create(&threads[i], thread_alloc_main, &ctx)) {
            break;
        }
        ++started;
    }
    if (started == 0) {
        SKIP("could not start any worker thread");
    }
    for (int i = 0; i < started; ++i) {
        mye_thread_join(threads[i]);
    }

    ASSERT_EQ_INT(0, ctx.failures);
    uint64_t total = (uint64_t)started * ITERATIONS;
    ASSERT_EQ_U64(total, t.total_allocs);
    ASSERT_EQ_U64(total, t.total_frees);
    ASSERT_EQ_U64(0, t.live_count);
    ASSERT_EQ_U64(0, t.live_bytes);
    ASSERT_EQ_U64(0, t.failed_allocs);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
    /* Every thread passes i == 48 (i % 8 == 0 and i % 7 == 6), where it
     * holds THREAD_ALLOC_MAX_LIVE bytes and its own post-add value reaches
     * track_peak -- a correct monotone max cannot end below that. Nor can
     * peak exceed every thread holding its largest block at once. */
    ASSERT_TRUE(t.peak_bytes >= THREAD_ALLOC_MAX_LIVE);
    ASSERT_TRUE(t.peak_bytes <= (size_t)started * THREAD_ALLOC_MAX_LIVE);
}

TEST_MAIN(TEST_CASE(interface_rejects_bad_input), TEST_CASE(align_up),
          TEST_CASE(heap_alloc_free), TEST_CASE(heap_resize_preserves_contents),
          TEST_CASE(alloc_zeroed_zeroes_every_byte),
          TEST_CASE(resize_with_null_ptr_acts_like_alloc),
          TEST_CASE(resize_rejects_zero_old_size_for_live_blocks),
          TEST_CASE(allocation_overflow_requests_fail_cleanly),
          TEST_CASE(arena_alignment_and_bump),
          TEST_CASE(arena_honours_alignment_from_a_misaligned_base),
          TEST_CASE(arena_from_buffer_does_not_free_borrowed_memory),
          TEST_CASE(arena_exhaustion_returns_null),
          TEST_CASE(arena_init_rejects_bad_input),
          TEST_CASE(arena_reset_reuses_memory), TEST_CASE(arena_mark_rewind),
          TEST_CASE(arena_resize_extends_last_allocation_in_place),
          TEST_CASE(arena_free_rolls_back_only_last_allocation),
          TEST_CASE(resize_failure_leaves_original_intact),
          TEST_CASE(arena_rewind_ignores_stale_mark),
          TEST_CASE(arena_resize_shrinks_top_block_in_place),
          TEST_CASE(arena_resize_moves_when_alignment_tightens),
          TEST_CASE(pool_alloc_free_reuse), TEST_CASE(pool_ownership_check),
          TEST_CASE(pool_double_free_is_detected_and_refused),
          TEST_CASE(pool_init_rejects_bad_input),
          TEST_CASE(pool_as_generic_allocator),
          TEST_CASE(pool_blocks_do_not_overlap),
          TEST_CASE(pool_honours_strict_alignment),
          TEST_CASE(resize_fallback_serves_backends_without_native_resize),
          TEST_CASE(new_returns_null_when_the_backend_is_exhausted),
          TEST_CASE(pool_reset_returns_every_block),
          TEST_CASE(pool_over_an_arena_returns_all_memory_on_deinit),
          TEST_CASE(arena_and_pool_survive_use_after_deinit),
          TEST_CASE(hdr_roundtrip_and_alignment),
          TEST_CASE(hdr_zeroed_and_resize),
          TEST_CASE(hdr_allocations_balance_in_tracking),
          TEST_CASE(hdr_adapter_composes_with_an_arena),
          TEST_CASE(hdr_adapter_over_a_pool),
          TEST_CASE(hdr_adapter_over_a_borrowed_misaligned_buffer),
          TEST_CASE(tracking_counts_and_detects_leaks),
          TEST_CASE(tracking_follows_resize), TEST_CASE(tracking_wraps_an_arena),
          TEST_CASE(tracking_counts_failed_resize),
          TEST_CASE(tracking_resize_is_not_a_second_alloc),
          TEST_CASE(array_macros_balance_in_tracking),
          TEST_CASE(array_macros_refuse_overflowing_counts),
          TEST_CASE(tracking_counters_balance_across_threads))
