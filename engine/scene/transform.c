#include "scene/transform.h"

/* For MyeInterpolate: an entity outside the hierarchy that opted in is drawn
 * blended by the sprite pass, and "where is it drawn" has to give the same
 * answer. A header dependency only -- render2d imports this module, not the
 * other way round. */
#include "render/render2d.h"

#include <raymath.h>

ECS_COMPONENT_DECLARE(MyePosition2D);
ECS_COMPONENT_DECLARE(MyeRotation2D);
ECS_COMPONENT_DECLARE(MyeScale2D);
ECS_COMPONENT_DECLARE(MyePosition3D);
ECS_COMPONENT_DECLARE(MyeRotation3D);
ECS_COMPONENT_DECLARE(MyeScale3D);
ECS_COMPONENT_DECLARE(MyeLocalTransform);
ECS_COMPONENT_DECLARE(MyeWorldTransform);
ECS_COMPONENT_DECLARE(MyeRenderTransform);

/* ------------------------------------------------------------------ maths -- */

Matrix mye_trs_matrix(Vector3 translation, Quaternion rotation, Vector3 scale)
{
    /* raymath composes row-vector style, so the product reads in the order
     * the operations are applied: scale, then rotate, then translate. */
    Matrix s = MatrixScale(scale.x, scale.y, scale.z);
    Matrix r = QuaternionToMatrix(rotation);
    Matrix t = MatrixTranslate(translation.x, translation.y, translation.z);
    return MatrixMultiply(MatrixMultiply(s, r), t);
}

Vector3 mye_matrix_translation(Matrix m)
{
    return (Vector3){ m.m12, m.m13, m.m14 };
}

/* -------------------------------------------------------- local matrices -- */

/* 3D placement -> local matrix. Rotation and scale are optional. */
static void MyeLocalFrom3D(ecs_iter_t *it)
{
    const MyePosition3D *pos = ecs_field(it, MyePosition3D, 0);
    const MyeRotation3D *rot = ecs_field(it, MyeRotation3D, 1);
    const MyeScale3D *scale = ecs_field(it, MyeScale3D, 2);
    MyeLocalTransform *local = ecs_field(it, MyeLocalTransform, 3);

    for (int i = 0; i < it->count; ++i) {
        Quaternion q = rot != NULL ? rot[i].q : QuaternionIdentity();
        Vector3 s = scale != NULL ? scale[i].v : (Vector3){ 1.0f, 1.0f, 1.0f };
        local[i].m = mye_trs_matrix(pos[i].v, q, s);
    }
}

/* 2D placement -> local matrix, so 2D entities can be parented too. The 2D
 * rotation is about Z, which is what a top-down or side-on view expects. */
static void MyeLocalFrom2D(ecs_iter_t *it)
{
    const MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const MyeRotation2D *rot = ecs_field(it, MyeRotation2D, 1);
    const MyeScale2D *scale = ecs_field(it, MyeScale2D, 2);
    MyeLocalTransform *local = ecs_field(it, MyeLocalTransform, 3);

    for (int i = 0; i < it->count; ++i) {
        float angle = rot != NULL ? rot[i].angle : 0.0f;
        Quaternion q = QuaternionFromAxisAngle((Vector3){ 0.0f, 0.0f, 1.0f },
                                               angle);
        Vector3 s = scale != NULL
                        ? (Vector3){ scale[i].x, scale[i].y, 1.0f }
                        : (Vector3){ 1.0f, 1.0f, 1.0f };
        local[i].m = mye_trs_matrix((Vector3){ pos[i].x, pos[i].y, 0.0f }, q, s);
    }
}

/* ------------------------------------------------------------ propagation -- */

/* The drawn transform (see MyeRenderTransform) is carried by everything in
 * the hierarchy, not only the interpolated entities, because an entity that
 * does not interpolate still has to be drawn at its parent's blended
 * position. Kept in step with MyeWorldTransform by these two observers. */
static void MyeAddRenderTransform(ecs_iter_t *it)
{
    for (int i = 0; i < it->count; ++i) {
        /* Deferred inside an observer, so it lands at the merge point rather
         * than mutating the table being iterated. */
        ecs_set(it->world, it->entities[i], MyeRenderTransform,
                { MatrixIdentity() });
    }
}

/* Without this, an entity that leaves the hierarchy but keeps its sprite
 * would keep a MyeRenderTransform that outranks its position in the draw
 * path -- freezing it at wherever it was last composed. */
static void MyeRemoveRenderTransform(ecs_iter_t *it)
{
    for (int i = 0; i < it->count; ++i) {
        ecs_remove(it->world, it->entities[i], MyeRenderTransform);
    }
}

/* world = parent_world * local, or world = local at the root.
 *
 * The parent term uses EcsCascade, which iterates breadth-first: every table
 * at depth N is visited before any table at depth N+1, so a parent's world
 * matrix is final by the time its children read it. Without cascade a deep
 * hierarchy would lag one frame per level. */
static void MyePropagateTransforms(ecs_iter_t *it)
{
    const MyeLocalTransform *local = ecs_field(it, MyeLocalTransform, 0);
    MyeWorldTransform *world = ecs_field(it, MyeWorldTransform, 1);
    const MyeWorldTransform *parent = ecs_field(it, MyeWorldTransform, 2);

    if (parent == NULL) {
        for (int i = 0; i < it->count; ++i) { /* roots */
            world[i].m = local[i].m;
        }
        return;
    }

    /* The parent field comes from the parent entity, so it is a single shared
     * value for the whole table, not an array to index by row. */
    Matrix parent_matrix = parent->m;
    for (int i = 0; i < it->count; ++i) {
        world[i].m = MatrixMultiply(local[i].m, parent_matrix);
    }
}

/* -------------------------------------------------------------- helpers -- */

ecs_entity_t mye_spawn_3d(ecs_world_t *world, Vector3 position)
{
    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, MyePosition3D, { position });
    ecs_set(world, e, MyeRotation3D, { QuaternionIdentity() });
    ecs_set(world, e, MyeScale3D, { { 1.0f, 1.0f, 1.0f } });
    ecs_set(world, e, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, e, MyeWorldTransform, { MatrixIdentity() });
    return e;
}

void mye_set_parent(ecs_world_t *world, ecs_entity_t child,
                    ecs_entity_t parent)
{
    ecs_entity_t existing = ecs_get_target(world, child, EcsChildOf, 0);
    if (existing != 0) {
        ecs_remove_pair(world, child, EcsChildOf, existing);
    }
    if (parent != 0) {
        ecs_add_pair(world, child, EcsChildOf, parent);
    }
}

Vector3 mye_world_position(const ecs_world_t *world, ecs_entity_t entity)
{
    const MyeWorldTransform *t = ecs_get(world, entity, MyeWorldTransform);
    if (t == NULL) {
        return (Vector3){ 0.0f, 0.0f, 0.0f };
    }
    return mye_matrix_translation(t->m);
}

Vector3 mye_render_position(const ecs_world_t *world, ecs_entity_t entity)
{
    const MyeRenderTransform *r = ecs_get(world, entity, MyeRenderTransform);
    if (r != NULL) {
        return mye_matrix_translation(r->m);
    }

    /* Not in the hierarchy: answer with whatever placement it does have,
     * rather than reporting the origin and sending a camera there. */
    const MyeWorldTransform *w = ecs_get(world, entity, MyeWorldTransform);
    if (w != NULL) {
        return mye_matrix_translation(w->m);
    }
    const MyePosition3D *p3 = ecs_get(world, entity, MyePosition3D);
    if (p3 != NULL) {
        return p3->v;
    }
    const MyePosition2D *p2 = ecs_get(world, entity, MyePosition2D);
    if (p2 != NULL) {
        /* An interpolated sprite outside the hierarchy is drawn blended by
         * the sprite pass (render2d.c). This must say the same, or a camera
         * following it trails the picture by up to a step -- the exact
         * shimmer the whole drawn-position idea exists to prevent. */
        const MyeInterpolate *interp = ecs_get(world, entity, MyeInterpolate);
        const MyeTime *time = ecs_singleton_get(world, MyeTime);
        if (interp != NULL && !interp->snap && time != NULL) {
            float alpha = time->alpha;
            return (Vector3){ interp->prev_x + (p2->x - interp->prev_x) * alpha,
                              interp->prev_y + (p2->y - interp->prev_y) * alpha,
                              0.0f };
        }
        return (Vector3){ p2->x, p2->y, 0.0f };
    }
    return (Vector3){ 0.0f, 0.0f, 0.0f };
}

/* raymath's QuaternionFromMatrix assumes an orthonormal 3x3. A scaled matrix
 * gives an unnormalised quaternion whose w:xyz ratio is already wrong --
 * normalising afterwards does not repair it -- and anything rotated by it is
 * then scaled as well. Divide the scale out first. */
Quaternion mye_matrix_rotation(Matrix m)
{
    Vector3 x = Vector3Normalize((Vector3){ m.m0, m.m1, m.m2 });
    Vector3 y = Vector3Normalize((Vector3){ m.m4, m.m5, m.m6 });
    Vector3 z = Vector3Normalize((Vector3){ m.m8, m.m9, m.m10 });

    /* A negative-scale flip is a mirror, not a rotation; there is no correct
     * quaternion for it. Restore a right-handed basis so the result is at
     * least a proper rotation instead of garbage. */
    if (Vector3DotProduct(Vector3CrossProduct(x, y), z) < 0.0f) {
        z = Vector3Negate(z);
    }

    Matrix ortho = m;
    ortho.m0 = x.x; ortho.m1 = x.y; ortho.m2 = x.z;
    ortho.m4 = y.x; ortho.m5 = y.y; ortho.m6 = y.z;
    ortho.m8 = z.x; ortho.m9 = z.y; ortho.m10 = z.z;
    return QuaternionNormalize(QuaternionFromMatrix(ortho));
}

Quaternion mye_render_rotation(const ecs_world_t *world, ecs_entity_t entity)
{
    const MyeRenderTransform *r = ecs_get(world, entity, MyeRenderTransform);
    const MyeWorldTransform *w =
        r == NULL ? ecs_get(world, entity, MyeWorldTransform) : NULL;
    if (r != NULL || w != NULL) {
        return mye_matrix_rotation(r != NULL ? r->m : w->m);
    }

    const MyeRotation3D *rot3 = ecs_get(world, entity, MyeRotation3D);
    if (rot3 != NULL) {
        return rot3->q;
    }
    const MyeRotation2D *rot2 = ecs_get(world, entity, MyeRotation2D);
    if (rot2 != NULL) {
        return QuaternionFromAxisAngle((Vector3){ 0.0f, 0.0f, 1.0f },
                                       rot2->angle);
    }
    return QuaternionIdentity();
}

ecs_entity_t mye_spawn_2d(ecs_world_t *world, Vector2 position)
{
    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, MyePosition2D, { position.x, position.y });
    ecs_set(world, e, MyeRotation2D, { 0.0f });
    ecs_set(world, e, MyeScale2D, { 1.0f, 1.0f });
    ecs_set(world, e, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, e, MyeWorldTransform, { MatrixIdentity() });
    return e;
}

/* ------------------------------------------------------------- lifecycle -- */

void MyeTransformModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeTransformModule);

    ECS_COMPONENT_DEFINE(world, MyePosition2D);
    ECS_COMPONENT_DEFINE(world, MyeRotation2D);
    ECS_COMPONENT_DEFINE(world, MyeScale2D);
    ECS_COMPONENT_DEFINE(world, MyePosition3D);
    ECS_COMPONENT_DEFINE(world, MyeRotation3D);
    ECS_COMPONENT_DEFINE(world, MyeScale3D);
    ECS_COMPONENT_DEFINE(world, MyeLocalTransform);
    ECS_COMPONENT_DEFINE(world, MyeWorldTransform);
    ECS_COMPONENT_DEFINE(world, MyeRenderTransform);

    /* EcsPostUpdate: after gameplay has moved things, before rendering. */
    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "MyeLocalFrom3D",
                                      .add = ecs_ids(ecs_dependson(EcsPostUpdate)) }),
        .query.terms = {
            { .id = ecs_id(MyePosition3D), .inout = EcsIn },
            { .id = ecs_id(MyeRotation3D), .inout = EcsIn, .oper = EcsOptional },
            { .id = ecs_id(MyeScale3D), .inout = EcsIn, .oper = EcsOptional },
            { .id = ecs_id(MyeLocalTransform), .inout = EcsOut },
        },
        .callback = MyeLocalFrom3D,
    });

    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "MyeLocalFrom2D",
                                      .add = ecs_ids(ecs_dependson(EcsPostUpdate)) }),
        .query.terms = {
            { .id = ecs_id(MyePosition2D), .inout = EcsIn },
            { .id = ecs_id(MyeRotation2D), .inout = EcsIn, .oper = EcsOptional },
            { .id = ecs_id(MyeScale2D), .inout = EcsIn, .oper = EcsOptional },
            { .id = ecs_id(MyeLocalTransform), .inout = EcsOut },
            /* 3D placement wins if an entity somehow has both. */
            { .id = ecs_id(MyePosition3D), .oper = EcsNot },
        },
        .callback = MyeLocalFrom2D,
    });

    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "MyePropagateTransforms",
                                      .add = ecs_ids(ecs_dependson(EcsPostUpdate)) }),
        .query.terms = {
            { .id = ecs_id(MyeLocalTransform), .inout = EcsIn },
            { .id = ecs_id(MyeWorldTransform), .inout = EcsOut },
            /* Parent's world transform, breadth-first so parents resolve
             * before children. Optional: roots have no parent. */
            { .id = ecs_id(MyeWorldTransform),
              .inout = EcsIn,
              .oper = EcsOptional,
              .src.id = EcsCascade,
              .trav = EcsChildOf },
        },
        .callback = MyePropagateTransforms,
    });

    ecs_observer(world, {
        .query.terms = {{ .id = ecs_id(MyeWorldTransform) }},
        .events = { EcsOnAdd },
        .callback = MyeAddRenderTransform,
    });

    ecs_observer(world, {
        .query.terms = {{ .id = ecs_id(MyeWorldTransform) }},
        .events = { EcsOnRemove },
        .callback = MyeRemoveRenderTransform,
    });
}
