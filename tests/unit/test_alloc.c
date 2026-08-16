/* Unit tests for the allocator interface and its backends.
 * See plan/04-memory.md and plan/09-testing.md. Headless, no raylib. */
#include "core/alloc.h"
#include "mye_test.h"

/* ----------------------------------------------------------- interface -- */

TEST(interface_rejects_bad_input)
{
    mye_allocator heap = mye_heap_allocator();
    mye_allocator invalid = { 0 };

    ASSERT_TRUE(mye_allocator_valid(heap));
    ASSERT_FALSE(mye_allocator_valid(invalid));

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

TEST_MAIN(TEST_CASE(interface_rejects_bad_input), TEST_CASE(align_up),
          TEST_CASE(heap_alloc_free), TEST_CASE(heap_resize_preserves_contents),
          TEST_CASE(arena_alignment_and_bump),
          TEST_CASE(arena_honours_alignment_from_a_misaligned_base),
          TEST_CASE(arena_from_buffer_does_not_free_borrowed_memory),
          TEST_CASE(arena_exhaustion_returns_null),
          TEST_CASE(arena_init_rejects_bad_input),
          TEST_CASE(arena_reset_reuses_memory), TEST_CASE(arena_mark_rewind),
          TEST_CASE(arena_resize_extends_last_allocation_in_place),
          TEST_CASE(arena_free_rolls_back_only_last_allocation),
          TEST_CASE(pool_alloc_free_reuse), TEST_CASE(pool_ownership_check),
          TEST_CASE(pool_init_rejects_bad_input),
          TEST_CASE(pool_as_generic_allocator),
          TEST_CASE(hdr_roundtrip_and_alignment),
          TEST_CASE(hdr_zeroed_and_resize),
          TEST_CASE(hdr_allocations_balance_in_tracking),
          TEST_CASE(tracking_counts_and_detects_leaks),
          TEST_CASE(tracking_follows_resize), TEST_CASE(tracking_wraps_an_arena))
