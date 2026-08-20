#include "render/text.h"

#include "render/camera.h"
#include "render/render2d.h"
#include "scene/transform.h"

#include <raymath.h>

#include <stdlib.h>
#include <string.h>

ECS_COMPONENT_DECLARE(MyeText);

/* --------------------------------------------------------- string owning -- */

/* The allocator the string copies come from. Boxed, because a flecs type hook
 * is handed a type_info and nothing else -- no world, so no way to ask the
 * world for its allocator. hooks.ctx is the slot flecs provides for exactly
 * this, and it is per-component-per-world, so two worlds cannot end up
 * sharing one. */
typedef struct text_hook_ctx {
    mye_allocator allocator;
} text_hook_ctx;

static mye_allocator hook_allocator(const ecs_type_info_t *type_info)
{
    const text_hook_ctx *ctx =
        type_info != NULL ? (const text_hook_ctx *)type_info->hooks.ctx : NULL;
    /* The fallback is unreachable while the module registered the hooks, and
     * is here so a hook can never dereference nothing. */
    return ctx != NULL ? ctx->allocator : mye_heap_allocator();
}

static char *text_dup(mye_allocator a, const char *text)
{
    if (text == NULL) {
        return NULL;
    }
    size_t bytes = strlen(text) + 1;
    char *copy = (char *)mye_alloc(a, bytes, 1);
    if (copy != NULL) {
        memcpy(copy, text, bytes);
    }
    return copy;
}

static void text_release(mye_allocator a, const char *text)
{
    if (text == NULL) {
        return;
    }
    /* The copy is exactly strlen+1 bytes, which is the size the allocator
     * interface wants handed back (see core/alloc.h). const is cast off here
     * and only here: the component publishes the string read-only, but this
     * module allocated it. */
    mye_free(a, (void *)(uintptr_t)text, strlen(text) + 1);
}

static void text_ctor(void *ptr, int32_t count,
                      const ecs_type_info_t *type_info)
{
    (void)type_info;
    memset(ptr, 0, (size_t)count * sizeof(MyeText));
}

static void text_dtor(void *ptr, int32_t count,
                      const ecs_type_info_t *type_info)
{
    mye_allocator a = hook_allocator(type_info);
    MyeText *text = (MyeText *)ptr;
    for (int32_t i = 0; i < count; ++i) {
        text_release(a, text[i].text);
        text[i].text = NULL;
    }
}

/* Assignment: every ecs_set of a MyeText that an entity already has lands
 * here, with the old value still intact in `dst`. */
static void text_copy(void *dst_ptr, const void *src_ptr, int32_t count,
                      const ecs_type_info_t *type_info)
{
    mye_allocator a = hook_allocator(type_info);
    MyeText *dst = (MyeText *)dst_ptr;
    const MyeText *src = (const MyeText *)src_ptr;

    for (int32_t i = 0; i < count; ++i) {
        const char *previous = dst[i].text;
        dst[i] = src[i];
        dst[i].text = text_dup(a, src[i].text);
        /* Freed after the copy, not before: setting an entity's text to the
         * string it already holds -- mye_text_set(e, ecs_get(e)->text), or a
         * component re-set that carried the old pointer through -- would
         * otherwise read memory it had just freed. */
        text_release(a, previous);
    }
}

/* Moves happen when an entity changes table and when a deferred set is
 * flushed. The source must be left holding nothing, or its destructor frees
 * the string the destination now owns. */
static void text_move(void *dst_ptr, void *src_ptr, int32_t count,
                      const ecs_type_info_t *type_info)
{
    mye_allocator a = hook_allocator(type_info);
    MyeText *dst = (MyeText *)dst_ptr;
    MyeText *src = (MyeText *)src_ptr;

    for (int32_t i = 0; i < count; ++i) {
        if (&dst[i] == &src[i]) {
            continue;
        }
        const char *previous = dst[i].text;
        dst[i] = src[i];
        src[i].text = NULL;
        text_release(a, previous);
    }
}

/* Called when flecs tears the type info down, which happens inside ecs_fini
 * and therefore while the engine allocator is still alive. Frees itself with
 * the allocator it carries. */
static void text_hook_ctx_free(void *ptr)
{
    text_hook_ctx *ctx = (text_hook_ctx *)ptr;
    if (ctx == NULL) {
        return;
    }
    mye_allocator a = ctx->allocator;
    MYE_DELETE(a, ctx);
}

/* ------------------------------------------------------------- draw list -- */

typedef struct text_item {
    const char *text;
    const Font *font;
    Vector2 position;
    float size;
    float spacing;
    Color color;
    int32_t layer;
} text_item;

/* Layer first, then position, so the order is the same every frame rather
 * than whatever order the tables happened to be iterated in. */
static int compare_text_items(const void *lhs, const void *rhs)
{
    const text_item *a = (const text_item *)lhs;
    const text_item *b = (const text_item *)rhs;

    if (a->layer != b->layer) {
        return a->layer < b->layer ? -1 : 1;
    }
    if (a->position.y != b->position.y) {
        return a->position.y < b->position.y ? -1 : 1;
    }
    if (a->position.x != b->position.x) {
        return a->position.x < b->position.x ? -1 : 1;
    }
    return 0;
}

typedef struct MyeTextState {
    ecs_query_t *texts;
} MyeTextState;

ECS_COMPONENT_DECLARE(MyeTextState);

/* Builds this frame's list of one coordinate space's text, sorted, in the
 * frame arena -- nothing to free, and no per-frame malloc. Returns the count
 * and writes the array to `out`. */
static int32_t collect_text(ecs_world_t *world, bool world_space,
                            text_item **out)
{
    *out = NULL;

    const MyeTextState *state = ecs_singleton_get(world, MyeTextState);
    if (state == NULL || state->texts == NULL) {
        return 0;
    }

    int32_t total = 0;
    ecs_iter_t count_it = ecs_query_iter(world, state->texts);
    while (ecs_query_next(&count_it)) {
        total += count_it.count;
    }
    if (total == 0) {
        return 0;
    }

    mye_allocator frame = mye_frame_allocator(world);
    text_item *items = MYE_NEW_ARRAY(frame, text_item, (size_t)total);
    if (items == NULL) {
        /* Frame arena exhausted: skip the pass rather than draw garbage.
         * Raising mye_config.frame_arena_bytes is the fix. */
        return 0;
    }

    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    float alpha = time != NULL ? time->alpha : 0.0f;

    int32_t count = 0;
    ecs_iter_t iter = ecs_query_iter(world, state->texts);
    while (ecs_query_next(&iter)) {
        const MyeText *texts = ecs_field(&iter, MyeText, 0);
        const MyePosition2D *positions = ecs_field(&iter, MyePosition2D, 1);
        const MyeInterpolate *interp = ecs_field(&iter, MyeInterpolate, 2);
        const MyeRenderTransform *render_tf =
            ecs_field(&iter, MyeRenderTransform, 3);

        for (int i = 0; i < iter.count && count < total; ++i) {
            if (texts[i].world_space != world_space) {
                continue; /* the other pass draws this one */
            }
            if (texts[i].text == NULL || texts[i].text[0] == '\0') {
                continue;
            }

            const Font *font =
                mye_font_get_or_placeholder(world, texts[i].font);
            if (font == NULL || font->baseSize <= 0) {
                continue; /* headless, or no default font to fall back to */
            }

            /* Same precedence as the sprite pass: the composed hierarchy
             * transform if there is one, else this entity's own blend, else
             * the plain position. */
            float x = positions[i].x;
            float y = positions[i].y;
            if (render_tf != NULL) {
                Vector3 p = mye_matrix_translation(render_tf[i].m);
                x = p.x;
                y = p.y;
            } else if (interp != NULL && !interp[i].snap) {
                x = interp[i].prev_x + (positions[i].x - interp[i].prev_x) * alpha;
                y = interp[i].prev_y + (positions[i].y - interp[i].prev_y) * alpha;
            }

            items[count++] = (text_item){
                .text = texts[i].text,
                .font = font,
                .position = { x, y },
                .size = texts[i].size > 0.0f ? texts[i].size
                                             : (float)font->baseSize,
                .spacing = texts[i].spacing,
                .color = texts[i].color,
                .layer = texts[i].layer,
            };
        }
    }

    if (count == 0) {
        return 0;
    }

    qsort(items, (size_t)count, sizeof *items, compare_text_items);
    *out = items;
    return count;
}

static void draw_items(const text_item *items, int32_t count)
{
    for (int32_t i = 0; i < count; ++i) {
        DrawTextEx(*items[i].font, items[i].text, items[i].position,
                   items[i].size, items[i].spacing, items[i].color);
    }
}

/* ---------------------------------------------------------------- passes -- */

/* Screen space, in MyeOnDrawUI: after the 3D and sprite passes, before
 * EndDrawing, and outside any BeginMode2D -- so a position is a pixel. */
static void MyeDrawTextUI(ecs_iter_t *it)
{
    (void)ecs_field(it, MyeRenderConfig, 0);

    text_item *items = NULL;
    int32_t count = collect_text(it->world, false, &items);
    draw_items(items, count);
}

/* World space, in MyeOnDraw2D. Runs after MyeRenderSprites -- the render2d
 * module imports first, so its system entity is older and flecs orders it
 * first within the phase -- which is why world-space text composites over the
 * sprites. Stated in text.h rather than left to be discovered. */
static void MyeDrawTextWorld(ecs_iter_t *it)
{
    (void)ecs_field(it, MyeRenderConfig, 0);
    ecs_world_t *world = it->world;

    text_item *items = NULL;
    int32_t count = collect_text(world, true, &items);
    if (count == 0) {
        return;
    }

    ecs_entity_t cameras[MYE_MAX_DRAWN_CAMERAS];
    int camera_count =
        mye_camera2d_collect_for(world, 0, cameras, MYE_MAX_DRAWN_CAMERAS);

    if (camera_count == 0) {
        /* No camera at all: draw once in world coordinates, matching what the
         * sprite pass does, so a game that never made one still sees this. */
        BeginMode2D((Camera2D){ .zoom = 1.0f });
        draw_items(items, count);
        EndMode2D();
        return;
    }

    MyeSurface surface = mye_camera_surface(world, 0);
    for (int c = 0; c < camera_count; ++c) {
        Camera2D camera;
        if (!mye_camera2d_resolve(world, cameras[c], &camera)) {
            continue;
        }
        mye_camera_begin_2d(mye_camera_viewport(world, cameras[c]), surface,
                            camera);
        draw_items(items, count);
        mye_camera_end_2d();
    }
}

/* --------------------------------------------------------------- helpers -- */

ecs_entity_t mye_text_spawn(ecs_world_t *world, const char *text, float x,
                            float y, Color color)
{
    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, MyePosition2D, { x, y });
    ecs_set(world, e, MyeText,
            { .text = text,
              .size = 20.0f,
              .spacing = 1.0f,
              .color = color,
              .layer = 0 });
    return e;
}

void mye_text_set(ecs_world_t *world, ecs_entity_t entity, const char *text)
{
    /* Called from gameplay code on whatever it is updating, which may be an
     * entity already deleted this frame. Refuse quietly rather than tripping
     * flecs' assert. */
    if (world == NULL || entity == 0 || text == NULL ||
        !ecs_is_alive(world, entity)) {
        return;
    }

    const MyeText *existing = ecs_get(world, entity, MyeText);
    if (existing == NULL) {
        return; /* not a text entity: nothing to replace */
    }

    /* Set the whole component rather than reaching in through ecs_get_mut:
     * the copy hook is what frees the previous string, and it only runs on a
     * set. `next` carries the old pointer for exactly as long as it takes to
     * overwrite it, and is never destructed -- it is a plain stack struct. */
    MyeText next = *existing;
    next.text = text;
    ecs_set_ptr(world, entity, MyeText, &next);
}

Vector2 mye_text_measure(const ecs_world_t *world, const MyeText *text)
{
    if (world == NULL || text == NULL || text->text == NULL) {
        return (Vector2){ 0.0f, 0.0f };
    }

    const Font *font = mye_font_get_or_placeholder(world, text->font);
    if (font == NULL || font->baseSize <= 0) {
        return (Vector2){ 0.0f, 0.0f }; /* headless: no atlas, no metrics */
    }

    float size = text->size > 0.0f ? text->size : (float)font->baseSize;
    return MeasureTextEx(*font, text->text, size, text->spacing);
}

/* ------------------------------------------------------------- lifecycle -- */

static void text_fini(ecs_world_t *world, void *ctx)
{
    (void)world;
    MyeTextState *state = (MyeTextState *)ctx;
    if (state == NULL) {
        return;
    }
    if (state->texts != NULL) {
        ecs_query_fini(state->texts);
        state->texts = NULL;
    }
}

void MyeTextModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeTextModule);

    /* MyeRenderConfig, MyeHidden and MyeInterpolate come from render2d; the
     * placement components come from transform, which render2d imports. The
     * query below names all of them, so they must exist first. */
    ECS_IMPORT(world, MyeRender2dModule);

    ECS_COMPONENT_DEFINE(world, MyeText);

    mye_allocator a = mye_allocator_of(world);
    text_hook_ctx *ctx = MYE_NEW(a, text_hook_ctx);
    if (ctx != NULL) {
        ctx->allocator = a;
        /* Ownership of MyeText.text lives here. See the STRINGS note in
         * text.h for why this is hooks and not an OnSet observer. */
        ecs_set_hooks(world, MyeText, {
            .ctor = text_ctor,
            .dtor = text_dtor,
            .copy = text_copy,
            .move = text_move,
            .ctx = ctx,
            .ctx_free = text_hook_ctx_free,
        });
    }

    ECS_COMPONENT_DEFINE(world, MyeTextState);
    ecs_add_id(world, ecs_id(MyeTextState), EcsSingleton);
    ecs_singleton_set(world, MyeTextState, { 0 });
    MyeTextState *state = ecs_singleton_ensure(world, MyeTextState);

    /* One query for both passes: `world_space` picks which half each takes.
     * Two queries would be two iterations over the same tables. */
    state->texts = ecs_query(world, {
        .terms = {
            { .id = ecs_id(MyeText), .inout = EcsIn },
            { .id = ecs_id(MyePosition2D), .inout = EcsIn },
            { .id = ecs_id(MyeInterpolate), .inout = EcsIn,
              .oper = EcsOptional },
            { .id = ecs_id(MyeRenderTransform), .inout = EcsIn,
              .oper = EcsOptional },
            { .id = ecs_id(MyeHidden), .oper = EcsNot },
        },
    });

    /* Headless worlds get the component, the query and the string ownership
     * -- so text is fully testable -- but no draw systems, because DrawTextEx
     * needs a window and an OpenGL context. Same rule as the sprite pass. */
    const mye_engine *engine = mye_engine_get(world);
    if (engine == NULL || !engine->headless) {
        ECS_SYSTEM(world, MyeDrawTextWorld, MyeOnDraw2D, MyeRenderConfig);
        ECS_SYSTEM(world, MyeDrawTextUI, MyeOnDrawUI, MyeRenderConfig);
    }

    ecs_atfini(world, text_fini, state);
}
