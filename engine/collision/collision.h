/* 2D collision: overlap tests, collision events, and velocity integration.
 * See plan/00-overview.md Tier 2 ("AABB + circle overlap tests and collision
 * events -- not a physics engine").
 *
 * WHAT THIS MODULE DOES NOT DO. It never resolves anything. No impulses, no
 * separation, no restitution, no friction, no sleeping, no broadphase. It
 * finds pairs of overlapping colliders and tells you; deciding what an
 * overlap MEANS -- destroy the bullet, bounce the ball, open the door -- is
 * the game's, and the engine has no business arbitrating it. `MyeOverlap`
 * carries a normal and a depth so a game that wants to push things apart can,
 * in its own system, on its own terms.
 *
 * The name is deliberate: this is `collision`, not `physics2d`, because
 * "physics" would advertise dynamics that are explicitly out of scope
 * (plan/00 non-goals: "Writing our own physics engine").
 *
 * OPT-IN. Nothing is tested unless it has a MyeCollider2D, and nothing moves
 * unless it has a MyeVelocity2D/3D. An entity without them is never touched.
 *
 * THREE LAYERS, USABLE SEPARATELY:
 *
 *   1. Pure geometry -- mye_overlap_*(). Plain structs in, bool out, no
 *      world, no components. Unit-testable and callable from anywhere.
 *   2. Components -- MyeCollider2D on an entity, positioned by the entity's
 *      world position plus an offset.
 *   3. The MyeOnFixedUpdate pass -- integrates velocities, then emits one
 *      MyeCollision2D event per overlapping pair per step.
 *
 * A GAME LISTENING FOR COLLISIONS:
 *
 *   static void OnCollision(ecs_iter_t *it)
 *   {
 *       const MyeCollision2D *c = it->param;
 *       // c->self and c->other; c->overlap.normal points self -> other.
 *   }
 *
 *   ecs_observer(world, {
 *       .query.terms = {{ .id = ecs_id(MyeCollider2D) }},
 *       .events = { ecs_id(MyeCollision2D) },
 *       .callback = OnCollision,
 *   });
 *
 * The observer runs synchronously, inside the fixed step, with the world in
 * its usual deferred mode -- so ecs_delete() and ecs_set() from it are safe
 * and land at the end of the step, exactly as they do from a system.
 */
#ifndef MYE_COLLISION_COLLISION_H
#define MYE_COLLISION_COLLISION_H

#include "core/engine.h"
#include "scene/transform.h"

#include <raylib.h>

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------- pure geometry -- */

typedef struct MyeCircle {
    Vector2 center;
    float radius;
} MyeCircle;

/* Axis-aligned box, as centre + half extents -- the form that makes the
 * overlap test a subtraction instead of four comparisons. */
typedef struct MyeAabb {
    Vector2 center;
    Vector2 half_extents;
} MyeAabb;

/* Optional detail about an overlap, filled in only when a test returns true.
 *
 *   normal  unit vector pointing from the FIRST shape to the SECOND, along
 *           the axis of least separation.
 *   depth   how far along -normal the first shape must move (or +normal the
 *           second) to stop overlapping. Always > 0.
 *   point   a representative contact point: the middle of the overlapping
 *           region, or the nearest point on a box.
 *
 * Degenerate cases (two concentric circles, boxes with the same centre) have
 * no defined direction; the normal is then (1, 0) or an axis unit vector, and
 * the depth is still correct. */
typedef struct MyeOverlap {
    Vector2 normal;
    float depth;
    Vector2 point;
} MyeOverlap;

/* Overlap tests. `out` may be NULL, and is written only when the result is
 * true.
 *
 * TOUCHING IS NOT OVERLAPPING. Two circles exactly `r1 + r2` apart, two boxes
 * sharing an edge, a point exactly on a box's boundary: all false. The test
 * is strict (`<`, not `<=`) so a row of tiles laid edge to edge does not emit
 * an event per seam every step.
 *
 * CONTAINMENT IS OVERLAPPING. A small shape wholly inside a large one
 * overlaps it, and the depth reflects how far it would have to travel to get
 * out.
 *
 * ZERO SIZE is legal and follows the same two rules: a zero-radius circle
 * strictly inside a box overlaps it; two zero-radius circles at the same
 * point do not (they touch). Negative radii and half extents are not
 * meaningful and are treated as zero. */
bool mye_overlap_circle_circle(MyeCircle a, MyeCircle b, MyeOverlap *out);
bool mye_overlap_aabb_aabb(MyeAabb a, MyeAabb b, MyeOverlap *out);
bool mye_overlap_circle_aabb(MyeCircle circle, MyeAabb box, MyeOverlap *out);

/* ------------------------------------------------------------ components -- */

typedef enum MyeColliderShape {
    MYE_COLLIDER_CIRCLE = 0, /* the default, so a zeroed collider is a circle */
    MYE_COLLIDER_AABB = 1,
} MyeColliderShape;

/* Layer bits.
 *
 *   layers  what this collider IS.            "I am a bullet."
 *   mask    what this collider WANTS TO HEAR. "Tell me about rocks."
 *
 * A pair is tested when either side is interested in the other:
 *
 *     (a.layers & b.mask) != 0 || (b.layers & a.mask) != 0
 *
 * One collider asking is enough, and the pair still produces exactly ONE
 * event -- the check is symmetric on purpose, so a game never has to set the
 * same relationship up from both ends.
 *
 * A wall that only ever needs to be hit can leave `mask` at 0: bullets ask
 * about it. A collider with BOTH at 0 can never collide with anything, which
 * is almost always a mistake, so the module logs a warning once for it.
 *
 * The bits mean whatever the game decides. MYE_LAYER(n) names one. */
#define MYE_LAYER_NONE 0u
#define MYE_LAYER_ALL 0xFFFFFFFFu
#define MYE_LAYER(n) ((uint32_t)1u << (n))

/* Attach to any entity that should take part in collision detection.
 *
 *   ecs_set(world, bullet, MyeCollider2D, {
 *       .shape = MYE_COLLIDER_CIRCLE, .radius = 3.0f,
 *       .layers = MYE_LAYER(1), .mask = MYE_LAYER(2),
 *   });
 *
 * WHERE IT SITS. The collider is centred on the entity's world position plus
 * `offset`:
 *
 *   1. MyeWorldTransform's translation, when the entity takes part in the
 *      transform hierarchy -- the same simulated position mye_world_position()
 *      returns, and the one the fixed step is supposed to read;
 *   2. otherwise MyePosition2D.
 *
 * That is the render2d draw-position precedence with the display-only step
 * removed: rendering blends between fixed steps, and gameplay must not, or
 * collisions would depend on the framerate.
 *
 * An entity with neither component is skipped -- a collider needs somewhere
 * to be.
 *
 * `offset` is in world axes and is NOT rotated by MyeRotation2D or by a
 * parent's rotation. An AABB cannot rotate anyway, so rotating the offset
 * alone would be half a feature; a rotating hitbox is a circle at an offset
 * you update yourself, or a later milestone. */
typedef struct MyeCollider2D {
    MyeColliderShape shape;

    float radius;         /* MYE_COLLIDER_CIRCLE only */
    Vector2 half_extents; /* MYE_COLLIDER_AABB only */

    Vector2 offset; /* from the entity's world position */

    uint32_t layers;
    uint32_t mask;
} MyeCollider2D;

/* The payload of a collision event, delivered as `it->param`.
 *
 * Emitted ONCE per overlapping pair per fixed step, on `self`. Which of the
 * two entities is `self` is decided by entity id (the lower one), so a replay
 * of the same simulation reports the same pair the same way round.
 *
 * `overlap.normal` points from `self` to `other`. */
typedef struct MyeCollision2D {
    ecs_entity_t self;
    ecs_entity_t other;
    MyeOverlap overlap;
} MyeCollision2D;

/* ------------------------------------------------------------- movement -- */

/* World units per second, added to MyePosition2D/3D once per fixed step.
 *
 * Opt-in: an entity without one is never moved by the engine. Integration is
 * plain Euler -- position += velocity * fixed_dt -- which is exact for
 * constant velocity and is all a non-physics engine owes you. Nothing here
 * applies gravity, drag, or a maximum speed; a game that wants those writes
 * its own MyeOnFixedUpdate system over MyeVelocity2D. */
typedef struct MyeVelocity2D {
    float x, y;
} MyeVelocity2D;

typedef struct MyeVelocity3D {
    Vector3 v;
} MyeVelocity3D;

extern ECS_COMPONENT_DECLARE(MyeCollider2D);
extern ECS_COMPONENT_DECLARE(MyeCollision2D);
extern ECS_COMPONENT_DECLARE(MyeVelocity2D);
extern ECS_COMPONENT_DECLARE(MyeVelocity3D);

/* Registers the components and the fixed-step systems.
 *
 * ORDER MATTERS, and module import order is how this engine expresses it.
 * Systems inside MyeOnFixedUpdate run in creation order, so this module must
 * be imported AFTER render2d: its MyeCapturePrevPositions has to snapshot
 * where an entity was BEFORE the step's movement, or interpolation blends a
 * position against itself and every interpolated entity visibly stutters.
 * The resulting fixed step is:
 *
 *   MyeCapturePrevPositions  (render2d)  where things were
 *   MyeIntegrateVelocity2D/3D            move them
 *   MyeDetectCollisions2D                report what now overlaps
 *
 * The same rule has a consequence worth knowing: a system the GAME registers
 * in MyeOnFixedUpdate is created after all of these, so it runs after
 * detection. Move things with MyeVelocity2D and collisions are reported from
 * this step's positions; move them from your own fixed system and detection
 * sees them one step later. That is the reason the velocity components live
 * in this module rather than beside the positions they integrate. */
void MyeCollisionModuleImport(ecs_world_t *world);

/* ---------------------------------------------------------------- helpers -- */

/* The layer/mask rule above, as a function -- so a game can pre-filter with
 * exactly the test the detection pass uses. */
bool mye_collision_layers_match(uint32_t a_layers, uint32_t a_mask,
                                uint32_t b_layers, uint32_t b_mask);

/* Shape-dispatching overlap test for two colliders at two world positions.
 * `pos_a` / `pos_b` are the entities' world positions; each collider's own
 * `offset` is applied here, so callers pass the position, not the centre.
 *
 * Pure: no world, no entities. This is the whole of what the detection pass
 * does per pair, which makes the pair logic testable on its own. */
bool mye_collider_overlap(const MyeCollider2D *a, Vector2 pos_a,
                          const MyeCollider2D *b, Vector2 pos_b,
                          MyeOverlap *out);

/* Emits a collision event by hand, exactly as the detection pass does.
 *
 * For a game that does its own overlap test -- a swept query, a trigger
 * volume, a hand-rolled broadphase -- and wants the result to arrive through
 * the same observers as everything else. `overlap` may be NULL. */
void mye_collision_emit(ecs_world_t *world, ecs_entity_t self,
                        ecs_entity_t other, const MyeOverlap *overlap);

#endif /* MYE_COLLISION_COLLISION_H */
