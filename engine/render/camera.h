/* Cameras. See plan/03-rendering.md.
 *
 * A camera is an entity in the transform hierarchy, like everything else.
 * Where it is and which way it faces come from its MyePosition/MyeRotation
 * and its parents; the camera component only carries what a camera *is* --
 * field of view, projection, zoom.
 *
 * That one decision is what makes the two things games actually want fall
 * out of machinery that already exists:
 *
 *   rigid attachment   parent the camera to something.
 *                      mye_set_parent(world, cam, player);
 *                      It now turns and moves with the player, because the
 *                      transform hierarchy composes rotation, not just
 *                      position. A first-person view, a turret sight, a
 *                      camera on a rollercoaster: all just parenting.
 *
 *   follow with lag    add MyeCameraFollow. The camera stays independent
 *                      and moves itself toward the target each frame.
 *
 * Orientation is a rotation, not a "look at" point, because a target point
 * has no meaning once a camera is parented -- a target in whose space? For
 * the common case there is mye_camera_look_at(), which just writes the
 * rotation that points at a world position.
 *
 * WHEN CAMERA LOGIC RUNS. Anything that moves a camera belongs in the
 * MyeOnCamera phase (see core/engine.h). It runs after transforms are
 * propagated and blended, so a camera that reads a target's position gets
 * this frame's final, interpolated one. Put that logic in EcsOnUpdate
 * instead and the camera trails the picture by a frame -- which looks like
 * jitter, and is the kind of bug that gets blamed on the renderer.
 *
 * ONE CAVEAT. A camera moved by a follow system is moved *after* the frame's
 * transforms were computed, so its own world matrix is one frame behind
 * until the next propagation. Nothing that draws is affected -- the view is
 * recomputed from components, not from that matrix -- but anything parented
 * *to* such a camera lags by a frame. Parent cameras to things; do not parent
 * things to a following camera.
 */
#ifndef MYE_RENDER_CAMERA_H
#define MYE_RENDER_CAMERA_H

#include "core/engine.h"
#include "scene/transform.h"

#include <raylib.h>

/* Follows another entity. The camera keeps its own position -- it is not a
 * child of the target -- so it can lag behind, which is what makes motion
 * readable instead of rigid. */
typedef struct MyeCameraFollow {
    ecs_entity_t target;

    /* Where to sit relative to the target, in the TARGET's own space, so a
     * "behind the player" offset stays behind them as they turn. For a 2D
     * camera z is ignored. */
    Vector3 offset;

    /* How hard the camera is pulled toward where it should be, per second.
     * 0 snaps exactly. Around 8 is a gentle lag; 20 is nearly rigid. The
     * blend is framerate-independent (exponential decay), so it behaves the
     * same at 30 and 240 fps. */
    float stiffness;
} MyeCameraFollow;

extern ECS_COMPONENT_DECLARE(MyeCameraFollow);

void MyeCameraModuleImport(ecs_world_t *world);

/* ------------------------------------------------------------- resolving -- */

/* Builds the raylib camera an entity describes, from its drawn transform.
 * False if the entity has no camera component.
 *
 * Deliberately recomputed rather than cached: the renderer and the
 * screen/world helpers call this, so they cannot disagree, and a camera
 * moved this frame resolves to where it was moved -- not to last frame's
 * matrix. */
bool mye_camera3d_resolve(const ecs_world_t *world, ecs_entity_t camera,
                          Camera3D *out);
bool mye_camera2d_resolve(const ecs_world_t *world, ecs_entity_t camera,
                          Camera2D *out);

/* The first camera marked active. False when a scene has none: the 3D pass
 * then draws nothing, and the 2D pass falls back to an identity view.
 * `out_entity` may be NULL. */
bool mye_camera3d_active(const ecs_world_t *world, Camera3D *out,
                         ecs_entity_t *out_entity);
bool mye_camera2d_active(const ecs_world_t *world, Camera2D *out,
                         ecs_entity_t *out_entity);

/* --------------------------------------------------------------- spawning -- */

/* A camera at `position` looking at `look_at`, with the full transform set,
 * marked active. Parent it afterwards if you want it carried by something. */
ecs_entity_t mye_camera3d_spawn(ecs_world_t *world, Vector3 position,
                                Vector3 look_at, float fov_degrees);

/* A 2D camera centred on `position`. Its offset defaults to the middle of
 * the window, so the entity's position is what ends up centre-screen. */
ecs_entity_t mye_camera2d_spawn(ecs_world_t *world, Vector2 position,
                                float zoom);

/* ---------------------------------------------------------------- aiming -- */

/* Points the camera at a WORLD position by writing its rotation. The target
 * is converted into the camera's own space first, so this is correct for a
 * parented camera too -- which is the whole reason a target point is not
 * stored on the component. */
void mye_camera_look_at(ecs_world_t *world, ecs_entity_t camera,
                        Vector3 target);

/* Vertical field of view in degrees. Clamped to a sane range: a zero or
 * negative fov renders nothing, which looks like a crash. */
void mye_camera_set_fov(ecs_world_t *world, ecs_entity_t camera,
                        float degrees);
float mye_camera_get_fov(const ecs_world_t *world, ecs_entity_t camera);

/* ------------------------------------------------- screen and world space -- */

/* All of these use the active camera, so call them from MyeOnCamera or
 * later to get this frame's view. They work headless, using the configured
 * window size, so picking logic is testable without a window. */

/* Where a world point lands on screen. */
Vector2 mye_world_to_screen(const ecs_world_t *world, Vector3 point);
Vector2 mye_world_to_screen_2d(const ecs_world_t *world, Vector2 point);

/* The ray under a screen pixel -- what to intersect for click-to-select. */
Ray mye_screen_ray(const ecs_world_t *world, Vector2 screen);

/* The world point under a screen pixel, for a 2D camera. */
Vector2 mye_screen_to_world_2d(const ecs_world_t *world, Vector2 screen);

#endif /* MYE_RENDER_CAMERA_H */
