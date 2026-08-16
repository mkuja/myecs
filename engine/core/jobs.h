/* Background worker pool. See plan/05-concurrency.md.
 *
 * This is NOT the engine's parallelism story for systems -- flecs' worker
 * pipeline handles that. This pool exists for work that must happen *off*
 * the frame: reading files, decoding images, and anything else slow that
 * does not touch the ECS world.
 *
 * The contract for a job, and the reason it is short:
 *   - it owns its inputs and its output; nothing is shared mutably
 *   - it NEVER touches the ecs_world_t (flecs worlds are single-threaded
 *     outside ecs_progress)
 *   - it NEVER calls raylib draw, window, or GPU-upload functions, which are
 *     main-thread only
 *   - it reports results by sending a message on a mye_channel
 */
#ifndef MYE_CORE_JOBS_H
#define MYE_CORE_JOBS_H

#include "core/alloc.h"

typedef struct mye_jobs mye_jobs;
typedef void (*mye_job_fn)(void *arg);

/* `worker_count` <= 0 picks a sensible default from the CPU count.
 * `queue_capacity` must be a power of two. NULL on failure. */
mye_jobs *mye_jobs_create(mye_allocator allocator, int worker_count,
                          size_t queue_capacity);

/* Waits for running jobs to finish, joins every worker, then frees. */
void mye_jobs_destroy(mye_jobs *jobs);

/* Queues a job. false means the queue is full -- the caller decides whether
 * to retry, drop the work, or do it inline. Never blocks. */
bool mye_jobs_submit(mye_jobs *jobs, mye_job_fn fn, void *arg);

int mye_jobs_worker_count(const mye_jobs *jobs);
/* Jobs queued but not yet finished. Zero means the pool is idle. */
size_t mye_jobs_pending(mye_jobs *jobs);

/* Blocks until every submitted job has finished. For shutdown and tests --
 * never call this from inside a frame. */
void mye_jobs_wait_idle(mye_jobs *jobs);

#endif /* MYE_CORE_JOBS_H */
