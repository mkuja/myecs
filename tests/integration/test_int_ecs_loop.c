/* M2 integration test: the engine loop end to end, headless.
 *
 * Covers what a player would notice if it broke -- entities actually move,
 * time advances, per-frame scratch memory is reclaimed every frame, and
 * shutdown leaves nothing allocated. See plan/09-testing.md. */
#include "core/engine.h"
#include "mye_test.h"

#define FIXED_DT (1.0f / 60.0f)
#define ENTITY_COUNT 1000
#define FRAMES 100
#define BOUND 500.0f

typedef struct Position {
    float x, y;
} Position;

typedef struct Velocity {
    float x, y;
} Velocity;

static void Move(ecs_iter_t *it)
{
    Position *p = ecs_field(it, Position, 0);
    const Velocity *v = ecs_field(it, Velocity, 1);

    for (int i = 0; i < it->count; ++i) {
        p[i].x += v[i].x * it->delta_time;
        p[i].y += v[i].y * it->delta_time;
        if (p[i].x < -BOUND || p[i].x > BOUND) p[i].x = -p[i].x;
        if (p[i].y < -BOUND || p[i].y > BOUND) p[i].y = -p[i].y;
    }
}

/* Allocates a large slice of the frame arena every frame. If the arena were
 * not reset at the top of each frame it would run dry within a few frames and
 * these allocations would start failing. */
static int frame_alloc_failures;
static size_t frame_arena_bytes_per_frame;

static void EatFrameArena(ecs_iter_t *it)
{
    mye_allocator frame = mye_frame_allocator(it->world);
    void *scratch = mye_alloc(frame, frame_arena_bytes_per_frame, 16);
    if (scratch == NULL) {
        ++frame_alloc_failures;
    }
}

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){
        .headless = true,
        .frame_arena_bytes = 64 * 1024,
    });
}

TEST(world_starts_and_shuts_down_clean)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_engine *engine = mye_engine_get(world);
    ASSERT_NOT_NULL(engine);
    ASSERT_TRUE(engine->headless);
    ASSERT_FALSE(engine->window_open);
    ASSERT_TRUE(mye_allocator_valid(mye_allocator_of(world)));
    ASSERT_TRUE(mye_allocator_valid(mye_frame_allocator(world)));
    /* flecs allocates through the engine allocator, so the world itself
     * shows up in tracking. */
    ASSERT_TRUE(engine->tracking.live_bytes > 0);

    /* Zero exit code means the tracking allocator found no leaks -- which
     * includes everything flecs allocated for the world. */
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(entities_move_over_frames)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ECS_COMPONENT(world, Position);
    ECS_COMPONENT(world, Velocity);
    ECS_SYSTEM(world, Move, EcsOnUpdate, Position, [in] Velocity);

    ecs_entity_t entities[ENTITY_COUNT];
    for (int i = 0; i < ENTITY_COUNT; ++i) {
        entities[i] = ecs_new(world);
        ecs_set(world, entities[i], Position, { 0.0f, 0.0f });
        /* Deterministic, non-zero, and different per entity. */
        ecs_set(world, entities[i], Velocity,
                { (float)(i % 37) + 1.0f, (float)(i % 53) + 1.0f });
    }

    for (int frame = 0; frame < FRAMES; ++frame) {
        ASSERT_TRUE(mye_progress(world, FIXED_DT));
    }

    for (int i = 0; i < ENTITY_COUNT; ++i) {
        const Position *p = ecs_get(world, entities[i], Position);
        ASSERT_NOT_NULL(p);
        ASSERT_TRUE(p->x != 0.0f); /* everything actually moved */
        ASSERT_TRUE(p->y != 0.0f);
        /* And stayed inside the bounds the system enforces. */
        ASSERT_TRUE(p->x >= -BOUND && p->x <= BOUND);
        ASSERT_TRUE(p->y >= -BOUND && p->y <= BOUND);
    }

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(time_singleton_advances)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    ASSERT_NOT_NULL(time);
    ASSERT_EQ_U64(0, time->frame);

    for (int frame = 0; frame < FRAMES; ++frame) {
        ASSERT_TRUE(mye_progress(world, FIXED_DT));
    }

    time = ecs_singleton_get(world, MyeTime);
    ASSERT_EQ_U64(FRAMES, time->frame);
    ASSERT_NEAR((double)FIXED_DT, (double)time->delta, 1e-6);
    ASSERT_NEAR((double)FIXED_DT * FRAMES, time->elapsed, 1e-3);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(frame_arena_is_reclaimed_every_frame)
{
    frame_alloc_failures = 0;

    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_engine *engine = mye_engine_get(world);
    ASSERT_NOT_NULL(engine);
    /* Ask for a third of the arena per frame: fine with a reset each frame,
     * exhausted by frame four without one. */
    frame_arena_bytes_per_frame = mye_arena_capacity(&engine->frame_arena) / 3;

    ECS_SYSTEM(world, EatFrameArena, EcsOnUpdate, MyeTime);

    for (int frame = 0; frame < FRAMES; ++frame) {
        ASSERT_TRUE(mye_progress(world, FIXED_DT));
    }

    ASSERT_EQ_INT(0, frame_alloc_failures);
    /* Peak usage stays at roughly one frame's worth, not FRAMES worth. */
    ASSERT_TRUE(mye_arena_high_water(&engine->frame_arena) <
                frame_arena_bytes_per_frame * 2);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(repeated_init_shutdown_leaves_nothing_behind)
{
    /* Catches state leaking between worlds -- a stale global allocator,
     * component ids surviving ecs_fini, and so on. */
    for (int i = 0; i < 3; ++i) {
        ecs_world_t *world = make_world();
        ASSERT_NOT_NULL(world);

        ECS_COMPONENT(world, Position);
        ecs_entity_t e = ecs_new(world);
        ecs_set(world, e, Position, { 1.0f, 2.0f });

        ASSERT_TRUE(mye_progress(world, FIXED_DT));
        ASSERT_EQ_INT(0, mye_shutdown(world));
    }
}

TEST_MAIN(TEST_CASE(world_starts_and_shuts_down_clean),
          TEST_CASE(entities_move_over_frames), TEST_CASE(time_singleton_advances),
          TEST_CASE(frame_arena_is_reclaimed_every_frame),
          TEST_CASE(repeated_init_shutdown_leaves_nothing_behind))
