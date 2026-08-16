/* Integration test for the fixed timestep: the property that makes physics
 * stable and replays reproducible. See plan/01-architecture.md. */
#include "core/engine.h"
#include "mye_test.h"

#define FIXED_DT (1.0f / 60.0f)

static int fixed_runs;
static float last_fixed_dt;
static int variable_runs;

typedef struct Counter {
    int steps;
    float distance;
} Counter;

static void FixedSystem(ecs_iter_t *it)
{
    Counter *c = ecs_field(it, Counter, 0);
    ++fixed_runs;
    last_fixed_dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        c[i].steps += 1;
        c[i].distance += 100.0f * (float)it->delta_time;
    }
}

static void VariableSystem(ecs_iter_t *it)
{
    (void)it;
    ++variable_runs;
}

static ecs_world_t *make_world(float fixed_dt, int max_steps)
{
    return mye_init(&(mye_config){
        .headless = true,
        .fixed_dt = fixed_dt,
        .max_steps_per_frame = max_steps,
    });
}

TEST(fixed_systems_run_at_a_constant_rate)
{
    fixed_runs = 0;
    last_fixed_dt = 0.0f;

    ecs_world_t *world = make_world(FIXED_DT, 5);
    ASSERT_NOT_NULL(world);

    ECS_COMPONENT(world, Counter);
    ECS_SYSTEM(world, FixedSystem, MyeOnFixedUpdate, Counter);

    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, Counter, { 0, 0.0f });

    /* 60 frames of exactly one step each. */
    for (int i = 0; i < 60; ++i) {
        mye_progress(world, FIXED_DT);
    }

    ASSERT_EQ_INT(60, fixed_runs);
    /* Fixed systems always see the fixed delta, never the frame delta. */
    ASSERT_NEAR((double)FIXED_DT, (double)last_fixed_dt, 1e-9);

    const Counter *c = ecs_get(world, e, Counter);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_INT(60, c->steps);
    ASSERT_NEAR(100.0, (double)c->distance, 0.01); /* 100 u/s for 1 s */

    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    ASSERT_EQ_U64(60, time->fixed_step);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(slow_frames_run_multiple_steps_fast_frames_run_none)
{
    fixed_runs = 0;

    ecs_world_t *world = make_world(FIXED_DT, 5);
    ASSERT_NOT_NULL(world);

    ECS_COMPONENT(world, Counter);
    ECS_SYSTEM(world, FixedSystem, MyeOnFixedUpdate, Counter);
    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, Counter, { 0, 0.0f });

    /* A frame that took four steps' worth of time catches up in one go. */
    mye_progress(world, FIXED_DT * 4.0f);
    ASSERT_EQ_INT(4, fixed_runs);
    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    ASSERT_EQ_INT(4, time->steps_this_frame);

    /* A frame far shorter than a step runs none, and banks the time. */
    fixed_runs = 0;
    mye_progress(world, FIXED_DT * 0.25f);
    ASSERT_EQ_INT(0, fixed_runs);
    time = ecs_singleton_get(world, MyeTime);
    ASSERT_EQ_INT(0, time->steps_this_frame);
    ASSERT_TRUE(time->accumulator > 0.0f);

    /* Banked time is spent later: four quarter-frames make one step. */
    for (int i = 0; i < 3; ++i) {
        mye_progress(world, FIXED_DT * 0.25f);
    }
    ASSERT_EQ_INT(1, fixed_runs);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(alpha_tracks_leftover_time_for_interpolation)
{
    ecs_world_t *world = make_world(FIXED_DT, 5);
    ASSERT_NOT_NULL(world);

    /* Half a step of unspent time means rendering should blend halfway. */
    mye_progress(world, FIXED_DT * 0.5f);
    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    ASSERT_NEAR(0.5, (double)time->alpha, 0.01);

    /* Another half completes a step and leaves nothing over. */
    mye_progress(world, FIXED_DT * 0.5f);
    time = ecs_singleton_get(world, MyeTime);
    ASSERT_NEAR(0.0, (double)time->alpha, 0.01);

    /* alpha stays in [0,1): it is a blend factor, not a step count. */
    mye_progress(world, FIXED_DT * 2.75f);
    time = ecs_singleton_get(world, MyeTime);
    ASSERT_TRUE(time->alpha >= 0.0f && time->alpha < 1.0f);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(long_stall_does_not_spiral)
{
    fixed_runs = 0;

    ecs_world_t *world = make_world(FIXED_DT, 5);
    ASSERT_NOT_NULL(world);

    ECS_COMPONENT(world, Counter);
    ECS_SYSTEM(world, FixedSystem, MyeOnFixedUpdate, Counter);
    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, Counter, { 0, 0.0f });

    /* Ten seconds of stall (600 steps' worth) must not run 600 steps. */
    mye_progress(world, 10.0f);
    ASSERT_EQ_INT(5, fixed_runs); /* clamped to max_steps_per_frame */

    /* And the backlog is dropped, so the next frame is normal again. */
    fixed_runs = 0;
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, fixed_runs);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(fixed_systems_are_not_run_by_the_main_pipeline)
{
    fixed_runs = 0;
    variable_runs = 0;

    ecs_world_t *world = make_world(FIXED_DT, 5);
    ASSERT_NOT_NULL(world);

    ECS_COMPONENT(world, Counter);
    ECS_SYSTEM(world, FixedSystem, MyeOnFixedUpdate, Counter);
    ECS_SYSTEM(world, VariableSystem, EcsOnUpdate, Counter);
    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, Counter, { 0, 0.0f });

    /* Two steps' worth of time: the fixed system runs twice, the ordinary
     * system once. If MyeOnFixedUpdate leaked into the main pipeline the
     * fixed count would be one higher per frame. */
    mye_progress(world, FIXED_DT * 2.0f);
    ASSERT_EQ_INT(2, fixed_runs);
    ASSERT_EQ_INT(1, variable_runs);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(simulation_is_deterministic_across_runs)
{
    /* The point of a fixed step: identical inputs give identical state, no
     * matter how the frames were chopped up. This is the property replays,
     * networking, and reproducible bug reports all depend on. */
    float distance[2] = { 0.0f, 0.0f };
    int steps[2] = { 0, 0 };

    /* Run 1: steady 60 fps. Run 2: wildly irregular frame times summing to
     * the same total. */
    const float frames_a[] = { FIXED_DT, FIXED_DT, FIXED_DT, FIXED_DT,
                               FIXED_DT, FIXED_DT, FIXED_DT, FIXED_DT };
    const float frames_b[] = { FIXED_DT * 2.0f, FIXED_DT * 0.5f,
                               FIXED_DT * 0.5f, FIXED_DT * 3.0f,
                               FIXED_DT * 0.25f, FIXED_DT * 0.25f,
                               FIXED_DT * 1.5f };

    for (int run = 0; run < 2; ++run) {
        ecs_world_t *world = make_world(FIXED_DT, 10);
        ASSERT_NOT_NULL(world);

        ECS_COMPONENT(world, Counter);
        ECS_SYSTEM(world, FixedSystem, MyeOnFixedUpdate, Counter);
        ecs_entity_t e = ecs_new(world);
        ecs_set(world, e, Counter, { 0, 0.0f });

        const float *frames = run == 0 ? frames_a : frames_b;
        size_t count = run == 0 ? sizeof frames_a / sizeof frames_a[0]
                                : sizeof frames_b / sizeof frames_b[0];
        for (size_t i = 0; i < count; ++i) {
            mye_progress(world, frames[i]);
        }

        const Counter *c = ecs_get(world, e, Counter);
        ASSERT_NOT_NULL(c);
        distance[run] = c->distance;
        steps[run] = c->steps;

        ASSERT_EQ_INT(0, mye_shutdown(world));
    }

    /* Same total time elapsed -> same number of steps -> bit-identical state,
     * despite completely different frame pacing. */
    ASSERT_EQ_INT(steps[0], steps[1]);
    ASSERT_EQ_U64((uint64_t)(distance[0] * 1000.0f),
                  (uint64_t)(distance[1] * 1000.0f));
}

TEST_MAIN(TEST_CASE(fixed_systems_run_at_a_constant_rate),
          TEST_CASE(slow_frames_run_multiple_steps_fast_frames_run_none),
          TEST_CASE(alpha_tracks_leftover_time_for_interpolation),
          TEST_CASE(long_stall_does_not_spiral),
          TEST_CASE(fixed_systems_are_not_run_by_the_main_pipeline),
          TEST_CASE(simulation_is_deterministic_across_runs))
