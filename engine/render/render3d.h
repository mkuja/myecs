/* 3D rendering. See plan/03-rendering.md.
 *
 * Draws in the MyeOnDraw3D phase, before the 2D sprite pass, so a 3D world
 * composes under a 2D HUD in the same frame. Like all rendering it runs on
 * the main thread only.
 *
 * Meshes are placed by MyeWorldTransform (scene/transform.h), so a 3D object
 * can be parented to another and inherit its movement.
 */
#ifndef MYE_RENDER_RENDER3D_H
#define MYE_RENDER_RENDER3D_H

#include "asset/asset.h"
#include "core/engine.h"
#include "scene/transform.h"

#include <raylib.h>

/* A model drawn at the entity's world transform. */
typedef struct MyeMeshInstance {
    mye_model model;
    Color tint;
} MyeMeshInstance;

/* The active 3D camera. The first entity with `active` set is used. */
typedef struct MyeCamera3D {
    Camera3D camera;
    bool active;
} MyeCamera3D;

/* A directional light. Several may exist; the shader takes the first
 * MYE_MAX_LIGHTS of them. */
typedef struct MyeLight {
    Vector3 direction; /* pointing *from* the light towards the scene */
    Color color;
    float intensity;
    bool enabled;
} MyeLight;

#define MYE_MAX_LIGHTS 4

/* Scene-wide 3D settings. */
typedef struct MyeRender3dConfig {
    Color ambient;   /* light present everywhere, so nothing is pure black */
    bool draw_grid;  /* debug ground grid */
    int grid_slices;
    float grid_spacing;
} MyeRender3dConfig;

extern ECS_COMPONENT_DECLARE(MyeMeshInstance);
extern ECS_COMPONENT_DECLARE(MyeCamera3D);
extern ECS_COMPONENT_DECLARE(MyeLight);
extern ECS_COMPONENT_DECLARE(MyeRender3dConfig);

void MyeRender3dModuleImport(ecs_world_t *world);

/* Spawns an entity with a mesh and the transform components it needs. */
ecs_entity_t mye_mesh_spawn(ecs_world_t *world, mye_model model,
                            Vector3 position, Color tint);

/* Spawns a camera looking at `target` from `position`, marked active. */
ecs_entity_t mye_camera3d_spawn(ecs_world_t *world, Vector3 position,
                                Vector3 target, float fov_degrees);

#endif /* MYE_RENDER_RENDER3D_H */
