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

/* Plays one of a model's skeletal animations.
 *
 * IMPORTANT LIMITATION: raylib stores the animated pose *in the Model*, not
 * per instance, so two entities sharing one model handle cannot hold
 * different poses -- the last one to update wins. Give each animated entity
 * its own model handle until that changes. Static instances of the same model
 * are unaffected and still share freely. */
typedef struct MyeModelAnimator {
    int animation;   /* index into the model's animation array */
    float frame;     /* current frame; fractional -- raylib interpolates */
    float speed;     /* frames per second; 24 is a common glTF default */
    bool loop;
    bool playing;
} MyeModelAnimator;

/* A model drawn at the entity's world transform. */
typedef struct MyeMeshInstance {
    mye_model model;
    Color tint;
} MyeMeshInstance;

/* Marks an entity as a 3D camera. Where it is and which way it faces come
 * from the entity's transform, so a camera is placed and parented like
 * anything else -- see render/camera.h. The first one marked active is the
 * one that draws. */
typedef struct MyeCamera3D {
    float fov;       /* vertical, degrees; 0 is treated as 60 */
    int projection;  /* CAMERA_PERSPECTIVE or CAMERA_ORTHOGRAPHIC */
    bool active;

    /* Where on the window this camera draws, in pixels. A zero-sized rect
     * means the whole window -- what a game with one camera wants, and never
     * has to think about. A minimap is a corner rect. */
    Rectangle viewport;

    /* Draw order among cameras. Higher draws later, so a minimap at order 1
     * lands on top of a world view at order 0. Ties keep entity order. */
    int order;
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

    /* Use the physically-based shader rather than Blinn-Phong. PBR reads the
     * metallic/roughness/normal/emissive maps that glTF files actually carry,
     * so downloaded models look as their author intended. Blinn-Phong ignores
     * all of it and is cheaper. */
    bool use_pbr;

} MyeRender3dConfig;

extern ECS_COMPONENT_DECLARE(MyeMeshInstance);
extern ECS_COMPONENT_DECLARE(MyeModelAnimator);
extern ECS_COMPONENT_DECLARE(MyeCamera3D);
extern ECS_COMPONENT_DECLARE(MyeLight);
extern ECS_COMPONENT_DECLARE(MyeRender3dConfig);

void MyeRender3dModuleImport(ecs_world_t *world);

/* Spawns an entity with a mesh and the transform components it needs. */
ecs_entity_t mye_mesh_spawn(ecs_world_t *world, mye_model model,
                            Vector3 position, Color tint);

/* Cameras live in render/camera.h: mye_camera3d_spawn, mye_camera_look_at,
 * mye_camera_set_fov, MyeCameraFollow, and the screen/world helpers. */

#endif /* MYE_RENDER_RENDER3D_H */
