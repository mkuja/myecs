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
 * WHEN CAMERA LOGIC RUNS. The MyeOnCamera phase (see core/engine.h) runs
 * after transforms are propagated and blended, so a system there that reads
 * another entity's drawn position -- mye_render_position -- gets this frame's
 * final, interpolated value. A follower in EcsOnUpdate would see the stepped
 * position instead, which mid-step is not where the sprite is drawn, and the
 * world would shimmer against a perfectly smooth player.
 *
 * The rule, precisely: in MyeOnCamera, writes to the camera's OWN position
 * and rotation take effect this frame, because the view is recomputed from
 * those components at draw time. Writes to anything else -- a pivot the
 * camera is parented to, a rig -- land next frame, because propagation has
 * already run. Move rigs in EcsOnUpdate; move cameras in MyeOnCamera.
 *
 * Write in place (ecs_ensure / ecs_get_mut, then ecs_modified) rather than
 * with ecs_set: a deferred write from a system lands at the next merge, and
 * with worker threads that is after the draw passes.
 *
 * TWO CAVEATS. A camera moved in MyeOnCamera has a stale world matrix until
 * the next propagation; nothing that draws is affected, but anything parented
 * *to* such a camera lags a frame. Parent cameras to things; do not parent
 * things to a following camera. And mye_camera_look_at on a camera whose
 * parent has not been propagated yet (spawned this frame, before the first
 * mye_progress) sees an identity parent -- aim after one frame, or aim the
 * parent instead.
 */
#ifndef MYE_RENDER_CAMERA_H
#define MYE_RENDER_CAMERA_H

#include "core/engine.h"
#include "scene/transform.h"

#include <raylib.h>

/* Follows another entity. The camera keeps its own position -- it is not a
 * child of the target -- so it can lag behind, which is what makes motion
 * readable instead of rigid. A following camera may itself be parented; the
 * target's world position is converted into the parent's space. */
typedef struct MyeCameraFollow {
    ecs_entity_t target;

    /* Where to sit relative to the target, in the TARGET's own space, so a
     * "behind the player" offset stays behind them as they turn. For a 2D
     * camera z is ignored. */
    Vector3 offset;

    /* If the target is deleted the camera simply stays put, silently: what
     * that should mean is the game's decision, made by writing the camera or
     * this component. The engine takes no stance. */

    /* How hard the camera is pulled toward where it should be, per second.
     * 0 snaps exactly. Around 8 is a gentle lag; 20 is nearly rigid. The
     * blend is framerate-independent (exponential decay), so it behaves the
     * same at 30 and 240 fps. */
    float stiffness;
} MyeCameraFollow;

extern ECS_COMPONENT_DECLARE(MyeCameraFollow);

/* How many active cameras one pass will draw. Split screen wants four, a
 * minimap one more; sixteen is far past any real layout and keeps the
 * per-frame array on the stack. */
#define MYE_MAX_DRAWN_CAMERAS 16

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

/* The lowest-order active camera -- the "main view" by convention, and what
 * the screen/world helpers below use. False when there is none: the 3D pass
 * then draws nothing and the 2D pass falls back to an identity view.
 * `out_entity` may be NULL.
 *
 * Note this is a convenience, not a selection: EVERY active camera is drawn
 * (see mye_camera3d_collect). How many cameras a game has, and what they are
 * for, is the game's business. */
bool mye_camera3d_active(const ecs_world_t *world, Camera3D *out,
                         ecs_entity_t *out_entity);
bool mye_camera2d_active(const ecs_world_t *world, Camera2D *out,
                         ecs_entity_t *out_entity);

/* Every active camera of one dimensionality, sorted by `order` (stable, so
 * equal orders keep entity order). Writes at most `max` and returns how many
 * -- if that clips, the extra cameras simply are not drawn, which is
 * reported once rather than silently.
 *
 * This is what the built-in passes iterate: there is no chosen camera, so a
 * minimap or a split screen is a second camera entity and nothing else. */
int mye_camera3d_collect(const ecs_world_t *world, ecs_entity_t *out, int max);
int mye_camera2d_collect(const ecs_world_t *world, ecs_entity_t *out, int max);

/* The camera whose viewport contains a screen point, or 0. Split-screen
 * picking needs this: a click in player two's half read against player one's
 * camera silently lands somewhere else in the world. */
ecs_entity_t mye_camera_at_screen(const ecs_world_t *world, Vector2 screen);

/* The rect a camera draws into, resolving "zero means the whole window". */
Rectangle mye_camera_viewport(const ecs_world_t *world, ecs_entity_t camera);

/* ---------------------------------------------------- drawing through one -- */

/* raylib's BeginMode2D/3D always use the whole window, so these do the same
 * job against a viewport rect: set the GL viewport and scissor, build the
 * projection with THAT rect's aspect, and apply the view. Ending restores
 * the full window -- forget that and the HUD inherits the last camera's
 * rect, which reads as a layout bug rather than a camera one.
 *
 * Main thread only, between BeginDrawing and EndDrawing, like all drawing. */
void mye_camera_begin_2d(Rectangle viewport, Camera2D camera);
void mye_camera_end_2d(void);
/* `clear` wipes colour and depth inside the viewport first. Every camera
 * after the first needs it: the previous camera's depth values would
 * otherwise reject this one's fragments entirely. */
void mye_camera_begin_3d(Rectangle viewport, Camera3D camera, bool clear);
void mye_camera_end_3d(void);

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
