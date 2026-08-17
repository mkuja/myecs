/* Render smoke test: opens a real (hidden) window, renders frames through the
 * engine loop, and asserts the engine survives and shuts down clean.
 *
 * Labeled "render" in CTest, so headless machines can skip it:
 *     ctest -LE render
 * See plan/09-testing.md. */
#include "asset/asset.h"
#include "core/engine.h"
#include "render/camera.h"
#include "render/render3d.h"
#include "scene/transform.h"
#include "mye_test.h"

#include <raylib.h>

#define FRAMES 30

typedef struct Position {
    float x, y;
} Position;

static int frames_drawn;

static void Draw(ecs_iter_t *it)
{
    const Position *p = ecs_field(it, Position, 0);

    BeginDrawing();
    ClearBackground(BLACK);
    for (int i = 0; i < it->count; ++i) {
        DrawCircleV((Vector2){ p[i].x, p[i].y }, 4.0f, RAYWHITE);
    }
    DrawFPS(8, 8);
    EndDrawing();
    ++frames_drawn;
}

TEST(renders_frames_and_shuts_down_clean)
{
    /* Must precede InitWindow, which mye_init performs. */
    SetConfigFlags(FLAG_WINDOW_HIDDEN);

    ecs_world_t *world = mye_init(&(mye_config){
        .width = 640,
        .height = 360,
        .title = "myecs render smoke",
        .max_frames = FRAMES,
    });
    if (world == NULL) {
        /* No GPU/display available (headless CI without the label filter). */
        SKIP("no window could be created");
    }

    ECS_COMPONENT(world, Position);
    ECS_SYSTEM(world, Draw, EcsOnStore, [in] Position);

    for (int i = 0; i < 64; ++i) {
        ecs_entity_t e = ecs_new(world);
        ecs_set(world, e, Position, { (float)(i * 8 + 16), (float)(i * 4 + 16) });
    }

    /* The 3D pass with a resolved camera, through real GL: a parented
     * camera looking at a mesh. Nothing here asserts pixels; it asserts that
     * the whole path -- resolve, BeginMode3D, DrawMesh -- runs without
     * tripping the sanitizers or leaking. */
    ecs_entity_t pivot = mye_spawn_3d(world, (Vector3){ 0.0f, 0.0f, 0.0f });
    ecs_entity_t camera = mye_camera3d_spawn(world, (Vector3){ 0.0f, 3.0f, 8.0f },
                                             (Vector3){ 0.0f, 0.0f, 0.0f }, 50.0f);
    mye_set_parent(world, camera, pivot);
    mye_model cube = mye_model_from_mesh(world, "smoke:cube",
                                         GenMeshCube(1.0f, 1.0f, 1.0f), WHITE);
    mye_mesh_spawn(world, cube, (Vector3){ 0.0f, 0.5f, 0.0f }, SKYBLUE);

    frames_drawn = 0;
    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }

    /* max_frames stopped the loop, and drawing actually happened. */
    ASSERT_EQ_INT(FRAMES, frames_drawn);

    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    ASSERT_NOT_NULL(time);
    ASSERT_EQ_U64(FRAMES, time->frame);

    ASSERT_EQ_INT(0, mye_shutdown(world)); /* no leaks */
}

TEST_MAIN(TEST_CASE(renders_frames_and_shuts_down_clean))
