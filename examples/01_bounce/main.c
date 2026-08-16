/* M2 -- ECS loop: 1000 entities moving and bouncing, driven entirely by
 * flecs systems. Shows the shape every game built on this engine will take:
 *
 *   components = plain structs, systems = functions over queries,
 *   the whole frame = one ecs_progress() call.
 *
 * See plan/02-ecs.md. */
#include "core/engine.h"

#include <raylib.h>

#include <stdio.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define ENTITY_COUNT 1000
#define RADIUS 6.0f

typedef struct Position {
    float x, y;
} Position;

typedef struct Velocity {
    float x, y;
} Velocity;

typedef struct Tint {
    Color color;
} Tint;

/* Simulation: runs in EcsOnUpdate, knows nothing about rendering. */
static void Move(ecs_iter_t *it)
{
    Position *p = ecs_field(it, Position, 0);
    const Velocity *v = ecs_field(it, Velocity, 1);

    for (int i = 0; i < it->count; ++i) {
        p[i].x += v[i].x * it->delta_time;
        p[i].y += v[i].y * it->delta_time;
    }
}

static void Bounce(ecs_iter_t *it)
{
    Position *p = ecs_field(it, Position, 0);
    Velocity *v = ecs_field(it, Velocity, 1);

    for (int i = 0; i < it->count; ++i) {
        if (p[i].x < RADIUS || p[i].x > (float)SCREEN_W - RADIUS) {
            v[i].x = -v[i].x;
            p[i].x = p[i].x < RADIUS ? RADIUS : (float)SCREEN_W - RADIUS;
        }
        if (p[i].y < RADIUS || p[i].y > (float)SCREEN_H - RADIUS) {
            v[i].y = -v[i].y;
            p[i].y = p[i].y < RADIUS ? RADIUS : (float)SCREEN_H - RADIUS;
        }
    }
}

/* Rendering: EcsOnStore, main thread only. See plan/03-rendering.md.
 *
 * Begin and end are their own systems rather than being folded into the
 * sprite pass. Systems only run when their query matches something, so a
 * frame with no sprites would otherwise call EndDrawing without a matching
 * BeginDrawing. These two bracket the frame by querying the MyeTime
 * singleton, which always exists. Registration order fixes the order within
 * the phase. */
static void RenderBegin(ecs_iter_t *it)
{
    (void)it;
    BeginDrawing();
    ClearBackground((Color){ 18, 18, 24, 255 });
}

static void DrawScene(ecs_iter_t *it)
{
    const Position *p = ecs_field(it, Position, 0);
    const Tint *t = ecs_field(it, Tint, 1);

    for (int i = 0; i < it->count; ++i) {
        DrawCircleV((Vector2){ p[i].x, p[i].y }, RADIUS, t[i].color);
    }
}

static void DrawHud(ecs_iter_t *it)
{
    const MyeTime *time = ecs_field(it, MyeTime, 0);

    /* Per-frame scratch: reclaimed automatically at the top of next frame,
     * so this never has to be freed. */
    mye_allocator frame = mye_frame_allocator(it->world);
    char *line = MYE_NEW_ARRAY(frame, char, 128);
    if (line != NULL) {
        snprintf(line, 128, "%d entities | frame %llu | %.1f s", ENTITY_COUNT,
                 (unsigned long long)time->frame, time->elapsed);
        DrawText(line, 16, 44, 20, RAYWHITE);
    }

    DrawFPS(16, 16);
    EndDrawing();
}

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .width = SCREEN_W,
        .height = SCREEN_H,
        .title = "myecs -- M2 bounce",
    });
    if (world == NULL) {
        return 1;
    }

    ECS_COMPONENT(world, Position);
    ECS_COMPONENT(world, Velocity);
    ECS_COMPONENT(world, Tint);

    ECS_SYSTEM(world, Move, EcsOnUpdate, Position, [in] Velocity);
    ECS_SYSTEM(world, Bounce, EcsOnUpdate, Position, Velocity);
    ECS_SYSTEM(world, RenderBegin, EcsOnStore, [in] MyeTime);
    ECS_SYSTEM(world, DrawScene, EcsOnStore, [in] Position, [in] Tint);
    ECS_SYSTEM(world, DrawHud, EcsOnStore, [in] MyeTime);

    SetRandomSeed(1234);
    for (int i = 0; i < ENTITY_COUNT; ++i) {
        ecs_entity_t e = ecs_new(world);
        ecs_set(world, e, Position,
                { (float)GetRandomValue(50, SCREEN_W - 50),
                  (float)GetRandomValue(50, SCREEN_H - 50) });
        ecs_set(world, e, Velocity,
                { (float)GetRandomValue(-300, 300),
                  (float)GetRandomValue(-300, 300) });
        ecs_set(world, e, Tint,
                { (Color){ (unsigned char)GetRandomValue(80, 255),
                           (unsigned char)GetRandomValue(80, 255),
                           (unsigned char)GetRandomValue(80, 255), 255 } });
    }

    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }

    return mye_shutdown(world);
}
