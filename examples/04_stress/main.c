/* M7 -- stress benchmark: many entities, measured.
 *
 * Exists to answer one question with numbers rather than intuition: does
 * turning on flecs' worker threads actually make this engine faster, and at
 * what entity count? See plan/05-concurrency.md.
 *
 * Runs headless by default so it measures simulation rather than the GPU.
 *
 *   MYE_STRESS_ENTITIES=50000 MYE_STRESS_THREADS=0 ./example_04_stress
 *   MYE_STRESS_ENTITIES=50000 MYE_STRESS_THREADS=8 ./example_04_stress
 *   MYE_STRESS_WINDOW=1 ./example_04_stress          # watch it instead
 */
#include "core/engine.h"
#include "render/render2d.h"
#include "render/camera.h"
#include "render/render3d.h"
#include "scene/transform.h"

#include <raylib.h>
#include <raymath.h>

#include <stdio.h>
#include <stdlib.h>

#define WORLD_EXTENT 400.0f

typedef struct Velocity2 {
    float x, y;
} Velocity2;

typedef struct Wobble {
    float phase;
    float speed;
    float radius;
} Wobble;

ECS_COMPONENT_DECLARE(Velocity2);
ECS_COMPONENT_DECLARE(Wobble);

/* ------------------------------------------------------------- systems -- */

/* Pure per-entity arithmetic over its own query fields: the shape of system
 * that is safe to shard across worker threads. */
static void MoveAndBounce(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    Velocity2 *vel = ecs_field(it, Velocity2, 1);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        pos[i].x += vel[i].x * dt;
        pos[i].y += vel[i].y * dt;

        if (pos[i].x < -WORLD_EXTENT || pos[i].x > WORLD_EXTENT) {
            vel[i].x = -vel[i].x;
        }
        if (pos[i].y < -WORLD_EXTENT || pos[i].y > WORLD_EXTENT) {
            vel[i].y = -vel[i].y;
        }
    }
}

/* Deliberately heavier: trigonometry per entity, so the benchmark has real
 * work to parallelise rather than being purely memory-bound. */
static void WobbleSystem(ecs_iter_t *it)
{
    Wobble *wobble = ecs_field(it, Wobble, 0);
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 1);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        wobble[i].phase += wobble[i].speed * dt;
        pos[i].x += cosf(wobble[i].phase) * wobble[i].radius * dt;
        pos[i].y += sinf(wobble[i].phase * 1.3f) * wobble[i].radius * dt;
    }
}

static void SpinMeshes(ecs_iter_t *it)
{
    MyeRotation3D *rot = ecs_field(it, MyeRotation3D, 0);
    const Wobble *wobble = ecs_field(it, Wobble, 1);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        rot[i].q = QuaternionMultiply(
            rot[i].q,
            QuaternionFromAxisAngle((Vector3){ 0.0f, 1.0f, 0.0f },
                                    wobble[i].speed * dt));
    }
}

/* ----------------------------------------------------------- environment -- */

static int env_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    return (end != value && *end == '\0') ? (int)parsed : fallback;
}

/* --------------------------------------------------------------- timing -- */

typedef struct timing {
    double total_ms;
    double min_ms;
    double max_ms;
    int samples;
} timing;

static void timing_add(timing *t, double ms)
{
    if (t->samples == 0 || ms < t->min_ms) t->min_ms = ms;
    if (t->samples == 0 || ms > t->max_ms) t->max_ms = ms;
    t->total_ms += ms;
    ++t->samples;
}

int main(void)
{
    int entity_count = env_int("MYE_STRESS_ENTITIES", 50000);
    int threads = env_int("MYE_STRESS_THREADS", 0);
    int frames = env_int("MYE_STRESS_FRAMES", 600);
    bool windowed = env_int("MYE_STRESS_WINDOW", 0) != 0;
    int mesh_count = env_int("MYE_STRESS_MESHES", 0);

    ecs_world_t *world = mye_init(&(mye_config){
        .width = 1280,
        .height = 720,
        .title = "myecs -- M7 stress",
        .headless = !windowed,
        .worker_threads = threads,
        /* Enough scratch for the renderer's draw list at this scale. */
        .frame_arena_bytes = 16u * 1024u * 1024u,
    });
    if (world == NULL) {
        return 1;
    }

    ECS_COMPONENT_DEFINE(world, Velocity2);
    ECS_COMPONENT_DEFINE(world, Wobble);

    /* `.multi_threaded = true` is what actually allows sharding. Each of
     * these touches only its own fields, which is the precondition. */
    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "MoveAndBounce",
                                      .add = ecs_ids(ecs_dependson(EcsOnUpdate)) }),
        .query.terms = {
            { .id = ecs_id(MyePosition2D) },
            { .id = ecs_id(Velocity2) },
        },
        .callback = MoveAndBounce,
        .multi_threaded = true,
    });

    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "WobbleSystem",
                                      .add = ecs_ids(ecs_dependson(EcsOnUpdate)) }),
        .query.terms = {
            { .id = ecs_id(Wobble) },
            { .id = ecs_id(MyePosition2D) },
        },
        .callback = WobbleSystem,
        .multi_threaded = true,
    });

    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "SpinMeshes",
                                      .add = ecs_ids(ecs_dependson(EcsOnUpdate)) }),
        .query.terms = {
            { .id = ecs_id(MyeRotation3D) },
            { .id = ecs_id(Wobble) },
        },
        .callback = SpinMeshes,
        .multi_threaded = true,
    });

    SetRandomSeed(4242);

    mye_texture dot = { 0 };
    mye_model cube = { 0 };
    if (windowed) {
        Image image = GenImageColor(4, 4, WHITE);
        dot = mye_texture_from_image(world, "gen:dot", image);
        cube = mye_model_from_mesh(world, "gen:cube",
                                   GenMeshCube(1.0f, 1.0f, 1.0f), WHITE);
        mye_camera3d_spawn(world, (Vector3){ 0.0f, 60.0f, 120.0f },
                           (Vector3){ 0.0f, 0.0f, 0.0f }, 55.0f);
    }

    for (int i = 0; i < entity_count; ++i) {
        ecs_entity_t e = mye_entity_new(world);
        ecs_set(world, e, MyePosition2D,
                { (float)GetRandomValue(-400, 400),
                  (float)GetRandomValue(-400, 400) });
        ecs_set(world, e, Velocity2,
                { (float)GetRandomValue(-60, 60),
                  (float)GetRandomValue(-60, 60) });
        ecs_set(world, e, Wobble,
                { .phase = (float)GetRandomValue(0, 628) * 0.01f,
                  .speed = 1.0f + (float)GetRandomValue(0, 200) * 0.01f,
                  .radius = 4.0f });

        if (windowed) {
            ecs_set(world, e, MyeSprite,
                    { .texture = dot,
                      .origin = { 2.0f, 2.0f },
                      .tint = (Color){ (unsigned char)GetRandomValue(80, 255),
                                       (unsigned char)GetRandomValue(80, 255),
                                       (unsigned char)GetRandomValue(80, 255),
                                       255 },
                      .layer = 0 });
        }
    }

    for (int i = 0; i < mesh_count; ++i) {
        ecs_entity_t e = mye_spawn_3d(world,
                                      (Vector3){ (float)GetRandomValue(-60, 60),
                                                 (float)GetRandomValue(0, 40),
                                                 (float)GetRandomValue(-60, 60) });
        ecs_set(world, e, Wobble,
                { .phase = 0.0f,
                  .speed = 0.5f + (float)GetRandomValue(0, 100) * 0.01f,
                  .radius = 0.0f });
        if (windowed) {
            ecs_set(world, e, MyeMeshInstance, { .model = cube, .tint = WHITE });
        }
    }

    printf("stress: %d entities, %d meshes, %d threads, %d frames, %s\n",
           entity_count, mesh_count, threads, frames,
           windowed ? "windowed" : "headless");

    /* Warm-up frames, so table creation and first-touch page faults do not
     * land in the measurement. */
    for (int i = 0; i < 30; ++i) {
        mye_progress(world, 1.0f / 60.0f);
    }

    timing t = { 0 };
    for (int i = 0; i < frames; ++i) {
        double start = mye_time_now();
        mye_progress(world, 1.0f / 60.0f);
        timing_add(&t, (mye_time_now() - start) * 1000.0);
    }

    double average = t.total_ms / (double)(t.samples > 0 ? t.samples : 1);
    printf("frame ms: avg %.3f  min %.3f  max %.3f   (%.1f fps equivalent)\n",
           average, t.min_ms, t.max_ms, 1000.0 / (average > 0.0 ? average : 1.0));

    return mye_shutdown(world);
}
