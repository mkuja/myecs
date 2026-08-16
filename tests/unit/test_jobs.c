/* Unit tests for the background worker pool. See plan/05-concurrency.md. */
#include "core/jobs.h"
#include "mye_test.h"

#include <stdatomic.h>
#include "core/thread.h"

static atomic_int g_ran;
static atomic_int g_sum;

static void increment_job(void *arg)
{
    (void)arg;
    atomic_fetch_add(&g_ran, 1);
}

static void add_job(void *arg)
{
    int value = (int)(intptr_t)arg;
    atomic_fetch_add(&g_sum, value);
    atomic_fetch_add(&g_ran, 1);
}

TEST(create_rejects_bad_arguments)
{
    ASSERT_NULL(mye_jobs_create((mye_allocator){ 0 }, 2, 16));
    ASSERT_NULL(mye_jobs_create(mye_heap_allocator(), 2, 5)); /* not pow2 */
    mye_jobs_destroy(NULL);                                   /* no-op */
}

TEST(jobs_run_on_worker_threads)
{
    atomic_init(&g_ran, 0);

    mye_jobs *jobs = mye_jobs_create(mye_heap_allocator(), 4, 64);
    ASSERT_NOT_NULL(jobs);
    ASSERT_EQ_INT(4, mye_jobs_worker_count(jobs));

    for (int i = 0; i < 32; ++i) {
        ASSERT_TRUE(mye_jobs_submit(jobs, increment_job, NULL));
    }

    mye_jobs_wait_idle(jobs);
    ASSERT_EQ_INT(32, atomic_load(&g_ran));
    ASSERT_EQ_U64(0, mye_jobs_pending(jobs));

    mye_jobs_destroy(jobs);
}

TEST(job_arguments_arrive_intact)
{
    atomic_init(&g_ran, 0);
    atomic_init(&g_sum, 0);

    mye_jobs *jobs = mye_jobs_create(mye_heap_allocator(), 2, 64);
    ASSERT_NOT_NULL(jobs);

    int expected = 0;
    for (int i = 1; i <= 50; ++i) {
        expected += i;
        ASSERT_TRUE(mye_jobs_submit(jobs, add_job, (void *)(intptr_t)i));
    }

    mye_jobs_wait_idle(jobs);
    ASSERT_EQ_INT(50, atomic_load(&g_ran));
    ASSERT_EQ_INT(expected, atomic_load(&g_sum)); /* nothing lost or doubled */

    mye_jobs_destroy(jobs);
}

TEST(submit_refuses_null_function)
{
    mye_jobs *jobs = mye_jobs_create(mye_heap_allocator(), 1, 8);
    ASSERT_NOT_NULL(jobs);

    ASSERT_FALSE(mye_jobs_submit(jobs, NULL, NULL));
    ASSERT_FALSE(mye_jobs_submit(NULL, increment_job, NULL));
    ASSERT_EQ_U64(0, mye_jobs_pending(jobs));

    mye_jobs_destroy(jobs);
}

TEST(destroy_waits_for_queued_work)
{
    /* Destroying the pool must not abandon queued jobs -- a half-run batch
     * would leak whatever those jobs allocated. */
    atomic_init(&g_ran, 0);

    mye_jobs *jobs = mye_jobs_create(mye_heap_allocator(), 2, 256);
    ASSERT_NOT_NULL(jobs);

    const int submitted = 200;
    for (int i = 0; i < submitted; ++i) {
        ASSERT_TRUE(mye_jobs_submit(jobs, increment_job, NULL));
    }

    mye_jobs_destroy(jobs); /* joins workers after draining */
    ASSERT_EQ_INT(submitted, atomic_load(&g_ran));
}

TEST(pool_survives_repeated_create_destroy)
{
    /* Catches leaked threads, mutexes, or condition variables. */
    for (int round = 0; round < 5; ++round) {
        atomic_init(&g_ran, 0);

        mye_jobs *jobs = mye_jobs_create(mye_heap_allocator(), 3, 32);
        ASSERT_NOT_NULL(jobs);

        for (int i = 0; i < 20; ++i) {
            ASSERT_TRUE(mye_jobs_submit(jobs, increment_job, NULL));
        }

        mye_jobs_wait_idle(jobs);
        ASSERT_EQ_INT(20, atomic_load(&g_ran));
        mye_jobs_destroy(jobs);
    }
}

TEST(full_queue_reports_failure_without_losing_count)
{
    /* A tiny queue and a single worker: submissions will overflow. The pool
     * must refuse them cleanly and keep `pending` truthful. */
    atomic_init(&g_ran, 0);

    mye_jobs *jobs = mye_jobs_create(mye_heap_allocator(), 1, 2);
    ASSERT_NOT_NULL(jobs);

    int accepted = 0;
    for (int i = 0; i < 500; ++i) {
        if (mye_jobs_submit(jobs, increment_job, NULL)) {
            ++accepted;
        }
    }

    mye_jobs_wait_idle(jobs);
    /* Exactly the accepted jobs ran -- rejections cost nothing. */
    ASSERT_EQ_INT(accepted, atomic_load(&g_ran));
    ASSERT_EQ_U64(0, mye_jobs_pending(jobs));

    mye_jobs_destroy(jobs);
}

TEST_MAIN(TEST_CASE(create_rejects_bad_arguments),
          TEST_CASE(jobs_run_on_worker_threads),
          TEST_CASE(job_arguments_arrive_intact),
          TEST_CASE(submit_refuses_null_function),
          TEST_CASE(destroy_waits_for_queued_work),
          TEST_CASE(pool_survives_repeated_create_destroy),
          TEST_CASE(full_queue_reports_failure_without_losing_count))
