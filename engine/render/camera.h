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

/* The lowest-order active camera RENDERING TO THE WINDOW -- the "main view"
 * by convention, and what the screen/world helpers below use. A camera that
 * renders into a canvas is never "the main view", however low its order:
 * picking against a minimap's camera would silently give answers in the
 * wrong space. False when there is none: the 3D pass
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

/* The same, for cameras rendering into a particular canvas. `target` of 0
 * means the window, so the two calls above are these with 0. A camera whose
 * target is not a live canvas is counted as a window camera, and warns
 * once -- that is malformed data rather than a choice. */
int mye_camera3d_collect_for(const ecs_world_t *world, ecs_entity_t target,
                             ecs_entity_t *out, int max);
int mye_camera2d_collect_for(const ecs_world_t *world, ecs_entity_t target,
                             ecs_entity_t *out, int max);

/* The camera whose viewport contains a screen point, or 0. Split-screen
 * picking needs this: a click in player two's half read against player one's
 * camera silently lands somewhere else in the world. The screen→world helpers
 * below use it, so ordinary picking is already right; call it directly when
 * the game needs to know WHICH camera, not just what is under the cursor.
 *
 * Topmost first: with two cameras over the same pixel the one drawn last is
 * the one the player thinks they clicked on. 2D cameras are considered before
 * 3D ones, since a 2D camera over a 3D world is the HUD case. */
ecs_entity_t mye_camera_at_screen(const ecs_world_t *world, Vector2 screen);

/* Whether this camera draws this entity, by the layer rule both built-in
 * passes apply: a camera whose `layers` is 0 sees every layer; otherwise the
 * entity is drawn when any bit of its MyeVisibilityLayers mask matches. An
 * entity with no MyeVisibilityLayers is on every layer, so every camera sees
 * it -- see render2d.h for why absence is generous rather than exclusive.
 *
 * Public because a game that draws in a system of its own should be able to
 * obey the same rule the engine does instead of reinventing a near-miss of
 * it, and because it makes the rule checkable without a window. */
bool mye_camera_sees(const ecs_world_t *world, ecs_entity_t camera,
                     ecs_entity_t entity);

/* The rect a camera draws into, resolving "zero means the whole window". */
Rectangle mye_camera_viewport(const ecs_world_t *world, ecs_entity_t camera);

/* The pixel size of what a camera renders onto: a canvas, or the window at
 * its current size.
 *
 * This is passed explicitly to the begin functions rather than asked of rlgl,
 * because rlgl's framebuffer size is written by raylib's BeginTextureMode and
 * by nothing else -- not by EndTextureMode, not by a window resize. Reading it
 * would leave the window's viewport flipped against a stale height after
 * either, sliding everything the game draws off its true position. */
typedef struct MyeSurface { int width, height; } MyeSurface;
MyeSurface mye_camera_surface(const ecs_world_t *world, ecs_entity_t target);

/* ---------------------------------------------------- drawing through one -- */

/* raylib's BeginMode2D/3D always use the whole window, so these do the same
 * job against a viewport rect: set the GL viewport and scissor, build the
 * projection with THAT rect's aspect, and apply the view. Ending restores
 * the full window -- forget that and the HUD inherits the last camera's
 * rect, which reads as a layout bug rather than a camera one.
 *
 * Main thread only, between BeginDrawing and EndDrawing, like all drawing. */
void mye_camera_begin_2d(Rectangle viewport, MyeSurface surface,
                         Camera2D camera);
void mye_camera_end_2d(void);
/* What a 3D camera wipes inside its viewport before drawing.
 *
 * Depth is not optional for any camera but the first: the previous camera's
 * depth values are still there and silently reject this one's fragments --
 * a view that renders nothing, with no error and no clue.
 *
 * Colour is separable, and has to be, because a canvas with `clear = false`
 * accumulates: it wants this frame's meshes composited over last frame's
 * picture, which means a fresh depth buffer and an untouched colour one. */
typedef enum MyeCameraClear {
    MYE_CAMERA_CLEAR_NONE,  /* the target was just cleared by its owner */
    MYE_CAMERA_CLEAR_DEPTH, /* keep the colour already there, reset depth */
    MYE_CAMERA_CLEAR_ALL,
} MyeCameraClear;

/* `near_plane` / `far_plane` are MyeCamera3D's, in world units; 0 means
 * raylib's global defaults, and a far plane at or below the near one falls
 * back to them rather than build a projection that renders nothing. */
void mye_camera_begin_3d(Rectangle viewport, MyeSurface surface,
                         Camera3D camera, MyeCameraClear clear,
                         float near_plane, float far_plane);
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

/* Call these from MyeOnCamera or later to get this frame's view. They work
 * headless, using the configured window size, so picking logic is testable
 * without a window.
 *
 * WHICH CAMERA. Going screen→world, the POINT decides: the helper picks the
 * camera whose viewport the pixel is in (mye_camera_at_screen), because a
 * click in player two's half read through player one's camera lands somewhere
 * else in the world entirely -- invisible until two players are on screen.
 * Going world→screen there is no point to ask, so the active camera answers.
 * A pixel inside no viewport at all falls back to the active camera, which is
 * the only answer available and is exactly what a single-camera game gets.
 *
 * Every one of them accounts for the camera's viewport rect: a sub-viewport
 * shifts the pixel by the rect's origin and projects with the rect's aspect,
 * so screen coordinates are always window coordinates, whichever camera
 * answered. With one full-window camera all of that is the identity, which is
 * the code path a single-camera game has always taken. */

/* Where a world point lands on screen. */
Vector2 mye_world_to_screen(const ecs_world_t *world, Vector3 point);
Vector2 mye_world_to_screen_2d(const ecs_world_t *world, Vector2 point);

/* The ray under a screen pixel -- what to intersect for click-to-select. */
Ray mye_screen_ray(const ecs_world_t *world, Vector2 screen);

/* The world point under a screen pixel, for a 2D camera. */
Vector2 mye_screen_to_world_2d(const ecs_world_t *world, Vector2 screen);

/* The explicit-camera forms, so a game with several cameras is not forced
 * through "the active one" -- projecting a waypoint into the minimap, or
 * casting a ray from a pixel of a view the cursor is not in.
 *
 * `screen` is in the coordinates of the surface that camera draws on: window
 * pixels for a window camera, canvas pixels for one whose target is a canvas
 * (where a window mouse position means nothing until the game maps it back).
 * The camera's viewport offset and aspect are accounted for, so a ray cast
 * from a minimap pixel is a ray in the minimap's view and not in the main
 * one's. Zero if `cam` is not a 3D camera. */
Vector2 mye_world_to_screen_for(const ecs_world_t *world, ecs_entity_t cam,
                                Vector3 point);
Ray mye_screen_ray_for(const ecs_world_t *world, ecs_entity_t cam,
                       Vector2 screen);

#endif /* MYE_RENDER_CAMERA_H */
