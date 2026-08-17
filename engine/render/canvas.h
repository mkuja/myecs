/* Canvases: off-screen surfaces that cameras render into. See
 * plan/14-canvases.md.
 *
 * A canvas is an entity wrapping a render texture. Cameras render INTO it by
 * setting MyeCamera2D/3D.target; anything draws WITH it, because its result
 * is an ordinary texture handle:
 *
 *   ecs_entity_t map = mye_canvas_create(world, "minimap", 256, 256, BLACK);
 *   MyeCamera3D *cam = ecs_get_mut(world, overhead, MyeCamera3D);
 *   cam->target = map;                      // renders into the canvas
 *   ecs_modified(world, overhead, MyeCamera3D);
 *
 *   // ...and somewhere in a HUD system:
 *   DrawTextureRec(*mye_texture_get(world, mye_canvas_texture(world, map)),
 *                  mye_canvas_source_rect(world, map),
 *                  (Vector2){ 20, 20 }, WHITE);
 *
 * That is a picture-in-picture, a security monitor, a portal, or a minimap
 * on a car's dashboard, depending only on what displays it. The window is
 * simply the canvas a camera gets when its target is 0.
 *
 * ORDER. Canvases render in the MyeOnDrawCanvases phase, before the window's
 * passes, so whatever displays a canvas shows this frame's contents. One
 * level only: if a canvas contains something displaying ANOTHER canvas, the
 * inner one shows its previous frame. Sorting canvases by what they display
 * is a later milestone; nothing needs it yet.
 *
 * UPSIDE DOWN. raylib stores render textures bottom-up, so drawing one with
 * a plain source rect shows it mirrored vertically. mye_canvas_source_rect()
 * returns the rect that draws it the right way up; use it. On a mesh there
 * is no source rect, so a canvas applied as a mesh texture appears flipped
 * unless the mesh's UVs account for it.
 */
#ifndef MYE_RENDER_CANVAS_H
#define MYE_RENDER_CANVAS_H

#include "asset/asset.h"
#include "core/engine.h"

#include <raylib.h>

typedef struct MyeCanvas {
    int width, height;

    Color clear_color;
    /* False accumulates frame over frame -- trails, painting. */
    bool clear;
    /* False leaves the texture holding whatever it last contained, and costs
     * nothing to render. */
    bool active;

    /* Engine-maintained; do not write. */
    RenderTexture2D target;
    mye_texture texture;
} MyeCanvas;

extern ECS_COMPONENT_DECLARE(MyeCanvas);

void MyeCanvasModuleImport(ecs_world_t *world);

/* Creates a canvas of the given size. The name is the key its texture takes
 * in the asset registry, and it must not already be taken -- by another
 * canvas or by a loaded texture. A taken name is refused with a logged error
 * and a 0 return, rather than quietly sharing the other texture: that would
 * display an unrelated picture and confuse the two lifetimes.
 *
 * Returns 0 on failure. Check it: passing 0 on to the ECS is itself an
 * abort.
 *
 * Headless worlds get a canvas entity with no GPU object, so game logic
 * around canvases stays testable without a window -- the same rule the asset
 * registry follows. */
ecs_entity_t mye_canvas_create(ecs_world_t *world, const char *name,
                               int width, int height, Color clear_color);

/* Frees the GPU object and the entity. Cameras still targeting it fall back
 * to the window, with a warning. */
void mye_canvas_destroy(ecs_world_t *world, ecs_entity_t canvas);

/* The canvas's colour attachment as a texture handle: usable in
 * MyeSprite.texture, MyeMeshInstance.texture, or a HUD draw. Stable for the
 * canvas's lifetime. Zero handle if `canvas` is not one. */
mye_texture mye_canvas_texture(const ecs_world_t *world, ecs_entity_t canvas);

/* True when `texture` is the canvas `target`'s own surface -- i.e. drawing
 * with it while rendering into `target` would sample the framebuffer being
 * written. The draw passes use this to skip such a surface rather than let
 * GL produce undefined pixels (and, on WebGL, an error per draw call).
 * `target` of 0 (the window) is never a match. */
bool mye_canvas_is_own_texture(const ecs_world_t *world, ecs_entity_t target,
                               mye_texture texture);

/* Two things a canvas will not do for you, both harmless and neither hidden:
 *
 *   displaying ITSELF -- a sprite or mesh inside a canvas showing that same
 *   canvas -- would sample the texture attached to the framebuffer being
 *   drawn into. GL calls that a feedback loop and leaves the pixels
 *   undefined; WebGL additionally logs an error for every such draw call.
 *   The passes therefore SKIP that surface while rendering into the canvas
 *   it displays: a monitor showing the camera feed it is standing in front
 *   of is simply absent from its own picture. It draws normally everywhere
 *   else. This is the one case the engine decides for you, because the
 *   alternative is not a different picture but an undefined one.
 *
 *   displaying ANOTHER canvas works, but only one level deep and one frame
 *   late if the other canvas happens to render afterwards. Which order two
 *   canvases render in is table order, which is not something to build on. */

/* The source rect that draws the canvas the right way up: (0, 0, w, -h).
 * See the note about render textures being stored bottom-up. */
Rectangle mye_canvas_source_rect(const ecs_world_t *world, ecs_entity_t canvas);

#endif /* MYE_RENDER_CANVAS_H */
