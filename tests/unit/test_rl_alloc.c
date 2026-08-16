/* Proves raylib's own allocations really do route through a mye_allocator.
 *
 * Without the RL_MALLOC redirection in cmake/MyeDependencies.cmake these
 * tests fail: raylib would call plain malloc and the tracking allocator would
 * see nothing. See plan/04-memory.md. */
#include "core/alloc.h"
#include "core/rl_alloc.h"
#include "mye_test.h"

#include <raylib.h>

/* Everything here is CPU-side raylib (rtextures image functions), so no
 * window or GL context is needed. */

TEST(raylib_image_allocation_is_tracked)
{
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_rl_alloc_set(mye_tracking_allocator(&t));

    size_t before = t.live_bytes;

    Image image = GenImageColor(64, 48, RED);
    ASSERT_NOT_NULL(image.data);

    /* The pixel buffer alone is 64*48*4 bytes, and it came from raylib. */
    size_t pixels = (size_t)64 * 48 * 4;
    ASSERT_TRUE(t.live_bytes >= before + pixels);
    ASSERT_TRUE(t.total_allocs > 0);

    UnloadImage(image);
    ASSERT_EQ_U64(before, t.live_bytes); /* raylib gave it all back */

    mye_rl_alloc_reset();
    ASSERT_FALSE(mye_tracking_has_leaks(&t));
}

TEST(raylib_reallocation_paths_are_tracked)
{
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_rl_alloc_set(mye_tracking_allocator(&t));

    /* ImageResize and friends reallocate internally -- a mismatch between
     * our header-prefixed blocks and raylib's frees would corrupt the heap
     * here rather than merely miscount. */
    Image image = GenImageColor(32, 32, BLUE);
    ASSERT_NOT_NULL(image.data);
    ImageResize(&image, 128, 128);
    ASSERT_EQ_INT(128, image.width);
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
    ImageCrop(&image, (Rectangle){ 0, 0, 64, 64 });
    ASSERT_EQ_INT(64, image.width);

    UnloadImage(image);
    ASSERT_FALSE(mye_tracking_has_leaks(&t));

    mye_rl_alloc_reset();
}

TEST(shim_handles_edge_cases)
{
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_rl_alloc_set(mye_tracking_allocator(&t));

    ASSERT_NULL(mye_rl_malloc(0));
    ASSERT_NULL(mye_rl_calloc(0, 16));
    ASSERT_NULL(mye_rl_calloc(16, 0));
    /* count * size would overflow: refuse rather than under-allocate. */
    ASSERT_NULL(mye_rl_calloc(SIZE_MAX, 2));
    mye_rl_free(NULL); /* no-op */

    /* calloc zeroes. */
    unsigned char *zeroed = (unsigned char *)mye_rl_calloc(8, 4);
    ASSERT_NOT_NULL(zeroed);
    for (int i = 0; i < 32; ++i) {
        ASSERT_EQ_INT(0, zeroed[i]);
    }

    /* realloc grows while preserving contents. */
    zeroed[0] = 42;
    unsigned char *grown = (unsigned char *)mye_rl_realloc(zeroed, 256);
    ASSERT_NOT_NULL(grown);
    ASSERT_EQ_INT(42, grown[0]);
    mye_rl_free(grown);

    /* realloc(NULL, n) allocates, realloc(p, 0) frees. */
    void *fresh = mye_rl_realloc(NULL, 64);
    ASSERT_NOT_NULL(fresh);
    ASSERT_NULL(mye_rl_realloc(fresh, 0));

    ASSERT_FALSE(mye_tracking_has_leaks(&t));
    mye_rl_alloc_reset();
}

TEST(reset_returns_raylib_to_the_heap)
{
    mye_tracking t;
    mye_tracking_init(&t, mye_heap_allocator());
    mye_rl_alloc_set(mye_tracking_allocator(&t));
    mye_rl_alloc_reset();

    /* After reset nothing is attributed to the tracking allocator, and
     * raylib still works -- which is what makes shutdown ordering safe. */
    Image image = GenImageColor(16, 16, GREEN);
    ASSERT_NOT_NULL(image.data);
    ASSERT_EQ_U64(0, t.live_bytes);
    UnloadImage(image);

    ASSERT_FALSE(mye_tracking_has_leaks(&t));
}

TEST_MAIN(TEST_CASE(raylib_image_allocation_is_tracked),
          TEST_CASE(raylib_reallocation_paths_are_tracked),
          TEST_CASE(shim_handles_edge_cases),
          TEST_CASE(reset_returns_raylib_to_the_heap))
