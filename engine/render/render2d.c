#include "render/render2d.h"

#include "render/camera.h"
#include "render/canvas.h"

#include "core/log.h"

#include <raymath.h>
#include <rlgl.h>

#include <math.h>
#include <stdlib.h>

ECS_COMPONENT_DECLARE(MyeSprite);
ECS_COMPONENT_DECLARE(MyeSpriteAnim);
ECS_COMPONENT_DECLARE(MyeInterpolate);
ECS_COMPONENT_DECLARE(MyeHidden);
ECS_COMPONENT_DECLARE(MyeVisibilityLayers);
ECS_COMPONENT_DECLARE(MyeCamera2D);
ECS_COMPONENT_DECLARE(MyeRenderConfig);

/* Queries are built once at import -- building them per frame would allocate
 * every frame, which is exactly what a renderer must not do. Kept in a
 * singleton rather than a global so two worlds cannot clobber each other. */
typedef struct MyeRender2dState {
    ecs_query_t *sprites;
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
    /* The entity's VISIBILITY layers -- which cameras may draw it, nothing to
     * do with `layer` above, which is sort order within one camera. Resolved
     * once here rather than per camera, because the list is built once and
     * drawn by every camera. MYE_LAYERS_ALL when the entity has no
     * MyeVisibilityLayers, which every camera therefore draws. */
    uint32_t visibility;
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

/* ----------------------------------------------------------------- passes -- */

static void MyeRenderBegin(ecs_iter_t *it)
{
    const MyeRenderConfig *config = ecs_field(it, MyeRenderConfig, 0);
    BeginDrawing();
    ClearBackground(config->clear_color);
}

/* Draws every 2D camera rendering into `target` (0 = the window), in order.
 * The sprite list is built once and drawn per camera: sorting it again per
 * camera would be the same work repeated. */
void mye_render2d_draw_cameras_for(ecs_world_t *world, ecs_entity_t target,
                                   bool fallback_without_camera)
{
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
        const MyeRenderTransform *render_tf =
            ecs_field(&iter, MyeRenderTransform, 5);
        const MyeVisibilityLayers *visibility =
            ecs_field(&iter, MyeVisibilityLayers, 6);

        for (int i = 0; i < iter.count && count < total; ++i) {
            /* A sprite showing the very canvas being drawn would sample the
             * bound framebuffer -- undefined pixels, and a WebGL error per
             * draw. Leave it out of its own feed; it draws everywhere else. */
            if (mye_canvas_is_own_texture(world, target, sprites[i].texture)) {
                continue;
            }

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

            /* Where to draw, in order of precedence:
             *   1. the render transform, if the entity is in a hierarchy --
             *      it already carries the blend for this entity and every
             *      parent above it;
             *   2. its own blend, for an interpolated entity with no
             *      hierarchy to compose;
             *   3. its plain position.
             *
             * None of this is written back: the draw list is the only place
             * a blended position exists, so nothing can mistake it for the
             * simulation's. */
            float draw_x = positions[i].x;
            float draw_y = positions[i].y;
            if (render_tf != NULL) {
                Vector3 p = mye_matrix_translation(render_tf[i].m);
                draw_x = p.x;
                draw_y = p.y;
            } else if (interp != NULL && !interp[i].snap) {
                draw_x = interp[i].prev_x +
                         (positions[i].x - interp[i].prev_x) * alpha;
                draw_y = interp[i].prev_y +
                         (positions[i].y - interp[i].prev_y) * alpha;
            }

            items[count++] = (draw_item){
                .texture = texture,
                .source = source,
                /* Absolute: a negative source width or height means
                 * "draw mirrored" (raylib's idiom, and what a canvas needs
                 * because render textures are stored bottom-up). Letting the
                 * sign through to the destination would move the quad
                 * instead of flipping its contents. */
                .dest = { draw_x, draw_y, fabsf(source.width) * sx,
                          fabsf(source.height) * sy },
                .origin = { sprites[i].origin.x * sx, sprites[i].origin.y * sy },
                .rotation_degrees = angle * RAD2DEG,
                .tint = sprites[i].tint,
                .layer = sprites[i].layer,
                .visibility = visibility != NULL ? visibility[i].mask
                                                 : MYE_LAYERS_ALL,
            };
        }
    }

    qsort(items, (size_t)count, sizeof *items, compare_draw_items);

    /* Every active camera draws the same sorted list, each into its own
     * viewport, in order. The list is built once: sorting it per camera
     * would be the same work repeated. */
    ecs_entity_t cameras[MYE_MAX_DRAWN_CAMERAS];
    int camera_count = mye_camera2d_collect_for(world, target, cameras,
                                                MYE_MAX_DRAWN_CAMERAS);

    if (camera_count == 0 && fallback_without_camera) {
        /* No camera at all: draw once in world coordinates, so a game that
         * never sets one still sees its sprites. A canvas gets no such
         * fallback -- a canvas with no camera should stay empty, not
         * accidentally receive the whole world. */
        BeginMode2D((Camera2D){ .zoom = 1.0f });
        for (int32_t i = 0; i < count; ++i) {
            DrawTexturePro(*items[i].texture, items[i].source, items[i].dest,
                           items[i].origin, items[i].rotation_degrees,
                           items[i].tint);
        }
        EndMode2D();
        return;
    }

    MyeSurface surface = mye_camera_surface(world, target);

    for (int c = 0; c < camera_count; ++c) {
        Camera2D camera;
        if (!mye_camera2d_resolve(world, cameras[c], &camera)) {
            continue;
        }
        const MyeCamera2D *cam = ecs_get(world, cameras[c], MyeCamera2D);
        uint32_t layers = cam != NULL ? cam->layers : 0;

        mye_camera_begin_2d(mye_camera_viewport(world, cameras[c]), surface,
                            camera);
        for (int32_t i = 0; i < count; ++i) {
            /* The rule mye_camera_sees states, applied to the masks already
             * in hand. A camera with no layers set -- every camera that
             * predates the field -- draws the list exactly as before. */
            if (layers != 0 && (layers & items[i].visibility) == 0) {
                continue;
            }
            DrawTexturePro(*items[i].texture, items[i].source, items[i].dest,
                           items[i].origin, items[i].rotation_degrees,
                           items[i].tint);
        }
        mye_camera_end_2d();
    }
}

static void MyeRenderSprites(ecs_iter_t *it)
{
    (void)ecs_field(it, MyeRenderConfig, 0);
    mye_render2d_draw_cameras_for(it->world, 0, true);
}

static void MyeRenderEnd(ecs_iter_t *it)
{
    /* Anything that wants this frame's pixels must read them HERE, before
     * EndDrawing swaps buffers. After the swap the back buffer holds
     * whatever the driver left there -- usually the previous frame -- and a
     * read then is a frame late at best. That is not theoretical: it hid a
     * multi-camera bug for an afternoon by showing the frame before. */
    mye_engine *engine = mye_engine_get(it->world);
    if (engine != NULL && engine->screenshot_path != NULL &&
        engine->max_frames > 0) {
        const MyeTime *now = ecs_singleton_get(it->world, MyeTime);
        if (now != NULL && now->frame >= engine->max_frames) {
            rlDrawRenderBatchActive();
            Image shot = LoadImageFromScreen();
            if (shot.data != NULL) {
                ExportImage(shot, engine->screenshot_path);
                UnloadImage(shot);
            }
            engine->screenshot_path = NULL; /* once only */
        }
    }
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

/* Composes the drawn transform chain: each link contributes its own motion
 * blended between the last two fixed steps, then multiplies through its
 * parent's already-blended transform.
 *
 * Blending each link separately, rather than blending the finished world
 * transform, is what keeps the rate right: MyeInterpolate records where an
 * entity was one fixed STEP ago, while transforms are composed once per
 * FRAME. A frame that runs two steps -- or none -- would otherwise blend
 * across the wrong interval and draw the entity at the wrong place and the
 * wrong speed.
 *
 * Interpolation only ever moves an entity, so the blended local transform is
 * the raw one with its translation replaced; rotation and scale carry over
 * untouched. Runs in EcsPreStore: after propagation has produced this
 * frame's world transforms, before anything draws. */
static void MyeBlendRenderTransforms(ecs_iter_t *it)
{
    const MyeLocalTransform *local = ecs_field(it, MyeLocalTransform, 0);
    MyeRenderTransform *render = ecs_field(it, MyeRenderTransform, 1);
    const MyeInterpolate *interp = ecs_field(it, MyeInterpolate, 2);
    const MyePosition2D *pos = ecs_field(it, MyePosition2D, 3);
    const MyeRenderTransform *parent = ecs_field(it, MyeRenderTransform, 4);

    const MyeTime *time = ecs_singleton_get(it->world, MyeTime);
    float alpha = time != NULL ? time->alpha : 0.0f;

    for (int i = 0; i < it->count; ++i) {
        Matrix blended = local[i].m;

        if (interp != NULL && pos != NULL && !interp[i].snap) {
            blended.m12 = interp[i].prev_x +
                          (pos[i].x - interp[i].prev_x) * alpha;
            blended.m13 = interp[i].prev_y +
                          (pos[i].y - interp[i].prev_y) * alpha;
        }

        /* One shared value for the table, not an array: the parent term
         * reads from the parent entity. */
        render[i].m = parent != NULL ? MatrixMultiply(blended, parent->m)
                                     : blended;
    }
}

/* Tag: this entity has already been warned about, so a sprite that is set
 * every frame (a tint change, a scene reload) does not warn every frame. */
typedef struct MyeTransformWarned {
    char unused;
} MyeTransformWarned;

ECS_COMPONENT_DECLARE(MyeTransformWarned);

/* A sprite parented to something, but without the transform components the
 * hierarchy runs on, is a silent misplacement: MyePosition2D is then read as
 * a world position when it was authored as an offset, so the sprite draws
 * near the origin instead of on its parent.
 *
 * Reached from two directions, because either order is natural: setting the
 * sprite on an entity that is already a child, or -- as mye_sprite_spawn
 * encourages -- parenting an entity that already has its sprite. */
static void MyeWarnParentedWithoutTransform(ecs_iter_t *it)
{
    ecs_world_t *world = it->world;

    for (int i = 0; i < it->count; ++i) {
        ecs_entity_t e = it->entities[i];

        if (!ecs_has(world, e, MyeSprite)) {
            continue;
        }
        if (ecs_get_target(world, e, EcsChildOf, 0) == 0) {
            continue; /* a root: its position IS its world position */
        }
        if (ecs_has(world, e, MyeWorldTransform)) {
            continue; /* takes part in the hierarchy; nothing to warn about */
        }
        if (ecs_has(world, e, MyeTransformWarned)) {
            continue;
        }

        const char *name = ecs_get_name(world, e);
        mye_log_warn(
            "sprite '%s' has a parent but no MyeLocalTransform/"
            "MyeWorldTransform, so its MyePosition2D will be drawn as a world "
            "position rather than as an offset from its parent -- it will "
            "appear near the origin. Add both components (or use "
            "mye_spawn_3d) to place it relative to its parent.",
            name != NULL ? name : "<unnamed>");
        ecs_add(world, e, MyeTransformWarned);
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
}

void MyeRender2dModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeRender2dModule);

    /* The sprite pass reads MyeWorldTransform and the placement components,
     * so they must exist before its query is built. */
    ECS_IMPORT(world, MyeTransformModule);

    ECS_COMPONENT_DEFINE(world, MyeSprite);
    ECS_COMPONENT_DEFINE(world, MyeSpriteAnim);
    ECS_COMPONENT_DEFINE(world, MyeInterpolate);
    ECS_COMPONENT_DEFINE(world, MyeHidden);
    /* Defined here, before the sprite query names it, and used by the 3D pass
     * too: layers belong to drawing, and both renderers do the same drawing. */
    ECS_COMPONENT_DEFINE(world, MyeVisibilityLayers);
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
            /* Present when the entity takes part in the transform hierarchy;
             * this is where the blended parent chain has actually put it. */
            { .id = ecs_id(MyeRenderTransform), .inout = EcsIn,
              .oper = EcsOptional },
            /* Optional, and absent on almost everything: a sprite that names
             * no layers is drawn by every camera. */
            { .id = ecs_id(MyeVisibilityLayers), .inout = EcsIn,
              .oper = EcsOptional },
            { .id = ecs_id(MyeHidden), .oper = EcsNot },
        },
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

    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "MyeBlendRenderTransforms",
                                      .add = ecs_ids(ecs_dependson(
                                          EcsPreStore)) }),
        .query.terms = {
            { .id = ecs_id(MyeLocalTransform), .inout = EcsIn },
            { .id = ecs_id(MyeRenderTransform), .inout = EcsOut },
            { .id = ecs_id(MyeInterpolate), .inout = EcsIn,
              .oper = EcsOptional },
            { .id = ecs_id(MyePosition2D), .inout = EcsIn,
              .oper = EcsOptional },
            /* Parent's blended transform, breadth-first so it is final
             * before any child reads it. Optional: roots have no parent. */
            { .id = ecs_id(MyeRenderTransform),
              .inout = EcsIn,
              .oper = EcsOptional,
              .src.id = EcsCascade,
              .trav = EcsChildOf },
        },
        .callback = MyeBlendRenderTransforms,
    });

    ECS_COMPONENT_DEFINE(world, MyeTransformWarned);

    ecs_observer(world, {
        .query.terms = {{ .id = ecs_id(MyeSprite) }},
        .events = { EcsOnSet },
        .callback = MyeWarnParentedWithoutTransform,
    });

    ecs_observer(world, {
        .query.terms = {{ .id = ecs_pair(EcsChildOf, EcsWildcard) }},
        .events = { EcsOnAdd },
        .callback = MyeWarnParentedWithoutTransform,
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
