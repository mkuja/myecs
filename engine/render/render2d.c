#include "render/render2d.h"

#include <stdlib.h>

ECS_COMPONENT_DECLARE(MyePosition2D);
ECS_COMPONENT_DECLARE(MyeRotation2D);
ECS_COMPONENT_DECLARE(MyeScale2D);
ECS_COMPONENT_DECLARE(MyeSprite);
ECS_COMPONENT_DECLARE(MyeSpriteAnim);
ECS_COMPONENT_DECLARE(MyeInterpolate);
ECS_COMPONENT_DECLARE(MyeHidden);
ECS_COMPONENT_DECLARE(MyeCamera2D);
ECS_COMPONENT_DECLARE(MyeRenderConfig);

/* Queries are built once at import -- building them per frame would allocate
 * every frame, which is exactly what a renderer must not do. Kept in a
 * singleton rather than a global so two worlds cannot clobber each other. */
typedef struct MyeRender2dState {
    ecs_query_t *sprites;
    ecs_query_t *cameras;
} MyeRender2dState;

ECS_COMPONENT_DECLARE(MyeRender2dState);

static const MyeRender2dState *render_state(const ecs_world_t *world)
{
    return ecs_singleton_get(world, MyeRender2dState);
}

/* One entry per visible sprite, built fresh each frame in the frame arena. */
typedef struct draw_item {
    const Texture2D *texture;
    Rectangle source;
    Rectangle dest;
    Vector2 origin;
    float rotation_degrees;
    Color tint;
    int32_t layer;
} draw_item;

/* Back-to-front: layer first, then y so entities lower on screen overlap
 * those above them (the usual top-down depth illusion), then texture so
 * raylib can batch identical textures into one draw call. */
static int compare_draw_items(const void *lhs, const void *rhs)
{
    const draw_item *a = (const draw_item *)lhs;
    const draw_item *b = (const draw_item *)rhs;

    if (a->layer != b->layer) {
        return a->layer < b->layer ? -1 : 1;
    }
    if (a->dest.y != b->dest.y) {
        return a->dest.y < b->dest.y ? -1 : 1;
    }
    uintptr_t ta = (uintptr_t)a->texture;
    uintptr_t tb = (uintptr_t)b->texture;
    if (ta != tb) {
        return ta < tb ? -1 : 1;
    }
    return 0;
}

static Camera2D active_camera(ecs_world_t *world)
{
    Camera2D camera = { .zoom = 1.0f };
    const MyeRender2dState *state = render_state(world);
    if (state == NULL || state->cameras == NULL) {
        return camera;
    }

    ecs_iter_t it = ecs_query_iter(world, state->cameras);
    while (ecs_query_next(&it)) {
        const MyeCamera2D *cams = ecs_field(&it, MyeCamera2D, 0);
        for (int i = 0; i < it.count; ++i) {
            if (!cams[i].active) {
                continue;
            }
            camera = cams[i].camera;
            if (camera.zoom == 0.0f) {
                camera.zoom = 1.0f; /* a zero-zoom camera would show nothing */
            }
            ecs_iter_fini(&it);
            return camera;
        }
    }
    return camera;
}

/* ----------------------------------------------------------------- passes -- */

static void MyeRenderBegin(ecs_iter_t *it)
{
    const MyeRenderConfig *config = ecs_field(it, MyeRenderConfig, 0);
    BeginDrawing();
    ClearBackground(config->clear_color);
}

static void MyeRenderSprites(ecs_iter_t *it)
{
    ecs_world_t *world = it->world;
    const MyeRender2dState *state = render_state(world);
    if (state == NULL || state->sprites == NULL) {
        return;
    }

    /* Collect, sort, draw. The scratch array lives in the frame arena, so
     * there is nothing to free and no per-frame malloc. */
    mye_allocator frame = mye_frame_allocator(world);

    /* How far this frame sits between the last two fixed steps. */
    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    float alpha = time != NULL ? time->alpha : 0.0f;

    int32_t total = 0;
    ecs_iter_t count_it = ecs_query_iter(world, state->sprites);
    while (ecs_query_next(&count_it)) {
        total += count_it.count;
    }
    if (total == 0) {
        return;
    }

    draw_item *items = MYE_NEW_ARRAY(frame, draw_item, (size_t)total);
    if (items == NULL) {
        /* Frame arena exhausted: skip the pass rather than draw garbage.
         * Raising mye_config.frame_arena_bytes is the fix. */
        return;
    }

    int32_t count = 0;
    ecs_iter_t iter = ecs_query_iter(world, state->sprites);
    while (ecs_query_next(&iter)) {
        const MyeSprite *sprites = ecs_field(&iter, MyeSprite, 0);
        const MyePosition2D *positions = ecs_field(&iter, MyePosition2D, 1);
        const MyeRotation2D *rotations = ecs_field(&iter, MyeRotation2D, 2);
        const MyeScale2D *scales = ecs_field(&iter, MyeScale2D, 3);
        const MyeInterpolate *interp = ecs_field(&iter, MyeInterpolate, 4);

        for (int i = 0; i < iter.count && count < total; ++i) {
            const Texture2D *texture =
                mye_texture_get_or_placeholder(world, sprites[i].texture);
            if (texture == NULL) {
                continue; /* headless, or no placeholder available */
            }

            Rectangle source = sprites[i].source;
            if (source.width == 0.0f || source.height == 0.0f) {
                source = (Rectangle){ 0.0f, 0.0f, (float)texture->width,
                                      (float)texture->height };
            }

            float sx = scales != NULL ? scales[i].x : 1.0f;
            float sy = scales != NULL ? scales[i].y : 1.0f;
            float angle = rotations != NULL ? rotations[i].angle : 0.0f;

            /* Blend between the last two simulated positions if this entity
             * opted in. The result stays local to the draw list: it is never
             * written back, so nothing else can mistake it for the
             * simulation's position. */
            float draw_x = positions[i].x;
            float draw_y = positions[i].y;
            if (interp != NULL && !interp[i].snap) {
                draw_x = interp[i].prev_x +
                         (positions[i].x - interp[i].prev_x) * alpha;
                draw_y = interp[i].prev_y +
                         (positions[i].y - interp[i].prev_y) * alpha;
            }

            items[count++] = (draw_item){
                .texture = texture,
                .source = source,
                .dest = { draw_x, draw_y, source.width * sx,
                          source.height * sy },
                .origin = { sprites[i].origin.x * sx, sprites[i].origin.y * sy },
                .rotation_degrees = angle * RAD2DEG,
                .tint = sprites[i].tint,
                .layer = sprites[i].layer,
            };
        }
    }

    qsort(items, (size_t)count, sizeof *items, compare_draw_items);

    BeginMode2D(active_camera(world));
    for (int32_t i = 0; i < count; ++i) {
        DrawTexturePro(*items[i].texture, items[i].source, items[i].dest,
                       items[i].origin, items[i].rotation_degrees,
                       items[i].tint);
    }
    EndMode2D();
}

static void MyeRenderEnd(ecs_iter_t *it)
{
    (void)it;
    EndDrawing();
}

/* ------------------------------------------------------- interpolation -- */

/* Records where each interpolated entity was before this fixed step, so the
 * renderer has two endpoints to blend between. Runs in MyeOnFixedUpdate, at
 * the top of every step, so `prev` is always the state one step back -- not
 * one frame back, which would be wrong whenever a frame runs several steps.
 *
 * A pending snap makes prev = current, so the blend for that frame is a
 * no-op and a teleport draws no streak. */
static void MyeCapturePrevPositions(ecs_iter_t *it)
{
    MyeInterpolate *interp = ecs_field(it, MyeInterpolate, 0);
    const MyePosition2D *pos = ecs_field(it, MyePosition2D, 1);

    for (int i = 0; i < it->count; ++i) {
        interp[i].prev_x = pos[i].x;
        interp[i].prev_y = pos[i].y;
        interp[i].snap = false;
    }
}

void mye_transform_snap(ecs_world_t *world, ecs_entity_t entity)
{
    /* Called from gameplay code on whatever it just teleported, which may be
     * an entity already deleted this frame. Refuse quietly rather than
     * tripping flecs' assert. */
    if (world == NULL || entity == 0 || !ecs_is_alive(world, entity)) {
        return;
    }

    MyeInterpolate *interp = ecs_get_mut(world, entity, MyeInterpolate);
    if (interp == NULL) {
        return; /* not interpolated: nothing to suppress */
    }
    const MyePosition2D *pos = ecs_get(world, entity, MyePosition2D);
    if (pos != NULL) {
        interp->prev_x = pos->x;
        interp->prev_y = pos->y;
    }
    interp->snap = true;
    ecs_modified(world, entity, MyeInterpolate);
}

/* ----------------------------------------------------------- animation -- */

Rectangle mye_atlas_frame(Rectangle first_frame, int columns, int index)
{
    if (columns < 1) {
        columns = 1;
    }
    if (index < 0) {
        index = 0;
    }
    int col = index % columns;
    int row = index / columns;

    return (Rectangle){
        .x = first_frame.x + (float)col * first_frame.width,
        .y = first_frame.y + (float)row * first_frame.height,
        .width = first_frame.width,
        .height = first_frame.height,
    };
}

void mye_sprite_anim_restart(MyeSpriteAnim *anim)
{
    if (anim == NULL) {
        return;
    }
    anim->current = 0;
    anim->elapsed = 0.0f;
    anim->finished = false;
    anim->playing = true;
}

bool mye_sprite_anim_advance(MyeSpriteAnim *anim, float dt)
{
    if (anim == NULL || !anim->playing || anim->frame_count <= 0 ||
        anim->fps <= 0.0f) {
        return false;
    }

    anim->elapsed += dt;
    float seconds_per_frame = 1.0f / anim->fps;
    if (anim->elapsed < seconds_per_frame) {
        return false;
    }

    /* A long frame may cross several frames at once -- step by whole frames
     * rather than assuming one, so animations keep real time after a stall. */
    int steps = (int)(anim->elapsed / seconds_per_frame);
    anim->elapsed -= (float)steps * seconds_per_frame;

    int next = anim->current + steps;
    if (next < anim->frame_count) {
        anim->current = next;
        return true;
    }

    if (anim->loop) {
        anim->current = next % anim->frame_count;
    } else {
        anim->current = anim->frame_count - 1; /* hold the last frame */
        anim->playing = false;
        anim->finished = true;
    }
    return true;
}

/* Advances every animation and writes the chosen frame into its sprite.
 * EcsPreStore, so rendering in EcsOnStore sees this frame's selection. */
static void MyeSpriteAnimUpdate(ecs_iter_t *it)
{
    MyeSpriteAnim *anims = ecs_field(it, MyeSpriteAnim, 0);
    MyeSprite *sprites = ecs_field(it, MyeSprite, 1);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        mye_sprite_anim_advance(&anims[i], dt);
        sprites[i].source = mye_atlas_frame(anims[i].first_frame,
                                            anims[i].columns,
                                            anims[i].current);
    }
}

/* -------------------------------------------------------------- helpers -- */

ecs_entity_t mye_sprite_spawn(ecs_world_t *world, mye_texture texture, float x,
                              float y, Color tint)
{
    const Texture2D *t = mye_texture_get(world, texture);
    float half_w = t != NULL ? (float)t->width * 0.5f : 0.0f;
    float half_h = t != NULL ? (float)t->height * 0.5f : 0.0f;

    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, MyePosition2D, { x, y });
    ecs_set(world, e, MyeSprite,
            { .texture = texture,
              .origin = { half_w, half_h }, /* centred by default */
              .tint = tint,
              .layer = 0 });
    return e;
}

/* ------------------------------------------------------------- lifecycle -- */

static void render2d_fini(ecs_world_t *world, void *ctx)
{
    (void)world;
    MyeRender2dState *state = (MyeRender2dState *)ctx;
    if (state == NULL) {
        return;
    }
    if (state->sprites != NULL) {
        ecs_query_fini(state->sprites);
        state->sprites = NULL;
    }
    if (state->cameras != NULL) {
        ecs_query_fini(state->cameras);
        state->cameras = NULL;
    }
}

void MyeRender2dModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeRender2dModule);

    ECS_COMPONENT_DEFINE(world, MyePosition2D);
    ECS_COMPONENT_DEFINE(world, MyeRotation2D);
    ECS_COMPONENT_DEFINE(world, MyeScale2D);
    ECS_COMPONENT_DEFINE(world, MyeSprite);
    ECS_COMPONENT_DEFINE(world, MyeSpriteAnim);
    ECS_COMPONENT_DEFINE(world, MyeInterpolate);
    ECS_COMPONENT_DEFINE(world, MyeHidden);
    ECS_COMPONENT_DEFINE(world, MyeCamera2D);
    ECS_COMPONENT_DEFINE(world, MyeRenderConfig);

    ecs_add_id(world, ecs_id(MyeRenderConfig), EcsSingleton);
    ecs_singleton_set(world, MyeRenderConfig,
                      { .clear_color = (Color){ 18, 18, 24, 255 } });

    ECS_COMPONENT_DEFINE(world, MyeRender2dState);
    ecs_add_id(world, ecs_id(MyeRender2dState), EcsSingleton);
    ecs_singleton_set(world, MyeRender2dState, { 0 });
    MyeRender2dState *state = ecs_singleton_ensure(world, MyeRender2dState);

    state->sprites = ecs_query(world, {
        .terms = {
            { .id = ecs_id(MyeSprite), .inout = EcsIn },
            { .id = ecs_id(MyePosition2D), .inout = EcsIn },
            { .id = ecs_id(MyeRotation2D), .inout = EcsIn, .oper = EcsOptional },
            { .id = ecs_id(MyeScale2D), .inout = EcsIn, .oper = EcsOptional },
            { .id = ecs_id(MyeInterpolate), .inout = EcsIn,
              .oper = EcsOptional },
            { .id = ecs_id(MyeHidden), .oper = EcsNot },
        },
    });
    state->cameras = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyeCamera2D), .inout = EcsIn }},
    });

    /* Headless worlds get the components, queries and phases -- so gameplay
     * logic is fully testable -- but no draw systems, because raylib's
     * drawing calls require a window and an OpenGL context. */
    /* Captures previous positions at the top of every fixed step. Registered
     * headless too: the state it maintains is simulation, not drawing. */
    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "MyeCapturePrevPositions",
                                      .add = ecs_ids(ecs_dependson(
                                          MyeOnFixedUpdate)) }),
        .query.terms = {
            { .id = ecs_id(MyeInterpolate) },
            { .id = ecs_id(MyePosition2D), .inout = EcsIn },
        },
        .callback = MyeCapturePrevPositions,
    });

    /* Animation is simulation, not drawing: it runs headless too, so tests
     * can assert on frame progression without a window. */
    ECS_SYSTEM(world, MyeSpriteAnimUpdate, EcsPreStore, MyeSpriteAnim,
               MyeSprite);

    const mye_engine *engine = mye_engine_get(world);
    if (engine == NULL || !engine->headless) {
        /* Phases from engine.h fix the order: begin, then 3D, then these
         * sprites, then the game's UI, then end. */
        ECS_SYSTEM(world, MyeRenderBegin, EcsOnStore, MyeRenderConfig);
        ECS_SYSTEM(world, MyeRenderSprites, MyeOnDraw2D, MyeRenderConfig);
        ECS_SYSTEM(world, MyeRenderEnd, MyeOnRenderEnd, MyeRenderConfig);
    }

    ecs_atfini(world, render2d_fini, state);
}
