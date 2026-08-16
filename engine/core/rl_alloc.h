/* raylib allocator shim. See plan/04-memory.md.
 *
 * This header is force-included into every raylib translation unit (via
 * -include in cmake/MyeDependencies.cmake), where RL_MALLOC and friends are
 * redefined to call these. That routes every raylib allocation -- textures,
 * meshes, decoded images, audio buffers -- through a mye_allocator, so the
 * tracking allocator's leak report covers raylib too.
 *
 * Deliberately minimal: it is compiled as part of raylib, so it must not drag
 * in engine headers or anything beyond <stddef.h>. The setter lives in
 * core/alloc.h, which only the engine includes.
 *
 * raylib frees without telling us the size, so these use the sized-header
 * adapters (mye_alloc_hdr family).
 */
#ifndef MYE_CORE_RL_ALLOC_H
#define MYE_CORE_RL_ALLOC_H

#include <stddef.h>

void *mye_rl_malloc(size_t size);
void *mye_rl_calloc(size_t count, size_t size);
void *mye_rl_realloc(void *ptr, size_t size);
void mye_rl_free(void *ptr);

/* The redirection itself lives here rather than in target_compile_definitions
 * because CMake does not reliably pass function-like macros through -D. Since
 * this header is force-included ahead of raylib.h, rlgl.h and raudio.c, whose
 * own definitions are all #ifndef-guarded, these win. */
/* Guarded so this header is also safe to include from ordinary engine or game
 * code, where raylib.h has usually been included first and already defined
 * them. In raylib's own translation units the force-include puts us first, so
 * these win and raylib's #ifndef-guarded defaults are skipped. */
#ifndef RL_MALLOC
#define RL_MALLOC(sz) mye_rl_malloc(sz)
#endif
#ifndef RL_CALLOC
#define RL_CALLOC(n, sz) mye_rl_calloc(n, sz)
#endif
#ifndef RL_REALLOC
#define RL_REALLOC(ptr, sz) mye_rl_realloc(ptr, sz)
#endif
#ifndef RL_FREE
#define RL_FREE(ptr) mye_rl_free(ptr)
#endif

#endif /* MYE_CORE_RL_ALLOC_H */
