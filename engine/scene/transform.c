#include "scene/transform.h"

#include "render/render2d.h"

#include <raymath.h>

ECS_COMPONENT_DECLARE(MyePosition3D);
ECS_COMPONENT_DECLARE(MyeRotation3D);
ECS_COMPONENT_DECLARE(MyeScale3D);
ECS_COMPONENT_DECLARE(MyeLocalTransform);
ECS_COMPONENT_DECLARE(MyeWorldTransform);

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

/* ------------------------------------------------------------- lifecycle -- */

void MyeTransformModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeTransformModule);

    ECS_COMPONENT_DEFINE(world, MyePosition3D);
    ECS_COMPONENT_DEFINE(world, MyeRotation3D);
    ECS_COMPONENT_DEFINE(world, MyeScale3D);
    ECS_COMPONENT_DEFINE(world, MyeLocalTransform);
    ECS_COMPONENT_DEFINE(world, MyeWorldTransform);

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
}
