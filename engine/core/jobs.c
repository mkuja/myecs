#include "core/jobs.h"

#include "core/channel.h"
#include "core/thread.h"

#include <stdatomic.h>

#define MYE_MAX_WORKERS 32

typedef struct job_item {
    mye_job_fn fn;
    void *arg;
} job_item;

struct mye_jobs {
    mye_allocator allocator;
    mye_channel *queue;

    mye_thread workers[MYE_MAX_WORKERS];
    int worker_count;

    /* Workers sleep on this rather than spinning: a spinning pool would burn
     * a core per worker and starve the render thread. */
    mye_mutex mutex;
    mye_cond work_available;
    mye_cond all_idle;

    atomic_size_t pending; /* submitted but not yet finished */
    atomic_bool stopping;
};

static MYE_THREAD_RETURN worker_main(void *arg)
{
    mye_jobs *jobs = (mye_jobs *)arg;

    for (;;) {
        job_item job;
        bool got_job = mye_channel_recv(jobs->queue, &job);

        if (!got_job) {
            mye_mutex_lock(&jobs->mutex);
            /* Re-check under the lock: a job may have arrived between the
             * failed recv and taking the mutex, and missing that wakeup
             * would leave the job sitting in the queue forever. */
            if (atomic_load(&jobs->stopping)) {
                mye_mutex_unlock(&jobs->mutex);
                break;
            }
            if (mye_channel_count(jobs->queue) == 0) {
                mye_cond_wait(&jobs->work_available, &jobs->mutex);
            }
            mye_mutex_unlock(&jobs->mutex);
            continue;
        }

        if (job.fn != NULL) {
            job.fn(job.arg);
        }

        /* Announce idleness only after the count actually reaches zero. */
        if (atomic_fetch_sub(&jobs->pending, 1) == 1) {
            mye_mutex_lock(&jobs->mutex);
            mye_cond_broadcast(&jobs->all_idle);
            mye_mutex_unlock(&jobs->mutex);
        }
    }

    return MYE_THREAD_RESULT;
}

static int default_worker_count(void)
{
    /* Leave a core for the main thread, which owns rendering. */
    int count = mye_cpu_count() - 1;
    if (count < 1) count = 1;
    if (count > MYE_MAX_WORKERS) count = MYE_MAX_WORKERS;
    return count;
}

mye_jobs *mye_jobs_create(mye_allocator allocator, int worker_count,
                          size_t queue_capacity)
{
    if (!mye_allocator_valid(allocator)) {
        return NULL;
    }
    if (worker_count <= 0) {
        worker_count = default_worker_count();
    }
    if (worker_count > MYE_MAX_WORKERS) {
        worker_count = MYE_MAX_WORKERS;
    }

    mye_jobs *jobs = MYE_NEW(allocator, mye_jobs);
    if (jobs == NULL) {
        return NULL;
    }
    jobs->allocator = allocator;

    jobs->queue = mye_channel_create(allocator, queue_capacity,
                                     sizeof(job_item));
    if (jobs->queue == NULL) {
        MYE_DELETE(allocator, jobs);
        return NULL;
    }

    if (!mye_mutex_init(&jobs->mutex) ||
        !mye_cond_init(&jobs->work_available) ||
        !mye_cond_init(&jobs->all_idle)) {
        mye_channel_destroy(jobs->queue);
        MYE_DELETE(allocator, jobs);
        return NULL;
    }

    atomic_init(&jobs->pending, 0);
    atomic_init(&jobs->stopping, false);

    for (int i = 0; i < worker_count; ++i) {
        if (!mye_thread_create(&jobs->workers[i], worker_main, jobs)) {
            /* Run with however many started, rather than failing outright. */
            break;
        }
        jobs->worker_count = i + 1;
    }

    if (jobs->worker_count == 0) {
        mye_jobs_destroy(jobs);
        return NULL;
    }

    return jobs;
}

void mye_jobs_destroy(mye_jobs *jobs)
{
    if (jobs == NULL) {
        return;
    }

    /* Let queued work finish before pulling the rug out: a half-run job would
     * leak whatever it allocated. */
    mye_jobs_wait_idle(jobs);

    mye_mutex_lock(&jobs->mutex);
    atomic_store(&jobs->stopping, true);
    mye_cond_broadcast(&jobs->work_available);
    mye_mutex_unlock(&jobs->mutex);

    for (int i = 0; i < jobs->worker_count; ++i) {
        mye_thread_join(jobs->workers[i]);
    }

    mye_cond_destroy(&jobs->all_idle);
    mye_cond_destroy(&jobs->work_available);
    mye_mutex_destroy(&jobs->mutex);

    mye_allocator a = jobs->allocator;
    mye_channel_destroy(jobs->queue);
    MYE_DELETE(a, jobs);
}

bool mye_jobs_submit(mye_jobs *jobs, mye_job_fn fn, void *arg)
{
    if (jobs == NULL || fn == NULL || atomic_load(&jobs->stopping)) {
        return false;
    }

    /* Count before queueing: a worker could otherwise finish the job before
     * the counter went up and drive `pending` negative. */
    atomic_fetch_add(&jobs->pending, 1);

    job_item job = { .fn = fn, .arg = arg };
    if (!mye_channel_send(jobs->queue, &job)) {
        atomic_fetch_sub(&jobs->pending, 1);
        return false; /* queue full */
    }

    mye_mutex_lock(&jobs->mutex);
    mye_cond_signal(&jobs->work_available);
    mye_mutex_unlock(&jobs->mutex);
    return true;
}

int mye_jobs_worker_count(const mye_jobs *jobs)
{
    return jobs != NULL ? jobs->worker_count : 0;
}

/* Not const-qualified: C11 atomic loads take a non-const pointer, and
 * laundering the constness away would be worse than being honest here. */
size_t mye_jobs_pending(mye_jobs *jobs)
{
    return jobs != NULL ? atomic_load(&jobs->pending) : 0;
}

void mye_jobs_wait_idle(mye_jobs *jobs)
{
    if (jobs == NULL) {
        return;
    }

    mye_mutex_lock(&jobs->mutex);
    while (atomic_load(&jobs->pending) > 0) {
        /* Nudge workers in case one is asleep with work still queued. */
        mye_cond_broadcast(&jobs->work_available);
        mye_cond_wait(&jobs->all_idle, &jobs->mutex);
    }
    mye_mutex_unlock(&jobs->mutex);
}
