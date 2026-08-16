/* Skeletal animation and physically-based rendering, on real glTF assets.
 *
 * Both models come from the Khronos glTF sample set and are CC0 (public
 * domain). Fetch them first:
 *
 *     tools/fetch_sample_assets.sh
 *
 *   Fox      -- one rig, three animation cycles (Survey, Walk, Run)
 *   BoomBox  -- metallic-roughness, normal, emissive and occlusion maps
 *
 * Controls: arrows/WASD orbit, Q/E height, 1-3 switch the fox's animation,
 * P toggles PBR against Blinn-Phong, SPACE pauses.
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

enum {
    ACT_ORBIT = 0,
    ACT_ZOOM,
    ACT_HEIGHT,
    ACT_ANIM_1,
    ACT_ANIM_2,
    ACT_ANIM_3,
    ACT_TOGGLE_PBR,
    ACT_PAUSE,
};

typedef struct ShowcaseState {
    ecs_entity_t camera;
    ecs_entity_t fox;
    ecs_entity_t boombox;

    float orbit_angle;
    float orbit_distance;
    float camera_height;
    bool paused;

    int animation_count;
    bool assets_ok;
} ShowcaseState;

ECS_COMPONENT_DECLARE(ShowcaseState);

/* ------------------------------------------------------------- systems -- */

static void CameraControl(ecs_iter_t *it)
{
    ShowcaseState *state = ecs_field(it, ShowcaseState, 0);
    ecs_world_t *world = it->world;
    float dt = (float)it->delta_time;

    state->orbit_angle += mye_action_value(world, ACT_ORBIT) * 1.6f * dt;
    state->orbit_distance -= mye_action_value(world, ACT_ZOOM) * 60.0f * dt;
    state->camera_height += mye_action_value(world, ACT_HEIGHT) * 60.0f * dt;

    if (state->orbit_distance < 30.0f) state->orbit_distance = 30.0f;
    if (state->orbit_distance > 400.0f) state->orbit_distance = 400.0f;
    if (state->camera_height < 5.0f) state->camera_height = 5.0f;
    if (state->camera_height > 250.0f) state->camera_height = 250.0f;

    MyeCamera3D *cam = ecs_ensure(world, state->camera, MyeCamera3D);
    if (cam != NULL) {
        cam->camera.position = (Vector3){
            cosf(state->orbit_angle) * state->orbit_distance,
            state->camera_height,
            sinf(state->orbit_angle) * state->orbit_distance,
        };
        cam->camera.target = (Vector3){ 0.0f, 40.0f, 0.0f };
        ecs_modified(world, state->camera, MyeCamera3D);
    }

    /* Switching animation cycles on one rig -- the thing Fox exists to
     * demonstrate. */
    int wanted = -1;
    if (mye_action_pressed(world, ACT_ANIM_1)) wanted = 0;
    if (mye_action_pressed(world, ACT_ANIM_2)) wanted = 1;
    if (mye_action_pressed(world, ACT_ANIM_3)) wanted = 2;

    if (wanted >= 0 && wanted < state->animation_count) {
        MyeModelAnimator *animator = ecs_ensure(world, state->fox,
                                                MyeModelAnimator);
        if (animator != NULL) {
            animator->animation = wanted;
            animator->frame = 0.0f;
            animator->playing = true;
            ecs_modified(world, state->fox, MyeModelAnimator);
        }
    }

    if (mye_action_pressed(world, ACT_PAUSE)) {
        state->paused = !state->paused;
        MyeModelAnimator *animator = ecs_ensure(world, state->fox,
                                                MyeModelAnimator);
        if (animator != NULL) {
            animator->playing = !state->paused;
            ecs_modified(world, state->fox, MyeModelAnimator);
        }
    }

    if (mye_action_pressed(world, ACT_TOGGLE_PBR)) {
        MyeRender3dConfig *config = ecs_singleton_ensure(world,
                                                         MyeRender3dConfig);
        config->use_pbr = !config->use_pbr;
        ecs_singleton_modified(world, MyeRender3dConfig);
    }
}

static void SpinBoomBox(ecs_iter_t *it)
{
    MyeRotation3D *rot = ecs_field(it, MyeRotation3D, 0);
    const ShowcaseState *state = ecs_singleton_get(it->world, ShowcaseState);
    if (state != NULL && state->paused) {
        return;
    }

    static float angle = 0.0f;
    angle += 0.5f * (float)it->delta_time;
    for (int i = 0; i < it->count; ++i) {
        rot[i].q = QuaternionFromAxisAngle((Vector3){ 0.0f, 1.0f, 0.0f },
                                           angle);
    }
}

static void DrawHud(ecs_iter_t *it)
{
    const ShowcaseState *state = ecs_field(it, ShowcaseState, 0);
    ecs_world_t *world = it->world;

    const MyeRender3dConfig *config = ecs_singleton_get(world,
                                                        MyeRender3dConfig);
    /* Only once there is a fox: ecs_get on entity 0 aborts inside flecs, so
     * reading this before the assets_ok check below would crash on exactly
     * the failure the check exists to report. */
    const MyeModelAnimator *animator =
        state->fox != 0 ? ecs_get(world, state->fox, MyeModelAnimator) : NULL;

    mye_allocator frame = mye_frame_allocator(world);
    char *line = MYE_NEW_ARRAY(frame, char, 192);

    if (!state->assets_ok) {
        DrawText("assets missing -- run tools/fetch_sample_assets.sh", 20, 20,
                 22, (Color){ 240, 140, 120, 255 });
        return;
    }

    if (line != NULL) {
        snprintf(line, 192, "shading: %s     fox animation: %d/%d  frame %.1f",
                 (config != NULL && config->use_pbr) ? "PBR (Cook-Torrance GGX)"
                                                     : "Blinn-Phong",
                 animator != NULL ? animator->animation + 1 : 0,
                 state->animation_count,
                 animator != NULL ? (double)animator->frame : 0.0);
        DrawText(line, 20, 20, 20, RAYWHITE);
    }

    DrawText("1/2/3 animation - P shading - arrows orbit - Q/E height - "
             "SPACE pause",
             20, SCREEN_H - 34, 18, (Color){ 200, 200, 210, 255 });
    DrawFPS(SCREEN_W - 90, 20);
}

/* ------------------------------------------------------------------ main -- */

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .width = SCREEN_W,
        .height = SCREEN_H,
        .title = "myecs -- skeletal animation and PBR",
    });
    if (world == NULL) {
        return 1;
    }

    ECS_COMPONENT_DEFINE(world, ShowcaseState);
    ecs_add_id(world, ecs_id(ShowcaseState), EcsSingleton);

    mye_input_bind_axis_keys(world, ACT_ORBIT, KEY_LEFT, KEY_RIGHT);
    mye_input_bind_axis_keys(world, ACT_ORBIT, KEY_A, KEY_D);
    mye_input_bind_axis_keys(world, ACT_ZOOM, KEY_DOWN, KEY_UP);
    mye_input_bind_axis_keys(world, ACT_ZOOM, KEY_S, KEY_W);
    mye_input_bind_axis_keys(world, ACT_HEIGHT, KEY_Q, KEY_E);
    mye_input_bind_key(world, ACT_ANIM_1, KEY_ONE);
    mye_input_bind_key(world, ACT_ANIM_2, KEY_TWO);
    mye_input_bind_key(world, ACT_ANIM_3, KEY_THREE);
    mye_input_bind_key(world, ACT_TOGGLE_PBR, KEY_P);
    mye_input_bind_key(world, ACT_PAUSE, KEY_SPACE);

    ecs_singleton_set(world, ShowcaseState,
                      { .orbit_angle = 0.9f,
                        .orbit_distance = 190.0f,
                        .camera_height = 70.0f });
    ShowcaseState *state = ecs_singleton_ensure(world, ShowcaseState);

    state->camera = mye_camera3d_spawn(world,
                                       (Vector3){ 120.0f, 70.0f, 120.0f },
                                       (Vector3){ 0.0f, 40.0f, 0.0f }, 50.0f);

    /* --- the assets ---------------------------------------------------- */
    char fox_path[1024];
    char boombox_path[1024];
    bool found = mye_asset_path("assets/models/Fox.glb", fox_path,
                                sizeof fox_path);
    found &= mye_asset_path("assets/models/BoomBox.glb", boombox_path,
                            sizeof boombox_path);

    mye_model fox = mye_model_load(world, fox_path);
    mye_model boombox = mye_model_load(world, boombox_path);
    state->assets_ok = mye_model_valid(world, fox) &&
                       mye_model_valid(world, boombox);

    if (!state->assets_ok) {
        /* Say which of the two it is. "Run the fetch script" is wrong advice
         * when the files are already there, and sends you in a circle. */
        if (!found) {
            mye_log_error("assets/models/*.glb not found from here or from "
                          "%s -- run tools/fetch_sample_assets.sh",
                          GetApplicationDirectory());
        } else {
            mye_log_error("found the .glb files but could not load them; "
                          "they may be truncated -- delete assets/models and "
                          "re-run tools/fetch_sample_assets.sh");
        }
    }

    if (state->assets_ok) {
        const ModelAnimation *animations =
            mye_model_animations(world, fox, &state->animation_count);
        mye_log_info("fox: %d animation cycles", state->animation_count);
        for (int i = 0; i < state->animation_count && animations != NULL; ++i) {
            mye_log_info("  [%d] %s (%d keyframes)", i, animations[i].name,
                         animations[i].keyframeCount);
        }

        state->fox = mye_mesh_spawn(world, fox,
                                    (Vector3){ -55.0f, 0.0f, 0.0f }, WHITE);
        ecs_set(world, state->fox, MyeModelAnimator,
                { .animation = 0, .speed = 30.0f, .loop = true,
                  .playing = true });

        /* BoomBox is authored in metres and is tiny; scale it up to sit
         * beside the fox. */
        state->boombox = mye_mesh_spawn(world, boombox,
                                        (Vector3){ 55.0f, 40.0f, 0.0f },
                                        WHITE);
        ecs_set(world, state->boombox, MyeScale3D,
                { { 900.0f, 900.0f, 900.0f } });
    }

    /* Bright key light: PBR highlights need something to reflect. */
    ecs_entity_t key_light = mye_entity_new(world);
    ecs_set(world, key_light, MyeLight,
            { .direction = { -0.5f, -0.8f, -0.4f },
              .color = (Color){ 255, 250, 240, 255 },
              .intensity = 2.2f,
              .enabled = true });

    ecs_entity_t rim_light = mye_entity_new(world);
    ecs_set(world, rim_light, MyeLight,
            { .direction = { 0.7f, -0.2f, 0.6f },
              .color = (Color){ 140, 170, 255, 255 },
              .intensity = 1.0f,
              .enabled = true });

    ECS_SYSTEM(world, SpinBoomBox, MyeOnFixedUpdate, MyeRotation3D);
    ECS_SYSTEM(world, CameraControl, EcsOnUpdate, ShowcaseState);
    ECS_SYSTEM(world, DrawHud, MyeOnDrawUI, [in] ShowcaseState);

    MyeRenderConfig *render_config = ecs_singleton_ensure(world,
                                                          MyeRenderConfig);
    render_config->clear_color = (Color){ 26, 28, 34, 255 };
    ecs_singleton_modified(world, MyeRenderConfig);

    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }

    return mye_shutdown(world);
}
