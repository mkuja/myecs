/* Integration test for the collision module's ECS layer: the fixed-step pass,
 * the events it emits, and the velocity integration that runs just before it.
 * See plan/00-overview.md Tier 2 and engine/collision/collision.h.
 *
 * The mechanism under test is flecs' own event delivery -- a game registers an
 * ordinary observer and the engine's ecs_emit reaches it. Everything else here
 * (dedupe, filtering, opt-in) is about what the engine chooses to emit.
 */
#include "collision/collision.h"
#include "core/engine.h"
#include "render/render2d.h" /* MyeInterpolate, for the step-order test */

#include "mye_test.h"

#define FIXED_DT (1.0f / 60.0f)

/* ------------------------------------------------------------ the listener -- */

static int g_hits;
static MyeCollision2D g_last;
static ecs_world_t *g_deleting_world;
static ecs_entity_t g_delete_target;

static void OnCollision(ecs_iter_t *it)
{
    const MyeCollision2D *collision = it->param;
    ++g_hits;
    if (collision != NULL) {
        g_last = *collision;
    }
}

/* Deletes one participant from inside the observer -- the thing a real game
 * does the moment a bullet lands. */
static void OnCollisionDelete(ecs_iter_t *it)
{
    const MyeCollision2D *collision = it->param;
    ++g_hits;
    if (collision != NULL && g_deleting_world != NULL) {
        g_delete_target = collision->other;
        ecs_delete(it->world, collision->other);
    }
}

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){
        .headless = true,
        .fixed_dt = FIXED_DT,
        .max_steps_per_frame = 5,
    });
}

/* An observer over the collider component: "tell me about any collision, to
 * anything". This is the shape a game is meant to copy. */
static void listen_for_collisions(ecs_world_t *world, ecs_iter_action_t cb)
{
    ecs_observer(world, {
        .query.terms = {{ .id = ecs_id(MyeCollider2D), .inout = EcsIn }},
        .events = { ecs_id(MyeCollision2D) },
        .callback = cb,
    });
}

static ecs_entity_t spawn_circle(ecs_world_t *world, float x, float y, float r,
                                 uint32_t layers, uint32_t mask)
{
    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, MyePosition2D, { x, y });
    ecs_set(world, e, MyeCollider2D,
            { .shape = MYE_COLLIDER_CIRCLE,
              .radius = r,
              .layers = layers,
              .mask = mask });
    return e;
}

/* ------------------------------------------------------------------ events -- */

TEST(two_overlapping_circles_fire_one_event_per_step)
{
    g_hits = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    listen_for_collisions(world, OnCollision);

    ecs_entity_t a = spawn_circle(world, 0.0f, 0.0f, 10.0f, MYE_LAYER(0),
                                  MYE_LAYER_ALL);
    ecs_entity_t b = spawn_circle(world, 15.0f, 0.0f, 10.0f, MYE_LAYER(0),
                                  MYE_LAYER_ALL);

    /* One frame, one fixed step: the pair is reported ONCE, not once from
     * each end. */
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, g_hits);

    /* Both entities are named, self first, and the normal points self ->
     * other. `self` is the lower entity id, so a replay reports the same pair
     * the same way round. */
    ASSERT_EQ_U64(a, g_last.self);
    ASSERT_EQ_U64(b, g_last.other);
    ASSERT_NEAR(1.0, (double)g_last.overlap.normal.x, 1e-5);
    ASSERT_NEAR(5.0, (double)g_last.overlap.depth, 1e-4);

    /* Still overlapping next step: still exactly one more. */
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(2, g_hits);

    /* Two steps in one frame means two reports -- the pass is per STEP, which
     * is the rate gameplay is allowed to depend on. */
    g_hits = 0;
    mye_progress(world, FIXED_DT * 2.0f);
    ASSERT_EQ_INT(2, g_hits);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(three_mutually_overlapping_circles_fire_three_pairs)
{
    /* n*(n-1)/2, each pair once: the check that dedupe is by pair and not by
     * entity. */
    g_hits = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    listen_for_collisions(world, OnCollision);

    spawn_circle(world, 0.0f, 0.0f, 10.0f, MYE_LAYER(0), MYE_LAYER_ALL);
    spawn_circle(world, 5.0f, 0.0f, 10.0f, MYE_LAYER(0), MYE_LAYER_ALL);
    spawn_circle(world, 0.0f, 5.0f, 10.0f, MYE_LAYER(0), MYE_LAYER_ALL);

    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(3, g_hits);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(circles_that_only_touch_fire_nothing)
{
    g_hits = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    listen_for_collisions(world, OnCollision);

    /* Exactly r + r apart. A wall of tiles laid edge to edge must stay
     * silent, for ever. */
    spawn_circle(world, 0.0f, 0.0f, 10.0f, MYE_LAYER(0), MYE_LAYER_ALL);
    spawn_circle(world, 20.0f, 0.0f, 10.0f, MYE_LAYER(0), MYE_LAYER_ALL);

    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(0, g_hits);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(layers_and_mask_filter_which_pairs_are_tested)
{
    const uint32_t bullet = MYE_LAYER(0);
    const uint32_t rock = MYE_LAYER(1);
    const uint32_t ghost = MYE_LAYER(2);

    g_hits = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    listen_for_collisions(world, OnCollision);

    /* All three sit on top of each other; only the layer rule separates
     * them. The bullet asks about rocks and nothing else. */
    ecs_entity_t shot = spawn_circle(world, 0.0f, 0.0f, 10.0f, bullet, rock);
    ecs_entity_t stone = spawn_circle(world, 1.0f, 0.0f, 10.0f, rock,
                                      MYE_LAYER_NONE);
    spawn_circle(world, 2.0f, 0.0f, 10.0f, ghost, ghost);

    mye_progress(world, FIXED_DT);

    /* Exactly one pair survives the filter: bullet/rock. The ghost overlaps
     * both and is reported against neither. */
    ASSERT_EQ_INT(1, g_hits);
    ASSERT_EQ_U64(shot, g_last.self);
    ASSERT_EQ_U64(stone, g_last.other);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(entities_without_a_collider_are_never_tested)
{
    g_hits = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    listen_for_collisions(world, OnCollision);

    /* Opt-in: a position alone buys nothing. Three entities stacked at the
     * origin, one collider between them. */
    ecs_entity_t bare_a = mye_entity_new(world);
    ecs_set(world, bare_a, MyePosition2D, { 0.0f, 0.0f });
    ecs_entity_t bare_b = mye_entity_new(world);
    ecs_set(world, bare_b, MyePosition2D, { 0.0f, 0.0f });
    spawn_circle(world, 0.0f, 0.0f, 10.0f, MYE_LAYER(0), MYE_LAYER_ALL);

    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(0, g_hits);

    /* Give one of them a collider and the pair appears. */
    ecs_set(world, bare_a, MyeCollider2D,
            { .shape = MYE_COLLIDER_CIRCLE,
              .radius = 10.0f,
              .layers = MYE_LAYER(0),
              .mask = MYE_LAYER_ALL });
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, g_hits);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_boxed_collider_meets_a_circle_one)
{
    g_hits = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    listen_for_collisions(world, OnCollision);

    ecs_entity_t wall = mye_entity_new(world);
    ecs_set(world, wall, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, wall, MyeCollider2D,
            { .shape = MYE_COLLIDER_AABB,
              .half_extents = { 20.0f, 5.0f },
              .layers = MYE_LAYER(1),
              .mask = MYE_LAYER_NONE });

    /* Just inside the top face. */
    spawn_circle(world, 0.0f, 9.0f, 5.0f, MYE_LAYER(0), MYE_LAYER(1));

    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, g_hits);
    ASSERT_EQ_U64(wall, g_last.self); /* the wall was created first */
    ASSERT_NEAR(1.0, (double)g_last.overlap.depth, 1e-4);

    /* Clear of it: nothing. */
    g_hits = 0;
    ecs_entity_t high = spawn_circle(world, 0.0f, 100.0f, 5.0f, MYE_LAYER(0),
                                     MYE_LAYER(1));
    (void)high;
    ecs_delete(world, g_last.other);
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(0, g_hits);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(a_collider_in_a_hierarchy_uses_its_world_position)
{
    g_hits = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    listen_for_collisions(world, OnCollision);

    /* A turret parented to a tank far from the origin. Its MyePosition2D is
     * an OFFSET, not a place; reading it directly would put the collider at
     * (0, 0) and it would never meet the target. */
    ecs_entity_t tank = mye_spawn_2d(world, (Vector2){ 400.0f, 0.0f });
    ecs_entity_t turret = mye_spawn_2d(world, (Vector2){ 0.0f, 0.0f });
    mye_set_parent(world, turret, tank);
    ecs_set(world, turret, MyeCollider2D,
            { .shape = MYE_COLLIDER_CIRCLE,
              .radius = 10.0f,
              .layers = MYE_LAYER(0),
              .mask = MYE_LAYER_ALL });

    ecs_entity_t target = spawn_circle(world, 405.0f, 0.0f, 10.0f,
                                       MYE_LAYER(0), MYE_LAYER_ALL);

    /* World transforms are composed once per frame in EcsPostUpdate, so the
     * very first fixed step of a brand new hierarchy still sees identity.
     * From the second frame on it is settled. */
    mye_progress(world, FIXED_DT);
    g_hits = 0;
    mye_progress(world, FIXED_DT);

    ASSERT_EQ_INT(1, g_hits);
    ASSERT_EQ_U64(turret, g_last.self);
    ASSERT_EQ_U64(target, g_last.other);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(an_observer_may_delete_a_participant)
{
    /* The commonest thing a game does with a collision event. Deletes inside
     * a system are deferred to the end of the step, so the rest of the pass
     * keeps running against live entities. */
    g_hits = 0;
    g_delete_target = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    g_deleting_world = world;
    listen_for_collisions(world, OnCollisionDelete);

    spawn_circle(world, 0.0f, 0.0f, 10.0f, MYE_LAYER(0), MYE_LAYER_ALL);
    ecs_entity_t rock = spawn_circle(world, 5.0f, 0.0f, 10.0f, MYE_LAYER(0),
                                     MYE_LAYER_ALL);

    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, g_hits);
    ASSERT_EQ_U64(rock, g_delete_target);
    ASSERT_FALSE(ecs_is_alive(world, rock));

    /* And with it gone, the pair stops being reported. */
    g_hits = 0;
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(0, g_hits);

    g_deleting_world = NULL;
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(deleting_from_an_observer_does_not_disturb_the_pass)
{
    g_hits = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    g_deleting_world = world;
    listen_for_collisions(world, OnCollisionDelete);

    /* Four colliders in a heap: six pairs. The observer deletes one end of
     * every one of them, and the pass must still run all six -- a delete
     * inside a system is deferred to the end of the step, so the tables the
     * detection loop is walking never change under it. */
    for (int i = 0; i < 4; ++i) {
        spawn_circle(world, (float)i, 0.0f, 10.0f, MYE_LAYER(0),
                     MYE_LAYER_ALL);
    }

    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(6, g_hits);

    /* Everything that was ever `other` is gone; one collider is left and a
     * lone collider has no pair. */
    g_hits = 0;
    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(0, g_hits);

    g_deleting_world = NULL;
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* -------------------------------------------------- events, on their own -- */

typedef struct Damage {
    int amount;
} Damage;

static int g_damage_events;
static int g_damage_total;

static void OnDamage(ecs_iter_t *it)
{
    const Damage *d = it->param;
    ++g_damage_events;
    if (d != NULL) {
        g_damage_total += d->amount;
    }
}

TEST(a_game_can_emit_and_observe_its_own_events)
{
    /* The Tier-2 "gameplay events" deliverable, without a collider in sight:
     * a game defines an event type, watches one entity for it, and emits. */
    g_damage_events = 0;
    g_damage_total = 0;

    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ECS_COMPONENT(world, Damage);

    ecs_entity_t hero = mye_entity_new(world);
    ecs_entity_t bystander = mye_entity_new(world);

    ecs_observer(world, {
        .query.terms = {{ .id = EcsAny, .src.id = hero }},
        .events = { ecs_id(Damage) },
        .callback = OnDamage,
    });

    mye_event_emit(world, hero, ecs_id(Damage), 0, &(Damage){ .amount = 7 });
    ASSERT_EQ_INT(1, g_damage_events);
    ASSERT_EQ_INT(7, g_damage_total);

    /* Same event, different entity: this observer is scoped to `hero` and
     * must stay quiet. */
    mye_event_emit(world, bystander, ecs_id(Damage), 0,
                   &(Damage){ .amount = 100 });
    ASSERT_EQ_INT(1, g_damage_events);
    ASSERT_EQ_INT(7, g_damage_total);

    /* Emitting at a dead entity is a no-op, not a crash -- gameplay code
     * emits at whatever it just touched. */
    ecs_delete(world, hero);
    mye_event_emit(world, hero, ecs_id(Damage), 0, &(Damage){ .amount = 1 });
    ASSERT_EQ_INT(1, g_damage_events);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(an_entity_observer_hears_the_collisions_it_leads)
{
    /* Collision events are emitted against MyeCollider2D, which is what lets
     * the "any collision" observer above work. An observer scoped to a single
     * entity still hears the pairs that entity leads. */
    g_hits = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t ship = spawn_circle(world, 0.0f, 0.0f, 10.0f, MYE_LAYER(0),
                                     MYE_LAYER_ALL);
    ecs_entity_t rock = spawn_circle(world, 5.0f, 0.0f, 10.0f, MYE_LAYER(0),
                                     MYE_LAYER_ALL);

    ecs_observer(world, {
        .query.terms = {{ .id = EcsAny, .src.id = ship }},
        .events = { ecs_id(MyeCollision2D) },
        .callback = OnCollision,
    });

    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, g_hits);
    ASSERT_EQ_U64(ship, g_last.self);
    ASSERT_EQ_U64(rock, g_last.other);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* ---------------------------------------------------------------- velocity -- */

TEST(velocity_moves_an_entity_by_exactly_fixed_dt_per_step)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t mover = mye_entity_new(world);
    ecs_set(world, mover, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, mover, MyeVelocity2D, { 60.0f, -30.0f });

    /* An entity with a position and no velocity must not be touched: the
     * component is the opt-in. */
    ecs_entity_t still = mye_entity_new(world);
    ecs_set(world, still, MyePosition2D, { 7.0f, 7.0f });

    for (int i = 0; i < 60; ++i) {
        mye_progress(world, FIXED_DT);
    }

    /* One second at 60 u/s. Exactly 60 steps of exactly 1/60 s. */
    const MyePosition2D *p = ecs_get(world, mover, MyePosition2D);
    ASSERT_NOT_NULL(p);
    ASSERT_NEAR(60.0, (double)p->x, 1e-3);
    ASSERT_NEAR(-30.0, (double)p->y, 1e-3);

    const MyePosition2D *q = ecs_get(world, still, MyePosition2D);
    ASSERT_NOT_NULL(q);
    ASSERT_NEAR(7.0, (double)q->x, 1e-9);
    ASSERT_NEAR(7.0, (double)q->y, 1e-9);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(velocity_integrates_the_same_way_however_the_frames_fall)
{
    /* The property the fixed step exists for, applied to the engine's own
     * integrator: the same total elapsed time gives bit-identical positions,
     * whatever the frame pacing. Integrating with the FRAME delta instead of
     * fixed_dt breaks exactly this and nothing else. */
    const float steady[] = { FIXED_DT, FIXED_DT, FIXED_DT, FIXED_DT,
                             FIXED_DT, FIXED_DT, FIXED_DT, FIXED_DT };
    const float ragged[] = { FIXED_DT * 2.0f,  FIXED_DT * 0.5f,
                             FIXED_DT * 0.5f,  FIXED_DT * 3.0f,
                             FIXED_DT * 0.25f, FIXED_DT * 0.25f,
                             FIXED_DT * 1.5f };
    float x[2] = { 0.0f, 0.0f };
    float z[2] = { 0.0f, 0.0f };

    for (int run = 0; run < 2; ++run) {
        ecs_world_t *world = make_world();
        ASSERT_NOT_NULL(world);

        ecs_entity_t flat = mye_entity_new(world);
        ecs_set(world, flat, MyePosition2D, { 0.0f, 0.0f });
        ecs_set(world, flat, MyeVelocity2D, { 123.0f, 0.0f });

        ecs_entity_t deep = mye_entity_new(world);
        ecs_set(world, deep, MyePosition3D, { { 0.0f, 0.0f, 0.0f } });
        ecs_set(world, deep, MyeVelocity3D, { { 0.0f, 0.0f, 45.0f } });

        const float *frames = run == 0 ? steady : ragged;
        size_t count = run == 0 ? sizeof steady / sizeof steady[0]
                                : sizeof ragged / sizeof ragged[0];
        for (size_t i = 0; i < count; ++i) {
            mye_progress(world, frames[i]);
        }

        const MyePosition2D *p = ecs_get(world, flat, MyePosition2D);
        const MyePosition3D *v = ecs_get(world, deep, MyePosition3D);
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(v);
        x[run] = p->x;
        z[run] = v->v.z;

        ASSERT_EQ_INT(0, mye_shutdown(world));
    }

    /* Both runs cover eight steps of 1/60 s. */
    ASSERT_NEAR(123.0 * 8.0 / 60.0, (double)x[0], 1e-4);
    ASSERT_EQ_U64((uint64_t)(x[0] * 10000.0f), (uint64_t)(x[1] * 10000.0f));
    ASSERT_EQ_U64((uint64_t)(z[0] * 10000.0f), (uint64_t)(z[1] * 10000.0f));
}

TEST(velocity_moves_after_interpolation_has_captured_the_old_position)
{
    /* The fixed step's running order is capture -> integrate -> detect, and
     * it is expressed by nothing more than module import order (see
     * collision.h). Get it wrong and render2d's MyeCapturePrevPositions
     * snapshots the position AFTER the move, so `prev` equals the current
     * position, every blend is a no-op, and every interpolated entity
     * stutters at exactly the fixed rate. Nothing else would notice. */
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ecs_entity_t mover = mye_entity_new(world);
    ecs_set(world, mover, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, mover, MyeVelocity2D, { 60.0f, 0.0f });
    ecs_set(world, mover, MyeInterpolate, { 0.0f, 0.0f, false });

    mye_progress(world, FIXED_DT);

    const MyePosition2D *p = ecs_get(world, mover, MyePosition2D);
    const MyeInterpolate *i = ecs_get(world, mover, MyeInterpolate);
    ASSERT_NOT_NULL(p);
    ASSERT_NOT_NULL(i);
    ASSERT_NEAR(1.0, (double)p->x, 1e-4);   /* one step of 60 u/s */
    ASSERT_NEAR(0.0, (double)i->prev_x, 1e-9); /* where it was before it */

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(movement_is_reported_by_collision_in_the_same_step)
{
    /* The fixed step's order: capture, integrate, detect. A collision must be
     * reported from where the entity is NOW, not where it was a step ago --
     * otherwise everything fast is detected late, which is how bullets pass
     * through walls. */
    g_hits = 0;
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);
    listen_for_collisions(world, OnCollision);

    ecs_entity_t wall = spawn_circle(world, 0.0f, 0.0f, 10.0f, MYE_LAYER(0),
                                     MYE_LAYER_ALL);
    (void)wall;

    /* Overlap begins below 20 units of separation. The bullet starts at 35 --
     * clear -- and closes 18 in a single step, so the collision exists only
     * if detection reads the position integration wrote this same step. */
    ecs_entity_t bullet = spawn_circle(world, 35.0f, 0.0f, 10.0f, MYE_LAYER(0),
                                       MYE_LAYER_ALL);
    ecs_set(world, bullet, MyeVelocity2D, { -18.0f * 60.0f, 0.0f });

    mye_progress(world, FIXED_DT);
    ASSERT_EQ_INT(1, g_hits);

    const MyePosition2D *p = ecs_get(world, bullet, MyePosition2D);
    ASSERT_NOT_NULL(p);
    ASSERT_NEAR(17.0, (double)p->x, 1e-3);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(two_overlapping_circles_fire_one_event_per_step),
          TEST_CASE(three_mutually_overlapping_circles_fire_three_pairs),
          TEST_CASE(circles_that_only_touch_fire_nothing),
          TEST_CASE(layers_and_mask_filter_which_pairs_are_tested),
          TEST_CASE(entities_without_a_collider_are_never_tested),
          TEST_CASE(a_boxed_collider_meets_a_circle_one),
          TEST_CASE(a_collider_in_a_hierarchy_uses_its_world_position),
          TEST_CASE(an_observer_may_delete_a_participant),
          TEST_CASE(deleting_from_an_observer_does_not_disturb_the_pass),
          TEST_CASE(a_game_can_emit_and_observe_its_own_events),
          TEST_CASE(an_entity_observer_hears_the_collisions_it_leads),
          TEST_CASE(velocity_moves_an_entity_by_exactly_fixed_dt_per_step),
          TEST_CASE(velocity_integrates_the_same_way_however_the_frames_fall),
          TEST_CASE(velocity_moves_after_interpolation_has_captured_the_old_position),
          TEST_CASE(movement_is_reported_by_collision_in_the_same_step))
