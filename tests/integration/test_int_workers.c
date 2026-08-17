/* Integration tests for flecs worker threads. See plan/05-concurrency.md.
 *
 * The claim under test is not "it is faster" -- that is what the stress
 * benchmark measures -- but "it produces the same answer". A parallel
 * simulation that quietly diverges from the serial one is worse than a slow
 * one, because replays, networking and bug reports all stop meaning anything.
 */
#include "core/engine.h"
#include "mye_test.h"
#include "render/render2d.h"

#include <stdatomic.h>
#include <string.h>

/* Comparing floats bit-for-bit needs memcpy, not a pointer cast: casting
 * float* to uint32_t* breaks strict aliasing and is undefined behaviour.
 * Only the Release build's optimiser diagnoses it, which is exactly why
 * -Werror is applied to every configuration. */
static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

#define FIXED_DT (1.0f / 60.0f)
#define ENTITY_COUNT 4000
#define STEPS 120

typedef struct Velocity2 {
    float x, y;
} Velocity2;

ECS_COMPONENT_DECLARE(Velocity2);

static atomic_int g_invocations;

/* Per-entity arithmetic only: touches its own fields and nothing else, which
 * is the precondition for marking a system multi_threaded. */
static void Integrate(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    Velocity2 *vel = ecs_field(it, Velocity2, 1);
    float dt = (float)it->delta_time;

    atomic_fetch_add(&g_invocations, 1);

    for (int i = 0; i < it->count; ++i) {
        pos[i].x += vel[i].x * dt;
        pos[i].y += vel[i].y * dt;

        if (pos[i].x < -500.0f || pos[i].x > 500.0f) vel[i].x = -vel[i].x;
        if (pos[i].y < -500.0f || pos[i].y > 500.0f) vel[i].y = -vel[i].y;
    }
}

/* Builds an identical world, optionally with workers, and runs it. Positions
 * are written into `out` in creation order. */
static bool run_simulation(int threads, float *out, int count)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .headless = true,
        .worker_threads = threads,
    });
    if (world == NULL) {
        return false;
    }

    ECS_COMPONENT_DEFINE(world, Velocity2);

    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "Integrate",
                                      .add = ecs_ids(ecs_dependson(EcsOnUpdate)) }),
        .query.terms = {
            { .id = ecs_id(MyePosition2D) },
            { .id = ecs_id(Velocity2) },
        },
        .callback = Integrate,
        .multi_threaded = true,
    });

    /* Deterministic setup: no RNG, just arithmetic on the index. */
    ecs_entity_t *entities = malloc(sizeof(ecs_entity_t) * (size_t)count);
    if (entities == NULL) {
        mye_shutdown(world);
        return false;
    }
    for (int i = 0; i < count; ++i) {
        entities[i] = mye_entity_new(world);
        ecs_set(world, entities[i], MyePosition2D,
                { (float)(i % 173) - 86.0f, (float)(i % 91) - 45.0f });
        ecs_set(world, entities[i], Velocity2,
                { (float)(i % 37) - 18.0f, (float)(i % 53) - 26.0f });
    }

    for (int step = 0; step < STEPS; ++step) {
        mye_progress(world, FIXED_DT);
    }

    for (int i = 0; i < count; ++i) {
        const MyePosition2D *p = ecs_get(world, entities[i], MyePosition2D);
        out[i * 2 + 0] = p != NULL ? p->x : 0.0f;
        out[i * 2 + 1] = p != NULL ? p->y : 0.0f;
    }

    free(entities);
    return mye_shutdown(world) == 0;
}

TEST(workers_produce_bit_identical_results_to_a_single_thread)
{
    static float serial[ENTITY_COUNT * 2];
    static float parallel[ENTITY_COUNT * 2];

    atomic_init(&g_invocations, 0);
    ASSERT_TRUE(run_simulation(0, serial, ENTITY_COUNT));
    int serial_invocations = atomic_load(&g_invocations);

    atomic_init(&g_invocations, 0);
    ASSERT_TRUE(run_simulation(8, parallel, ENTITY_COUNT));
    int parallel_invocations = atomic_load(&g_invocations);

    /* The system really was sharded: flecs invokes the callback once per
     * worker per matched table, so the parallel run calls it more often for
     * the same amount of work. Without this the test could pass simply
     * because threading never engaged. */
    ASSERT_TRUE(parallel_invocations > serial_invocations);

    /* And every entity ended up in exactly the same place. Bit-identical,
     * not approximately: per-entity arithmetic does not depend on the order
     * entities are visited in, so parallelism must not change the result. */
    for (int i = 0; i < ENTITY_COUNT * 2; ++i) {
        ASSERT_EQ_U64(float_bits(serial[i]), float_bits(parallel[i]));
    }
}

TEST(a_worker_world_starts_and_stops_cleanly)
{
    /* Repeatedly: threads must be joined on shutdown and nothing leaked. A
     * leaked thread or unjoined worker shows up here or under TSan. */
    for (int round = 0; round < 3; ++round) {
        ecs_world_t *world = mye_init(&(mye_config){
            .headless = true,
            .worker_threads = 4,
        });
        ASSERT_NOT_NULL(world);

        ECS_COMPONENT_DEFINE(world, Velocity2);
        for (int i = 0; i < 100; ++i) {
            ecs_entity_t e = mye_entity_new(world);
            ecs_set(world, e, MyePosition2D, { 0.0f, 0.0f });
        }

        for (int step = 0; step < 5; ++step) {
            mye_progress(world, FIXED_DT);
        }

        ASSERT_EQ_INT(0, mye_shutdown(world));
    }
}

TEST(one_worker_is_treated_as_single_threaded)
{
    /* worker_threads of 0 or 1 must not spin up flecs' worker machinery --
     * it would be pure overhead for no parallelism. */
    static float a[100 * 2];
    static float b[100 * 2];

    ASSERT_TRUE(run_simulation(0, a, 100));
    ASSERT_TRUE(run_simulation(1, b, 100));

    for (int i = 0; i < 100 * 2; ++i) {
        ASSERT_EQ_U64(float_bits(a[i]), float_bits(b[i]));
    }
}

TEST_MAIN(TEST_CASE(workers_produce_bit_identical_results_to_a_single_thread),
          TEST_CASE(a_worker_world_starts_and_stops_cleanly),
          TEST_CASE(one_worker_is_treated_as_single_threaded))
