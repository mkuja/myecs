#include "core/thread.h"

#if !MYE_THREADS_C11 && !MYE_THREADS_NONE
#include <sched.h>
#include <unistd.h>
#endif

bool mye_thread_create(mye_thread *thread, mye_thread_fn fn, void *arg)
{
#if MYE_THREADS_NONE
    (void)thread; (void)fn; (void)arg;
    return false; /* callers fall back to doing the work inline */
#elif MYE_THREADS_C11
    return thrd_create(thread, fn, arg) == thrd_success;
#else
    return pthread_create(thread, NULL, fn, arg) == 0;
#endif
}

void mye_thread_join(mye_thread thread)
{
#if MYE_THREADS_NONE
    (void)thread;
    return;
#elif MYE_THREADS_C11
    thrd_join(thread, NULL);
#else
    pthread_join(thread, NULL);
#endif
}

void mye_thread_yield(void)
{
#if MYE_THREADS_NONE
    /* nothing to do */
    return;
#elif MYE_THREADS_C11
    thrd_yield();
#else
    sched_yield();
#endif
}

bool mye_mutex_init(mye_mutex *mutex)
{
#if MYE_THREADS_NONE
    (void)mutex;
    return true;
#elif MYE_THREADS_C11
    return mtx_init(mutex, mtx_plain) == thrd_success;
#else
    return pthread_mutex_init(mutex, NULL) == 0;
#endif
}

void mye_mutex_destroy(mye_mutex *mutex)
{
#if MYE_THREADS_NONE
    (void)mutex;
    return;
#elif MYE_THREADS_C11
    mtx_destroy(mutex);
#else
    pthread_mutex_destroy(mutex);
#endif
}

void mye_mutex_lock(mye_mutex *mutex)
{
#if MYE_THREADS_NONE
    (void)mutex;
    return;
#elif MYE_THREADS_C11
    mtx_lock(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

void mye_mutex_unlock(mye_mutex *mutex)
{
#if MYE_THREADS_NONE
    (void)mutex;
    return;
#elif MYE_THREADS_C11
    mtx_unlock(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

bool mye_cond_init(mye_cond *cond)
{
#if MYE_THREADS_NONE
    (void)cond;
    return true;
#elif MYE_THREADS_C11
    return cnd_init(cond) == thrd_success;
#else
    return pthread_cond_init(cond, NULL) == 0;
#endif
}

void mye_cond_destroy(mye_cond *cond)
{
#if MYE_THREADS_NONE
    (void)cond;
    return;
#elif MYE_THREADS_C11
    cnd_destroy(cond);
#else
    pthread_cond_destroy(cond);
#endif
}

void mye_cond_wait(mye_cond *cond, mye_mutex *mutex)
{
#if MYE_THREADS_NONE
    /* Single-threaded: waiting for another thread would deadlock, and there
     * is no other thread to wait for. */
    (void)cond; (void)mutex;
    return;
#elif MYE_THREADS_C11
    cnd_wait(cond, mutex);
#else
    pthread_cond_wait(cond, mutex);
#endif
}

void mye_cond_signal(mye_cond *cond)
{
#if MYE_THREADS_NONE
    (void)cond;
    return;
#elif MYE_THREADS_C11
    cnd_signal(cond);
#else
    pthread_cond_signal(cond);
#endif
}

void mye_cond_broadcast(mye_cond *cond)
{
#if MYE_THREADS_NONE
    (void)cond;
    return;
#elif MYE_THREADS_C11
    cnd_broadcast(cond);
#else
    pthread_cond_broadcast(cond);
#endif
}

int mye_cpu_count(void)
{
#if MYE_THREADS_NONE
    return 1;
#elif !MYE_THREADS_C11 && defined(_SC_NPROCESSORS_ONLN)
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) {
        return (int)online;
    }
#endif
    return 1;
}
