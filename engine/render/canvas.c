/* Canvas rendering. See canvas.h for the design. */
#include "render/canvas.h"

#include "core/log.h"
#include "render/camera.h"
#include "render/render2d.h"
#include "render/render3d.h"

#include <rlgl.h>

ECS_COMPONENT_DECLARE(MyeCanvas);

/* ------------------------------------------------------------ lifecycle -- */

ecs_entity_t mye_canvas_create(ecs_world_t *world, const char *name,
                               int width, int height, Color clear_color)
{
    if (name == NULL || width <= 0 || height <= 0) {
        mye_log_error("canvas: a canvas needs a name and a positive size");
        return 0;
    }

    const mye_engine *engine = mye_engine_get(world);
    bool headless = engine == NULL || engine->headless;

    RenderTexture2D target = { 0 };
    mye_texture handle = { 0 };
    if (!headless) {
        target = LoadRenderTexture(width, height);
        if (target.id == 0) {
            mye_log_error("canvas: could not create a %dx%d render texture",
                          width, height);
            return 0;
        }
        /* Adopted, not owned: UnloadRenderTexture frees this colour
         * attachment, and unloading it twice corrupts an unrelated texture
         * later. */
        handle = mye_texture_adopt(world, name, target.texture);
        if (handle.generation == 0) {
            /* The name was taken, or the registry is full -- adopt has said
             * which. Refusing here beats returning a canvas whose texture
             * handle resolves to somebody else's pixels: that draws a
             * perfectly plausible wrong picture. */
            UnloadRenderTexture(target);
            return 0;
        }
    }

    ecs_entity_t e = mye_entity_new(world);
    ecs_set_name(world, e, name);
    ecs_set(world, e, MyeCanvas,
            { .width = width, .height = height, .clear_color = clear_color,
              .clear = true, .active = true, .target = target,
              .texture = handle });
    return e;
}

void mye_canvas_destroy(ecs_world_t *world, ecs_entity_t canvas)
{
    if (canvas != 0 && ecs_is_alive(world, canvas)) {
        ecs_delete(world, canvas); /* the OnRemove observer frees the GPU side */
    }
}

/* Frees the render texture when the component goes away -- by explicit
 * destroy, by scene unload, or at world shutdown. The registry handle is
 * released first, and because it was adopted rather than owned, that does
 * not touch the GL texture; UnloadRenderTexture does. */
static void MyeCanvasRemoved(ecs_iter_t *it)
{
    MyeCanvas *canvas = ecs_field(it, MyeCanvas, 0);
    for (int i = 0; i < it->count; ++i) {
        if (canvas[i].texture.generation != 0) {
            mye_texture_release(it->world, canvas[i].texture);
            canvas[i].texture = (mye_texture){ 0 };
        }
        if (canvas[i].target.id != 0) {
            UnloadRenderTexture(canvas[i].target);
            canvas[i].target = (RenderTexture2D){ 0 };
        }
    }
}

/* --------------------------------------------------------------- access -- */

/* ecs_get aborts inside flecs on a dead entity, so every public lookup here
 * checks first: a canvas that has been destroyed is an ordinary thing for a
 * game to still be holding onto, not a reason to take the process down. */
static const MyeCanvas *canvas_get(const ecs_world_t *world,
                                   ecs_entity_t canvas)
{
    if (canvas == 0 || !ecs_is_alive(world, canvas)) {
        return NULL;
    }
    return ecs_get(world, canvas, MyeCanvas);
}

mye_texture mye_canvas_texture(const ecs_world_t *world, ecs_entity_t canvas)
{
    const MyeCanvas *c = canvas_get(world, canvas);
    return c != NULL ? c->texture : (mye_texture){ 0 };
}

bool mye_canvas_is_own_texture(const ecs_world_t *world, ecs_entity_t target,
                               mye_texture texture)
{
    if (target == 0 || texture.generation == 0) {
        return false; /* the window, or nothing to draw with */
    }
    const MyeCanvas *c = canvas_get(world, target);
    return c != NULL && c->texture.index == texture.index &&
           c->texture.generation == texture.generation;
}

Rectangle mye_canvas_source_rect(const ecs_world_t *world, ecs_entity_t canvas)
{
    const MyeCanvas *c = canvas_get(world, canvas);
    if (c == NULL) {
        return (Rectangle){ 0 };
    }
    /* Negative height flips it: render textures are stored bottom-up. */
    return (Rectangle){ 0.0f, 0.0f, (float)c->width, -(float)c->height };
}

/* -------------------------------------------------------------- drawing -- */

/* Renders every active canvas, before the window's passes. Each canvas is
 * self-contained: its own framebuffer, its own clear, its own depth. */
static void MyeCanvasDraw(ecs_iter_t *it)
{
    MyeCanvas *canvases = ecs_field(it, MyeCanvas, 0);
    ecs_world_t *world = it->world;

    for (int i = 0; i < it->count; ++i) {
        if (!canvases[i].active || canvases[i].target.id == 0) {
            continue;
        }
        ecs_entity_t canvas = it->entities[i];

        BeginTextureMode(canvases[i].target);
        if (canvases[i].clear) {
            ClearBackground(canvases[i].clear_color);
        }
        /* 3D first, then sprites over it -- the same order the window uses,
         * so a canvas looks like a small window. */
        mye_render3d_draw_cameras_for(world, canvas, canvases[i].clear);
        mye_render2d_draw_cameras_for(world, canvas, false);
        EndTextureMode();

        /* raylib is asymmetric here: BeginTextureMode tells rlgl the
         * framebuffer is now the canvas, and EndTextureMode restores the GL
         * viewport but NOT that recorded size (rcore.c: rlSetFramebufferWidth
         * is called in Begin, never in End). Every viewport computed after
         * this -- every window camera, every frame -- would then be flipped
         * against the canvas's height instead of the window's, sliding the
         * whole window's output down the screen by the difference. Restore it
         * ourselves; render size, not screen size, so HiDPI still works. */
        rlSetFramebufferWidth(GetRenderWidth());
        rlSetFramebufferHeight(GetRenderHeight());
    }
}

/* ------------------------------------------------------------- lifecycle -- */

void MyeCanvasModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeCanvasModule);

    /* The draw systems below call into both renderers, and cameras carry the
     * target field this module reads. Import order is not left to chance. */
    ECS_IMPORT(world, MyeRender2dModule);
    ECS_IMPORT(world, MyeRender3dModule);
    ECS_IMPORT(world, MyeCameraModule);

    ECS_COMPONENT_DEFINE(world, MyeCanvas);

    ecs_observer(world, {
        .query.terms = {{ .id = ecs_id(MyeCanvas) }},
        .events = { EcsOnRemove },
        .callback = MyeCanvasRemoved,
    });

    const mye_engine *engine = mye_engine_get(world);
    if (engine == NULL || engine->headless) {
        return; /* no framebuffers to render into */
    }

    ECS_SYSTEM(world, MyeCanvasDraw, MyeOnDrawCanvases, MyeCanvas);
}
