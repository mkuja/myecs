#include "collision/collision.h"

#include "core/log.h"

#include <math.h>

ECS_COMPONENT_DECLARE(MyeCollider2D);
ECS_COMPONENT_DECLARE(MyeCollision2D);
ECS_COMPONENT_DECLARE(MyeVelocity2D);
ECS_COMPONENT_DECLARE(MyeVelocity3D);

/* ------------------------------------------------------- pure geometry -- */

/* Negative extents are not a shape. Clamping here, once, keeps every test
 * below free of the question -- and keeps a stray minus sign from reading as
 * "overlaps everything" through a squared comparison. */
static float non_negative(float v)
{
    return v > 0.0f ? v : 0.0f;
}

static MyeCircle circle_sane(MyeCircle c)
{
    c.radius = non_negative(c.radius);
    return c;
}

static MyeAabb aabb_sane(MyeAabb b)
{
    b.half_extents.x = non_negative(b.half_extents.x);
    b.half_extents.y = non_negative(b.half_extents.y);
    return b;
}

/* +1 for zero, so a degenerate axis still yields a unit normal. */
static float sign_or_positive(float v)
{
    return v < 0.0f ? -1.0f : 1.0f;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool mye_overlap_circle_circle(MyeCircle a, MyeCircle b, MyeOverlap *out)
{
    a = circle_sane(a);
    b = circle_sane(b);

    float dx = b.center.x - a.center.x;
    float dy = b.center.y - a.center.y;
    float sum = a.radius + b.radius;
    float dist2 = dx * dx + dy * dy;

    /* Compared squared, so no sqrt is paid on the overwhelmingly common
     * "these two are nowhere near each other" answer. Strict: exactly
     * touching is not overlapping. */
    if (dist2 >= sum * sum) {
        return false;
    }
    if (out == NULL) {
        return true;
    }

    float dist = sqrtf(dist2);
    Vector2 normal = { 1.0f, 0.0f }; /* concentric: no direction exists */
    if (dist > 0.0f) {
        normal = (Vector2){ dx / dist, dy / dist };
    }

    float depth = sum - dist;
    out->normal = normal;
    out->depth = depth;
    /* Middle of the lens the two circles share, measured out from a. */
    float along = a.radius - depth * 0.5f;
    out->point = (Vector2){ a.center.x + normal.x * along,
                            a.center.y + normal.y * along };
    return true;
}

bool mye_overlap_aabb_aabb(MyeAabb a, MyeAabb b, MyeOverlap *out)
{
    a = aabb_sane(a);
    b = aabb_sane(b);

    float dx = b.center.x - a.center.x;
    float dy = b.center.y - a.center.y;
    float ox = (a.half_extents.x + b.half_extents.x) - fabsf(dx);
    float oy = (a.half_extents.y + b.half_extents.y) - fabsf(dy);

    if (ox <= 0.0f || oy <= 0.0f) {
        return false;
    }
    if (out == NULL) {
        return true;
    }

    /* The axis of least penetration is the cheapest way out, which is what a
     * game separating them by hand wants. */
    if (ox < oy) {
        out->normal = (Vector2){ sign_or_positive(dx), 0.0f };
        out->depth = ox;
    } else {
        out->normal = (Vector2){ 0.0f, sign_or_positive(dy) };
        out->depth = oy;
    }

    float lo_x = fmaxf(a.center.x - a.half_extents.x,
                       b.center.x - b.half_extents.x);
    float hi_x = fminf(a.center.x + a.half_extents.x,
                       b.center.x + b.half_extents.x);
    float lo_y = fmaxf(a.center.y - a.half_extents.y,
                       b.center.y - b.half_extents.y);
    float hi_y = fminf(a.center.y + a.half_extents.y,
                       b.center.y + b.half_extents.y);
    out->point = (Vector2){ (lo_x + hi_x) * 0.5f, (lo_y + hi_y) * 0.5f };
    return true;
}

bool mye_overlap_circle_aabb(MyeCircle circle, MyeAabb box, MyeOverlap *out)
{
    circle = circle_sane(circle);
    box = aabb_sane(box);

    float dx = circle.center.x - box.center.x;
    float dy = circle.center.y - box.center.y;
    /* How far inside the box's slab the centre is, per axis. Negative means
     * the centre is outside on that axis. */
    float px = box.half_extents.x - fabsf(dx);
    float py = box.half_extents.y - fabsf(dy);

    if (px >= 0.0f && py >= 0.0f) {
        /* Centre inside the box, or exactly on its boundary. Containment
         * counts however small the circle is -- a zero-radius circle strictly
         * inside a box overlaps it -- but a zero-radius circle sitting
         * exactly on the edge only touches, and touching is not overlapping.
         * Both fall out of `depth > 0`. */
        float depth = fminf(px, py) + circle.radius;
        if (depth <= 0.0f) {
            return false;
        }
        if (out == NULL) {
            return true;
        }
        out->depth = depth;
        if (px < py) {
            /* Nearest face is on x. The circle escapes along +sign(dx), so
             * the normal -- which points circle -> box -- is the opposite. */
            out->normal = (Vector2){ -sign_or_positive(dx), 0.0f };
            out->point = (Vector2){ box.center.x + sign_or_positive(dx) *
                                                       box.half_extents.x,
                                    circle.center.y };
        } else {
            out->normal = (Vector2){ 0.0f, -sign_or_positive(dy) };
            out->point = (Vector2){ circle.center.x,
                                    box.center.y + sign_or_positive(dy) *
                                                       box.half_extents.y };
        }
        return true;
    }

    /* Outside on at least one axis, so the nearest point on the box is a
     * strictly positive distance away and the division below is safe. */
    Vector2 nearest = {
        clampf(circle.center.x, box.center.x - box.half_extents.x,
               box.center.x + box.half_extents.x),
        clampf(circle.center.y, box.center.y - box.half_extents.y,
               box.center.y + box.half_extents.y),
    };
    float ex = circle.center.x - nearest.x;
    float ey = circle.center.y - nearest.y;
    float dist2 = ex * ex + ey * ey;
    if (dist2 >= circle.radius * circle.radius) {
        return false;
    }
    if (out == NULL) {
        return true;
    }

    float dist = sqrtf(dist2);
    out->normal = (Vector2){ -ex / dist, -ey / dist };
    out->depth = circle.radius - dist;
    out->point = nearest;
    return true;
}

/* ------------------------------------------------------- collider pairs -- */

bool mye_collision_layers_match(uint32_t a_layers, uint32_t a_mask,
                                uint32_t b_layers, uint32_t b_mask)
{
    /* Symmetric: one side asking is enough. See the convention in
     * collision.h -- `layers` is what you ARE, `mask` is what you want to
     * hear about. */
    return (a_layers & b_mask) != 0u || (b_layers & a_mask) != 0u;
}

static MyeCircle collider_circle(const MyeCollider2D *c, Vector2 pos)
{
    return (MyeCircle){
        .center = { pos.x + c->offset.x, pos.y + c->offset.y },
        .radius = c->radius,
    };
}

static MyeAabb collider_aabb(const MyeCollider2D *c, Vector2 pos)
{
    return (MyeAabb){
        .center = { pos.x + c->offset.x, pos.y + c->offset.y },
        .half_extents = c->half_extents,
    };
}

bool mye_collider_overlap(const MyeCollider2D *a, Vector2 pos_a,
                          const MyeCollider2D *b, Vector2 pos_b,
                          MyeOverlap *out)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    bool a_circle = a->shape == MYE_COLLIDER_CIRCLE;
    bool b_circle = b->shape == MYE_COLLIDER_CIRCLE;

    if (a_circle && b_circle) {
        return mye_overlap_circle_circle(collider_circle(a, pos_a),
                                         collider_circle(b, pos_b), out);
    }
    if (!a_circle && !b_circle) {
        return mye_overlap_aabb_aabb(collider_aabb(a, pos_a),
                                     collider_aabb(b, pos_b), out);
    }
    if (a_circle) {
        return mye_overlap_circle_aabb(collider_circle(a, pos_a),
                                       collider_aabb(b, pos_b), out);
    }

    /* Box against circle. Only one mixed test exists, so run it the other way
     * round and flip the normal -- the contract is that it points from the
     * FIRST argument to the second, and swapping the arguments must not
     * quietly swap that too. */
    bool hit = mye_overlap_circle_aabb(collider_circle(b, pos_b),
                                       collider_aabb(a, pos_a), out);
    if (hit && out != NULL) {
        out->normal.x = -out->normal.x;
        out->normal.y = -out->normal.y;
    }
    return hit;
}

/* ------------------------------------------------------------- the event -- */

void mye_collision_emit(ecs_world_t *world, ecs_entity_t self,
                        ecs_entity_t other, const MyeOverlap *overlap)
{
    if (world == NULL || self == 0 || other == 0) {
        return;
    }
    /* An observer earlier in this same step may already have deleted one of
     * them. Deletes inside a system are deferred, so this is belt and braces
     * -- but emitting at a dead entity would dereference a null record. */
    if (!ecs_is_alive(world, self) || !ecs_is_alive(world, other)) {
        return;
    }

    MyeCollision2D info = { .self = self, .other = other };
    if (overlap != NULL) {
        info.overlap = *overlap;
    }

    /* Emitted against MyeCollider2D, not as a bare entity event, so an
     * observer can say "any collision, whoever it happens to" by querying for
     * the component instead of naming entities up front. Entity-scoped
     * observers (`{ EcsAny, .src.id = ship }`) still hear it: flecs checks
     * EcsAny observers before the ones keyed on the emitted id.
     *
     * The consequence, and it is a real one: an observer written that way
     * only hears pairs where its entity came out as `self`. Query for
     * MyeCollider2D and test both ends of the pair -- that is the pattern. */
    mye_event_emit(world, self, ecs_id(MyeCollision2D), ecs_id(MyeCollider2D),
                   &info);
}

/* ------------------------------------------------------------- detection -- */

/* Built once at import; building it per step would allocate every step. Held
 * in a singleton rather than a global so two worlds cannot clobber each
 * other -- the same arrangement render2d uses for its sprite query. */
typedef struct MyeCollisionState {
    ecs_query_t *colliders;
} MyeCollisionState;

ECS_COMPONENT_DECLARE(MyeCollisionState);

/* Where the collider actually sits. Mirrors render2d's draw-position
 * precedence with the display-only step removed: the hierarchy's world
 * transform first, the plain 2D position otherwise. Never
 * MyeRenderTransform -- that one is blended between fixed steps, and gameplay
 * reading it would make collisions depend on the framerate.
 *
 * Note that MyeWorldTransform is recomposed once per FRAME (EcsPostUpdate),
 * while this runs once per fixed STEP. A frame that runs several steps
 * therefore sees a parented collider at last frame's world position for all
 * of them. Roots -- which is nearly everything that collides -- are exact. */
static bool collider_center(const MyeCollider2D *collider,
                            const MyePosition2D *position,
                            const MyeWorldTransform *world_tf, Vector2 *out)
{
    if (world_tf != NULL) {
        Vector3 t = mye_matrix_translation(world_tf->m);
        *out = (Vector2){ t.x + collider->offset.x, t.y + collider->offset.y };
        return true;
    }
    if (position != NULL) {
        *out = (Vector2){ position->x + collider->offset.x,
                          position->y + collider->offset.y };
        return true;
    }
    return false; /* a collider with nowhere to be */
}

/* Every collider against every other, once per pair.
 *
 * O(n^2), deliberately and with no broadphase. At this engine's scale -- the
 * shipped game peaks at a few dozen colliders -- a grid or a BVH would cost
 * more in code, cache misses and rebuild time than the loop it replaces, and
 * plan/00 defers physics entirely. When something here actually has thousands
 * of colliders, that is the moment to measure and add one, not before.
 *
 * The outer loop is the system's own iteration; the inner is a second query
 * over the same set. Pairs are deduped by entity id (`b > a` only), which is
 * also what makes `self` the same end of the pair on every replay. */
static void MyeDetectCollisions2D(ecs_iter_t *it)
{
    ecs_world_t *world = it->world;
    const MyeCollisionState *state =
        ecs_singleton_get(world, MyeCollisionState);
    if (state == NULL || state->colliders == NULL) {
        return;
    }

    const MyeCollider2D *a_col = ecs_field(it, MyeCollider2D, 0);
    const MyePosition2D *a_pos = ecs_field(it, MyePosition2D, 1);
    const MyeWorldTransform *a_tf = ecs_field(it, MyeWorldTransform, 2);

    for (int i = 0; i < it->count; ++i) {
        ecs_entity_t a = it->entities[i];

        Vector2 center_a;
        if (!collider_center(&a_col[i], a_pos != NULL ? &a_pos[i] : NULL,
                             a_tf != NULL ? &a_tf[i] : NULL, &center_a)) {
            continue;
        }

        ecs_iter_t inner = ecs_query_iter(world, state->colliders);
        while (ecs_query_next(&inner)) {
            const MyeCollider2D *b_col = ecs_field(&inner, MyeCollider2D, 0);
            const MyePosition2D *b_pos = ecs_field(&inner, MyePosition2D, 1);
            const MyeWorldTransform *b_tf =
                ecs_field(&inner, MyeWorldTransform, 2);

            for (int j = 0; j < inner.count; ++j) {
                ecs_entity_t b = inner.entities[j];

                /* Each unordered pair exactly once: skips both the self-test
                 * and the mirror image of a pair already reported. */
                if (b <= a) {
                    continue;
                }
                if (!mye_collision_layers_match(a_col[i].layers, a_col[i].mask,
                                                b_col[j].layers,
                                                b_col[j].mask)) {
                    continue;
                }

                Vector2 center_b;
                if (!collider_center(&b_col[j],
                                     b_pos != NULL ? &b_pos[j] : NULL,
                                     b_tf != NULL ? &b_tf[j] : NULL,
                                     &center_b)) {
                    continue;
                }

                MyeOverlap overlap;
                if (!mye_collider_overlap(&a_col[i], center_a, &b_col[j],
                                          center_b, &overlap)) {
                    continue;
                }
                mye_collision_emit(world, a, b, &overlap);
            }
        }
    }
}

/* ------------------------------------------------------------- movement -- */

static void MyeIntegrateVelocity2D(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const MyeVelocity2D *vel = ecs_field(it, MyeVelocity2D, 1);
    /* Always exactly MyeTime.fixed_dt -- that is what the phase guarantees,
     * and what makes N steps land on the same position on every machine. */
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        pos[i].x += vel[i].x * dt;
        pos[i].y += vel[i].y * dt;
    }
}

static void MyeIntegrateVelocity3D(ecs_iter_t *it)
{
    MyePosition3D *pos = ecs_field(it, MyePosition3D, 0);
    const MyeVelocity3D *vel = ecs_field(it, MyeVelocity3D, 1);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        pos[i].v.x += vel[i].v.x * dt;
        pos[i].v.y += vel[i].v.y * dt;
        pos[i].v.z += vel[i].v.z * dt;
    }
}

/* --------------------------------------------------------------- warning -- */

/* Tag: already warned about, so a collider re-set every frame does not warn
 * every frame. */
typedef struct MyeColliderWarned {
    char unused;
} MyeColliderWarned;

ECS_COMPONENT_DECLARE(MyeColliderWarned);

/* layers = 0 and mask = 0 makes the pair test false against everything, for
 * ever: the collider is inert and nothing will ever say so. That is the one
 * silent failure this module has, so it is worth a line in the log. */
static void MyeWarnInertCollider(ecs_iter_t *it)
{
    const MyeCollider2D *colliders = ecs_field(it, MyeCollider2D, 0);
    ecs_world_t *world = it->world;

    for (int i = 0; i < it->count; ++i) {
        if (colliders[i].layers != 0u || colliders[i].mask != 0u) {
            continue;
        }
        ecs_entity_t e = it->entities[i];
        if (ecs_has(world, e, MyeColliderWarned)) {
            continue;
        }

        const char *name = ecs_get_name(world, e);
        mye_log_warn(
            "collider on '%s' has layers = 0 and mask = 0, so it can never "
            "collide with anything. Set `layers` to what this entity is, and "
            "`mask` to what it wants to be told about (MYE_LAYER_ALL for "
            "everything).",
            name != NULL ? name : "<unnamed>");
        ecs_add(world, e, MyeColliderWarned);
    }
}

/* ------------------------------------------------------------- lifecycle -- */

static void collision_fini(ecs_world_t *world, void *ctx)
{
    (void)world;
    MyeCollisionState *state = (MyeCollisionState *)ctx;
    if (state == NULL) {
        return;
    }
    if (state->colliders != NULL) {
        ecs_query_fini(state->colliders);
        state->colliders = NULL;
    }
}

void MyeCollisionModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeCollisionModule);

    /* Positions and world transforms must be registered before the queries
     * below can name them. */
    ECS_IMPORT(world, MyeTransformModule);

    ECS_COMPONENT_DEFINE(world, MyeCollider2D);
    ECS_COMPONENT_DEFINE(world, MyeCollision2D);
    ECS_COMPONENT_DEFINE(world, MyeVelocity2D);
    ECS_COMPONENT_DEFINE(world, MyeVelocity3D);
    ECS_COMPONENT_DEFINE(world, MyeColliderWarned);

    ECS_COMPONENT_DEFINE(world, MyeCollisionState);
    ecs_add_id(world, ecs_id(MyeCollisionState), EcsSingleton);
    ecs_singleton_set(world, MyeCollisionState, { 0 });
    MyeCollisionState *state = ecs_singleton_ensure(world, MyeCollisionState);

    /* Both placement terms are optional and neither is enough on its own:
     * an entity in the hierarchy may carry only MyeWorldTransform, a plain
     * sprite only MyePosition2D. collider_center() picks. */
    state->colliders = ecs_query(world, {
        .terms = {
            { .id = ecs_id(MyeCollider2D), .inout = EcsIn },
            { .id = ecs_id(MyePosition2D), .inout = EcsIn,
              .oper = EcsOptional },
            { .id = ecs_id(MyeWorldTransform), .inout = EcsIn,
              .oper = EcsOptional },
        },
    });

    /* Registration order IS execution order inside MyeOnFixedUpdate (flecs
     * orders a pipeline's systems by entity id). Move first, then look at
     * what overlaps -- reporting last step's positions would be a step late
     * on everything that moves. See the note in collision.h about why this
     * module imports after render2d. */
    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "MyeIntegrateVelocity2D",
                                      .add = ecs_ids(ecs_dependson(
                                          MyeOnFixedUpdate)) }),
        .query.terms = {
            { .id = ecs_id(MyePosition2D), .inout = EcsInOut },
            { .id = ecs_id(MyeVelocity2D), .inout = EcsIn },
        },
        .callback = MyeIntegrateVelocity2D,
    });

    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "MyeIntegrateVelocity3D",
                                      .add = ecs_ids(ecs_dependson(
                                          MyeOnFixedUpdate)) }),
        .query.terms = {
            { .id = ecs_id(MyePosition3D), .inout = EcsInOut },
            { .id = ecs_id(MyeVelocity3D), .inout = EcsIn },
        },
        .callback = MyeIntegrateVelocity3D,
    });

    ecs_system(world, {
        .entity = ecs_entity(world, { .name = "MyeDetectCollisions2D",
                                      .add = ecs_ids(ecs_dependson(
                                          MyeOnFixedUpdate)) }),
        .query.terms = {
            { .id = ecs_id(MyeCollider2D), .inout = EcsIn },
            { .id = ecs_id(MyePosition2D), .inout = EcsIn,
              .oper = EcsOptional },
            { .id = ecs_id(MyeWorldTransform), .inout = EcsIn,
              .oper = EcsOptional },
        },
        .callback = MyeDetectCollisions2D,
    });

    ecs_observer(world, {
        .query.terms = {{ .id = ecs_id(MyeCollider2D), .inout = EcsIn }},
        .events = { EcsOnSet },
        .callback = MyeWarnInertCollider,
    });

    ecs_atfini(world, collision_fini, state);
}
