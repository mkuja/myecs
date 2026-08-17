/* 2D rendering. See plan/03-rendering.md.
 *
 * All drawing happens on the main thread, in the engine's draw phases (see
 * core/engine.h), which are single-threaded. The pass order is fixed:
 *
 *   MyeRenderBegin   BeginDrawing + clear
 *   MyeRenderSprites world-space sprites, sorted, inside BeginMode2D
 *   (game systems registered in MyeOnDrawUI draw screen-space overlays)
 *   MyeRenderEnd     EndDrawing
 *
 * Begin and end are separate systems querying a singleton so they run even
 * when nothing is on screen -- otherwise a frame with no sprites would end a
 * frame it never began.
 */
#ifndef MYE_RENDER_RENDER2D_H
#define MYE_RENDER_RENDER2D_H

#include "asset/asset.h"
#include "core/engine.h"
/* The 2D placement components live in the transform module: they belong to
 * the hierarchy, not to the renderer, and the renderer is only one of their
 * consumers. */
#include "scene/transform.h"

#include <raylib.h>

typedef struct MyeSprite {
    mye_texture texture;
    Rectangle source; /* atlas sub-rectangle; zero width = whole texture */
    Vector2 origin;   /* pivot in source pixels; rotation happens about this */
    Color tint;
    int16_t layer;    /* higher draws in front */
} MyeSprite;

/* Flipbook animation: steps MyeSprite.source through frames laid out in a
 * grid inside the texture. Added by a system in EcsPreStore, so the sprite a
 * frame draws is always the one the animation just selected. */
typedef struct MyeSpriteAnim {
    Rectangle first_frame; /* rect of frame 0 within the texture */
    int columns;           /* frames per row; frames wrap to the next row */
    int frame_count;
    float fps;
    float elapsed;         /* seconds spent on the current frame */
    int current;           /* frame index, 0-based */
    bool loop;
    bool playing;
    /* Set once the LAST frame has had its full display time -- not when it is
     * merely reached. Despawning on `finished` therefore always shows every
     * frame. */
    bool finished;
} MyeSpriteAnim;

/* Opt-in smoothing between fixed simulation steps.
 *
 * The simulation advances in fixed 1/60 s jumps while the display may refresh
 * at any rate, so most drawn frames fall *between* two simulated states.
 * Adding this component makes the renderer blend between the previous and
 * current position using MyeTime.alpha, which removes the judder that would
 * otherwise show on a high-refresh display.
 *
 * Deliberately opt-in per entity. With it on by default an entity's rendered
 * position would silently stop matching its MyePosition2D value, and someone
 * comparing the component in the flecs Explorer against the screen would find
 * a discrepancy with no visible cause. The engine should not diverge data
 * from pixels behind your back.
 *
 * Composes through the transform hierarchy: an entity with a parent blends
 * its own local offset, and the chain is multiplied together into
 * MyeRenderTransform at draw time, so a child stays rigidly attached to an
 * interpolated parent. Blending each link separately is what keeps the rate
 * right -- prev_x/prev_y are one fixed STEP back, while transforms compose
 * once per FRAME, and a frame can run several steps or none.
 *
 * 2D only: a 3D entity is drawn at its un-blended world transform.
 *
 * The previous position is maintained by the engine; do not write it. Call
 * mye_transform_snap() after teleporting an entity, or the blend will smear
 * it across the screen -- a screen wrap from x=1270 to x=10 draws a streak
 * through everything in between. */
typedef struct MyeInterpolate {
    /* Engine-maintained: position at the start of the last fixed step. */
    float prev_x, prev_y;
    /* Set by mye_transform_snap(); consumed and cleared by the next step. */
    bool snap;
} MyeInterpolate;

/* Tag: skip this entity when drawing. */
typedef struct MyeHidden {
    char unused;
} MyeHidden;

/* Marks an entity as a 2D camera. Its position comes from the entity's
 * transform, so it can be parented or driven by MyeCameraFollow -- see
 * render/camera.h. The first one marked active is the one that draws. */
typedef struct MyeCamera2D {
    float zoom;      /* 1 = one world unit per pixel; 0 is treated as 1 */
    Vector2 offset;  /* where in its viewport the camera's position lands;
                        mye_camera2d_spawn defaults it to the centre */
    bool active;

    /* As MyeCamera3D: a zero-sized rect means the whole window, and higher
     * order draws later. */
    Rectangle viewport;
    int order;
} MyeCamera2D;
/* No rotation field: the view turns with the entity's MyeRotation2D and its
 * parents, like everything else. */

/* Screen clear colour and other per-scene render settings. */
typedef struct MyeRenderConfig {
    Color clear_color;

} MyeRenderConfig;

extern ECS_COMPONENT_DECLARE(MyeSprite);
extern ECS_COMPONENT_DECLARE(MyeSpriteAnim);
extern ECS_COMPONENT_DECLARE(MyeInterpolate);
extern ECS_COMPONENT_DECLARE(MyeHidden);
extern ECS_COMPONENT_DECLARE(MyeCamera2D);
extern ECS_COMPONENT_DECLARE(MyeRenderConfig);

void MyeRender2dModuleImport(ecs_world_t *world);

/* Suppresses interpolation for this entity on the next frame. Call it
 * whenever you move an entity discontinuously -- teleports, respawns, screen
 * wraps -- so the renderer does not draw the journey. No-op on entities that
 * are not interpolated. */
void mye_transform_snap(ecs_world_t *world, ecs_entity_t entity);

/* Rect of `index` within a grid whose frame 0 is `first_frame`. Pure: the
 * animation systems are built on this, and so are the tests. */
Rectangle mye_atlas_frame(Rectangle first_frame, int columns, int index);

/* Advances by `dt` and returns true if the frame changed. Pure state machine
 * -- no world, no rendering -- so it is unit testable headlessly. */
bool mye_sprite_anim_advance(MyeSpriteAnim *anim, float dt);

/* Restarts from frame 0. */
void mye_sprite_anim_restart(MyeSpriteAnim *anim);

/* Convenience: spawn a sprite entity at a position. */
ecs_entity_t mye_sprite_spawn(ecs_world_t *world, mye_texture texture, float x,
                              float y, Color tint);

#endif /* MYE_RENDER_RENDER2D_H */
