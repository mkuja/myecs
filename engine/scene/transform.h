/* Transform hierarchy, shared by 2D and 3D. See plan/03-rendering.md.
 *
 * An entity's placement is authored as translation / rotation / scale, in
 * whichever dimensionality suits it, and the engine turns that into two
 * matrices:
 *
 *   MyeLocalTransform  -- placement relative to the parent
 *   MyeWorldTransform  -- placement in the world, after the parent chain
 *
 * Parenting uses flecs' own EcsChildOf relationship, so hierarchies are ECS
 * data rather than something the engine invents:
 *
 *   ecs_add_pair(world, turret, EcsChildOf, tank);
 *
 * Move the tank and the turret follows. Propagation runs breadth-first
 * (EcsCascade), so a parent's world matrix is always final before its
 * children read it, however deep the chain.
 */
#ifndef MYE_SCENE_TRANSFORM_H
#define MYE_SCENE_TRANSFORM_H

#include "core/engine.h"

#include <raylib.h>

/* ------------------------------------------------------------ 3D placement -- */

typedef struct MyePosition3D {
    Vector3 v;
} MyePosition3D;

typedef struct MyeRotation3D {
    Quaternion q;
} MyeRotation3D;

typedef struct MyeScale3D {
    Vector3 v;
} MyeScale3D;

/* --------------------------------------------------------------- matrices -- */

typedef struct MyeLocalTransform {
    Matrix m;
} MyeLocalTransform;

typedef struct MyeWorldTransform {
    Matrix m;
} MyeWorldTransform;

extern ECS_COMPONENT_DECLARE(MyePosition3D);
extern ECS_COMPONENT_DECLARE(MyeRotation3D);
extern ECS_COMPONENT_DECLARE(MyeScale3D);
extern ECS_COMPONENT_DECLARE(MyeLocalTransform);
extern ECS_COMPONENT_DECLARE(MyeWorldTransform);

void MyeTransformModuleImport(ecs_world_t *world);

/* Gives an entity the components the hierarchy needs, at the given position.
 * Rotation and scale are optional and default to identity. */
ecs_entity_t mye_spawn_3d(ecs_world_t *world, Vector3 position);

/* Parents `child` to `parent`; the child's local transform is then relative
 * to the parent. Passing 0 as parent detaches it. */
void mye_set_parent(ecs_world_t *world, ecs_entity_t child,
                    ecs_entity_t parent);

/* World-space position of an entity, i.e. the translation column of its
 * world matrix. Zero vector if it has no world transform yet. */
Vector3 mye_world_position(const ecs_world_t *world, ecs_entity_t entity);

/* ------------------------------------------------------------------ maths -- */

/* Composes translation * rotation * scale, in that application order: scale
 * first, then rotate, then translate. Pure, so the tests do not need a
 * world. */
Matrix mye_trs_matrix(Vector3 translation, Quaternion rotation, Vector3 scale);

/* Translation column of a matrix. */
Vector3 mye_matrix_translation(Matrix m);

#endif /* MYE_SCENE_TRANSFORM_H */
