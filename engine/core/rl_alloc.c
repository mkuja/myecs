#include "core/rl_alloc.h"

#include "core/alloc.h"

#include <string.h>

/* raylib's RL_MALLOC macros take no context, so the allocator they route to
 * must be reachable globally -- the same forced compromise as the flecs
 * bridge in engine/core/engine.c. Engine code still passes allocators
 * explicitly; this exists only because the library's hook shape demands it.
 *
 * Defaults to the heap so raylib allocations before mye_init (and after
 * mye_shutdown) still work. */
static mye_allocator g_rl_allocator;
static bool g_rl_allocator_set;

static mye_allocator rl_allocator(void)
{
    if (!g_rl_allocator_set) {
        return mye_heap_allocator();
    }
    return g_rl_allocator;
}

void mye_rl_alloc_set(mye_allocator allocator)
{
    if (mye_allocator_valid(allocator)) {
        g_rl_allocator = allocator;
        g_rl_allocator_set = true;
    } else {
        g_rl_allocator_set = false;
    }
}

void mye_rl_alloc_reset(void)
{
    g_rl_allocator_set = false;
}

void *mye_rl_malloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    return mye_alloc_hdr(rl_allocator(), size);
}

void *mye_rl_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0) {
        return NULL;
    }
    if (count > SIZE_MAX / size) {
        return NULL; /* overflow */
    }
    return mye_alloc_hdr_zeroed(rl_allocator(), count * size);
}

void *mye_rl_realloc(void *ptr, size_t size)
{
    return mye_resize_hdr(rl_allocator(), ptr, size);
}

void mye_rl_free(void *ptr)
{
    mye_free_hdr(rl_allocator(), ptr);
}
