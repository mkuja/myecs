/* 2D rendering. See plan/03-rendering.md.
 *
 * All drawing happens on the main thread inside the EcsOnStore phase, which
 * the engine keeps single-threaded. The pass order is fixed:
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

#include <raylib.h>

/* World-space position. Shared by 2D gameplay and rendering. */
typedef struct MyePosition2D {
    float x, y;
} MyePosition2D;

/* Radians, clockwise, 0 = facing right. */
typedef struct MyeRotation2D {
    float angle;
} MyeRotation2D;

typedef struct MyeScale2D {
    float x, y;
} MyeScale2D;

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

/* Tag: skip this entity when drawing. */
typedef struct MyeHidden {
    char unused;
} MyeHidden;

/* The active 2D camera. The engine draws with the first entity that has one
 * marked active. */
typedef struct MyeCamera2D {
    Camera2D camera;
    bool active;
} MyeCamera2D;

/* Screen clear colour and other per-scene render settings. */
typedef struct MyeRenderConfig {
    Color clear_color;
} MyeRenderConfig;

extern ECS_COMPONENT_DECLARE(MyePosition2D);
extern ECS_COMPONENT_DECLARE(MyeRotation2D);
extern ECS_COMPONENT_DECLARE(MyeScale2D);
extern ECS_COMPONENT_DECLARE(MyeSprite);
extern ECS_COMPONENT_DECLARE(MyeSpriteAnim);
extern ECS_COMPONENT_DECLARE(MyeHidden);
extern ECS_COMPONENT_DECLARE(MyeCamera2D);
extern ECS_COMPONENT_DECLARE(MyeRenderConfig);

void MyeRender2dModuleImport(ecs_world_t *world);

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
