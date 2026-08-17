/* Thin threading shim. See plan/05-concurrency.md.
 *
 * POSIX pthreads by default, C11 <threads.h> elsewhere (Windows/MSVC).
 *
 * Why not C11 threads everywhere, given this is a strict-C project?
 * ThreadSanitizer cannot instrument glibc's C11 threads shim -- a two-line
 * thrd_create program dies with DEADLYSIGNAL under both gcc and clang on
 * this toolchain, while the identical pthread program runs clean. Since the
 * job pool and the lock-free channel are precisely the code that needs TSan,
 * pthreads wins on the platform we develop on. pthreads is a C API; nothing
 * here compromises the no-C++ rule.
 */
#ifndef MYE_CORE_THREAD_H
#define MYE_CORE_THREAD_H

#include <stdbool.h>

/* Three backends. Emscripten without -pthread gets a no-op one: browser
 * threads need SharedArrayBuffer and cross-origin isolation, which is a
 * deployment constraint rather than a build flag, so the first web build
 * ships single-threaded.
 *
 * Nothing else has to know: mye_jobs_create fails, and a caller that wanted
 * a pool runs its work inline instead. Asset loading is synchronous on every
 * target, so it never had a thread to lose. */
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define MYE_THREADS_NONE 1
#define MYE_THREADS_C11 0
#elif defined(_WIN32)
#define MYE_THREADS_NONE 0
#define MYE_THREADS_C11 1
#else
#define MYE_THREADS_NONE 0
#define MYE_THREADS_C11 0
#endif

#if MYE_THREADS_NONE

/* Placeholders: nothing is ever created, so nothing is ever locked. */
typedef int mye_thread;
typedef int mye_mutex;
typedef int mye_cond;

#define MYE_THREAD_RETURN void *
#define MYE_THREAD_RESULT NULL

#elif MYE_THREADS_C11
#include <threads.h>

typedef thrd_t mye_thread;
typedef mtx_t mye_mutex;
typedef cnd_t mye_cond;

#define MYE_THREAD_RETURN int
#define MYE_THREAD_RESULT 0

#else
#include <pthread.h>

typedef pthread_t mye_thread;
typedef pthread_mutex_t mye_mutex;
typedef pthread_cond_t mye_cond;

#define MYE_THREAD_RETURN void *
#define MYE_THREAD_RESULT NULL

#endif

typedef MYE_THREAD_RETURN (*mye_thread_fn)(void *arg);

bool mye_thread_create(mye_thread *thread, mye_thread_fn fn, void *arg);
void mye_thread_join(mye_thread thread);
void mye_thread_yield(void);

bool mye_mutex_init(mye_mutex *mutex);
void mye_mutex_destroy(mye_mutex *mutex);
void mye_mutex_lock(mye_mutex *mutex);
void mye_mutex_unlock(mye_mutex *mutex);

bool mye_cond_init(mye_cond *cond);
void mye_cond_destroy(mye_cond *cond);
void mye_cond_wait(mye_cond *cond, mye_mutex *mutex);
void mye_cond_signal(mye_cond *cond);
void mye_cond_broadcast(mye_cond *cond);

/* Logical CPU count, 1 if it cannot be determined. */
int mye_cpu_count(void);

#endif /* MYE_CORE_THREAD_H */
