/* M5 -- 3D scene: a transform hierarchy, lighting, and a 2D HUD drawn over
 * the top in the same frame.
 *
 * The hierarchy is the point. A tank body carries a turret, the turret
 * carries a barrel, and the barrel carries a muzzle marker. Only the body is
 * driven; everything else follows because it is parented. Orbiting moons
 * around a spinning planet make the same point at a larger scale.
 *
 * All geometry is generated at runtime (GenMesh*), so the example ships no
 * model files.
 *
 * Controls: arrow keys / WASD orbit the camera, Q and E change height,
 * G toggles the ground grid, SPACE pauses the animation.
 */
#include "asset/asset.h"
#include "core/engine.h"
#include "input/input.h"
#include "render/render2d.h"
#include "render/render3d.h"
#include "scene/transform.h"

#include <raylib.h>
#include <raymath.h>

#include <stdio.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define MOON_COUNT 4

enum {
    ACT_ORBIT = 0, /* axis: -1 left, +1 right */
    ACT_PITCH,     /* axis: -1 down, +1 up */
    ACT_HEIGHT,    /* axis: -1 lower, +1 raise */
    ACT_TOGGLE_GRID,
    ACT_PAUSE,
};

/* ------------------------------------------------------------ components -- */

/* Spins an entity about its own Y axis -- the driver for the whole scene. */
typedef struct Spin {
    float radians_per_second;
    float angle;
} Spin;

/* Bobs an entity up and down around its rest height. */
typedef struct Bob {
    float amplitude;
    float speed;
    float base_y;
    float phase;
} Bob;

typedef struct SceneState {
    ecs_entity_t camera;
    float orbit_angle;
    float orbit_distance;
    float camera_height;
    bool paused;

    ecs_entity_t tank_body;
    ecs_entity_t turret;
} SceneState;

ECS_COMPONENT_DECLARE(Spin);
ECS_COMPONENT_DECLARE(Bob);
ECS_COMPONENT_DECLARE(SceneState);

/* ------------------------------------------------------------- systems -- */

static void SpinSystem(ecs_iter_t *it)
{
    Spin *spin = ecs_field(it, Spin, 0);
    MyeRotation3D *rot = ecs_field(it, MyeRotation3D, 1);

    const SceneState *scene = ecs_singleton_get(it->world, SceneState);
    if (scene != NULL && scene->paused) {
        return;
    }

    float dt = (float)it->delta_time;
    for (int i = 0; i < it->count; ++i) {
        spin[i].angle += spin[i].radians_per_second * dt;
        rot[i].q = QuaternionFromAxisAngle((Vector3){ 0.0f, 1.0f, 0.0f },
                                           spin[i].angle);
    }
}

static void BobSystem(ecs_iter_t *it)
{
    Bob *bob = ecs_field(it, Bob, 0);
    MyePosition3D *pos = ecs_field(it, MyePosition3D, 1);

    const SceneState *scene = ecs_singleton_get(it->world, SceneState);
    if (scene != NULL && scene->paused) {
        return;
    }

    float dt = (float)it->delta_time;
    for (int i = 0; i < it->count; ++i) {
        bob[i].phase += bob[i].speed * dt;
        pos[i].v.y = bob[i].base_y + sinf(bob[i].phase) * bob[i].amplitude;
    }
}

/* Orbit camera: reads actions, writes the camera component. */
static void CameraControl(ecs_iter_t *it)
{
    SceneState *scene = ecs_field(it, SceneState, 0);
    ecs_world_t *world = it->world;
    float dt = (float)it->delta_time;

    scene->orbit_angle += mye_action_value(world, ACT_ORBIT) * 1.8f * dt;
    scene->camera_height += mye_action_value(world, ACT_HEIGHT) * 6.0f * dt;
    scene->orbit_distance -= mye_action_value(world, ACT_PITCH) * 10.0f * dt;

    if (scene->orbit_distance < 6.0f) scene->orbit_distance = 6.0f;
    if (scene->orbit_distance > 45.0f) scene->orbit_distance = 45.0f;
    if (scene->camera_height < 1.0f) scene->camera_height = 1.0f;
    if (scene->camera_height > 30.0f) scene->camera_height = 30.0f;

    MyeCamera3D *cam = ecs_ensure(world, scene->camera, MyeCamera3D);
    if (cam != NULL) {
        cam->camera.position = (Vector3){
            cosf(scene->orbit_angle) * scene->orbit_distance,
            scene->camera_height,
            sinf(scene->orbit_angle) * scene->orbit_distance,
        };
        cam->camera.target = (Vector3){ 0.0f, 2.0f, 0.0f };
        ecs_modified(world, scene->camera, MyeCamera3D);
    }

    if (mye_action_pressed(world, ACT_PAUSE)) {
        scene->paused = !scene->paused;
    }
    if (mye_action_pressed(world, ACT_TOGGLE_GRID)) {
        MyeRender3dConfig *config = ecs_singleton_ensure(world,
                                                         MyeRender3dConfig);
        config->draw_grid = !config->draw_grid;
        ecs_singleton_modified(world, MyeRender3dConfig);
    }
}

/* 2D HUD drawn over the 3D scene -- the mixed-rendering proof. */
static void DrawHud(ecs_iter_t *it)
{
    const SceneState *scene = ecs_field(it, SceneState, 0);
    ecs_world_t *world = it->world;

    Vector3 muzzle = mye_world_position(world, scene->turret);
    mye_allocator frame = mye_frame_allocator(world);
    char *line = MYE_NEW_ARRAY(frame, char, 160);
    if (line != NULL) {
        snprintf(line, 160,
                 "turret world pos: %.2f, %.2f, %.2f   %s",
                 (double)muzzle.x, (double)muzzle.y, (double)muzzle.z,
                 scene->paused ? "[PAUSED]" : "");
        DrawText(line, 20, 20, 20, RAYWHITE);
    }

    DrawText("arrows/WASD orbit - Q/E height - G grid - SPACE pause", 20,
             SCREEN_H - 34, 18, (Color){ 200, 200, 210, 255 });
    DrawFPS(SCREEN_W - 90, 20);
}

/* ---------------------------------------------------------------- scene -- */

static void build_scene(ecs_world_t *world)
{
    /* Geometry, generated rather than loaded. */
    mye_model ground = mye_model_from_mesh(world, "gen:ground",
                                           GenMeshPlane(40.0f, 40.0f, 4, 4),
                                           (Color){ 70, 90, 70, 255 });
    mye_model body = mye_model_from_mesh(world, "gen:body",
                                         GenMeshCube(3.0f, 1.0f, 4.0f),
                                         (Color){ 120, 130, 90, 255 });
    mye_model turret = mye_model_from_mesh(world, "gen:turret",
                                           GenMeshCylinder(1.0f, 0.8f, 12),
                                           (Color){ 150, 160, 110, 255 });
    mye_model barrel = mye_model_from_mesh(world, "gen:barrel",
                                           GenMeshCube(0.3f, 0.3f, 3.0f),
                                           (Color){ 90, 95, 70, 255 });
    mye_model planet = mye_model_from_mesh(world, "gen:planet",
                                           GenMeshSphere(2.0f, 24, 24),
                                           (Color){ 90, 110, 190, 255 });
    mye_model moon = mye_model_from_mesh(world, "gen:moon",
                                         GenMeshSphere(0.5f, 12, 12),
                                         (Color){ 190, 190, 200, 255 });

    mye_mesh_spawn(world, ground, (Vector3){ 0.0f, 0.0f, 0.0f }, WHITE);

    /* --- the tank: body -> turret -> barrel ---------------------------- */
    ecs_entity_t tank = mye_mesh_spawn(world, body,
                                       (Vector3){ -6.0f, 0.5f, 0.0f }, WHITE);
    ecs_set(world, tank, Spin, { .radians_per_second = 0.4f });

    /* Positioned relative to the body: 0.9 units above its centre. */
    ecs_entity_t tank_turret = mye_mesh_spawn(world, turret,
                                              (Vector3){ 0.0f, 0.9f, 0.0f },
                                              WHITE);
    mye_set_parent(world, tank_turret, tank);
    ecs_set(world, tank_turret, Spin, { .radians_per_second = -1.1f });

    /* And the barrel relative to the turret, so it inherits both rotations
     * without any code linking them. */
    ecs_entity_t tank_barrel = mye_mesh_spawn(world, barrel,
                                              (Vector3){ 0.0f, 0.2f, 1.6f },
                                              WHITE);
    mye_set_parent(world, tank_barrel, tank_turret);

    /* --- the planet: spinning, with parented moons --------------------- */
    ecs_entity_t planet_entity = mye_mesh_spawn(world, planet,
                                                (Vector3){ 7.0f, 3.0f, 0.0f },
                                                WHITE);
    ecs_set(world, planet_entity, Spin, { .radians_per_second = 0.6f });

    for (int i = 0; i < MOON_COUNT; ++i) {
        float angle = (float)i * (2.0f * PI / (float)MOON_COUNT);
        float radius = 4.0f;
        ecs_entity_t m = mye_mesh_spawn(
            world, moon,
            (Vector3){ cosf(angle) * radius, 0.0f, sinf(angle) * radius },
            WHITE);
        mye_set_parent(world, m, planet_entity);
        /* Bobbing is in the moon's own space, so it rides the planet's spin
         * while moving independently. */
        ecs_set(world, m, Bob,
                { .amplitude = 0.8f, .speed = 1.5f, .base_y = 0.0f,
                  .phase = angle });
    }

    /* --- lights -------------------------------------------------------- */
    ecs_entity_t key_light = ecs_new(world);
    ecs_set(world, key_light, MyeLight,
            { .direction = { -0.6f, -1.0f, -0.4f },
              .color = (Color){ 255, 244, 214, 255 },
              .intensity = 1.0f,
              .enabled = true });

    ecs_entity_t fill_light = ecs_new(world);
    ecs_set(world, fill_light, MyeLight,
            { .direction = { 0.8f, -0.3f, 0.5f },
              .color = (Color){ 120, 150, 255, 255 },
              .intensity = 0.4f,
              .enabled = true });

    SceneState *scene = ecs_singleton_ensure(world, SceneState);
    scene->tank_body = tank;
    scene->turret = tank_turret;
}

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .width = SCREEN_W,
        .height = SCREEN_H,
        .title = "myecs -- M5 3D scene",
    });
    if (world == NULL) {
        return 1;
    }

    ECS_COMPONENT_DEFINE(world, Spin);
    ECS_COMPONENT_DEFINE(world, Bob);
    ECS_COMPONENT_DEFINE(world, SceneState);
    ecs_add_id(world, ecs_id(SceneState), EcsSingleton);

    mye_input_bind_axis_keys(world, ACT_ORBIT, KEY_LEFT, KEY_RIGHT);
    mye_input_bind_axis_keys(world, ACT_ORBIT, KEY_A, KEY_D);
    mye_input_bind_axis_keys(world, ACT_PITCH, KEY_DOWN, KEY_UP);
    mye_input_bind_axis_keys(world, ACT_PITCH, KEY_S, KEY_W);
    mye_input_bind_axis_keys(world, ACT_HEIGHT, KEY_Q, KEY_E);
    mye_input_bind_key(world, ACT_TOGGLE_GRID, KEY_G);
    mye_input_bind_key(world, ACT_PAUSE, KEY_SPACE);

    ecs_singleton_set(world, SceneState,
                      { .orbit_angle = 0.7f,
                        .orbit_distance = 22.0f,
                        .camera_height = 9.0f });
    SceneState *scene = ecs_singleton_ensure(world, SceneState);
    scene->camera = mye_camera3d_spawn(world, (Vector3){ 16.0f, 9.0f, 16.0f },
                                       (Vector3){ 0.0f, 2.0f, 0.0f }, 50.0f);

    build_scene(world);

    /* Animation on the fixed step: framerate-independent, like the 2D game. */
    ECS_SYSTEM(world, SpinSystem, MyeOnFixedUpdate, Spin, MyeRotation3D);
    ECS_SYSTEM(world, BobSystem, MyeOnFixedUpdate, Bob, MyePosition3D);
    ECS_SYSTEM(world, CameraControl, EcsOnUpdate, SceneState);
    ECS_SYSTEM(world, DrawHud, MyeOnDrawUI, [in] SceneState);

    /* A darker sky than the 2D default suits a lit 3D scene. */
    MyeRenderConfig *render_config = ecs_singleton_ensure(world,
                                                          MyeRenderConfig);
    render_config->clear_color = (Color){ 24, 26, 34, 255 };
    ecs_singleton_modified(world, MyeRenderConfig);

    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }

    return mye_shutdown(world);
}
