/* Asteroids: game logic, separated from main() so it can be driven headlessly
 * by tests/integration/test_int_asteroids.c. See plan/09-testing.md. */
#ifndef ASTEROIDS_H
#define ASTEROIDS_H

#include "asset/asset.h"
#include "audio/audio.h"
#include "core/engine.h"
#include "input/input.h"
#include "render/render2d.h"

#define SCREEN_W 1280
#define SCREEN_H 720

#define SHIP_TURN_SPEED 3.2f /* radians per second */
#define SHIP_THRUST 260.0f   /* pixels per second squared */
#define SHIP_DRAG 0.6f
#define SHIP_MAX_SPEED 420.0f
#define SHIP_RADIUS 14.0f
#define SHIP_INVULN_TIME 2.0f

#define BULLET_SPEED 520.0f
#define BULLET_LIFETIME 1.1f
#define BULLET_RADIUS 3.0f
#define FIRE_COOLDOWN 0.22f

#define ROCK_MIN_SPEED 40.0f
#define ROCK_MAX_SPEED 130.0f
#define STARTING_ROCKS 5
#define STARTING_LIVES 3

#define EXPLOSION_FRAMES 4
#define EXPLOSION_FRAME_SIZE 32
#define EXPLOSION_FPS 14.0f

enum {
    ACT_TURN = 0, /* axis: -1 left, +1 right */
    ACT_THRUST,
    ACT_FIRE,
    ACT_RESTART,
};

typedef struct Velocity {
    float x, y;
} Velocity;

typedef struct Collider {
    float radius;
} Collider;

typedef struct Lifetime {
    float remaining;
} Lifetime;

typedef struct Ship {
    float fire_cooldown;
    float invulnerable;
} Ship;

typedef struct Rock {
    int size; /* 3 large, 2 medium, 1 small */
} Rock;

typedef struct Bullet {
    char unused;
} Bullet;

/* Short-lived entity that plays an explosion animation then deletes itself. */
typedef struct Explosion {
    char unused;
} Explosion;

typedef struct GameState {
    int score;
    int lives;
    int rocks_alive;
    bool game_over;
    int wave;

    mye_texture tex_ship;
    mye_texture tex_bullet;
    mye_texture tex_rock[3];
    mye_texture tex_explosion; /* 4x1 flipbook atlas, built at runtime */

    mye_sound sfx_fire;
    mye_sound sfx_boom;
    mye_sound sfx_hurt;

    /* Prefabs: templates the spawned entities inherit from (EcsIsA). Every
     * rock of a given size shares one MyeSprite, Collider and Rock rather
     * than storing its own copy, and retuning a prefab retunes every live
     * instance. Excluded from queries, so they never act as game objects. */
    ecs_entity_t prefab_ship;
    ecs_entity_t prefab_bullet;
    ecs_entity_t prefab_explosion;
    ecs_entity_t prefab_rock[3]; /* indexed by size - 1 */

    /* Built once: creating a query allocates, and the systems using it run
     * every fixed step. */
    ecs_query_t *rocks;
} GameState;

extern ECS_COMPONENT_DECLARE(Velocity);
extern ECS_COMPONENT_DECLARE(Collider);
extern ECS_COMPONENT_DECLARE(Lifetime);
extern ECS_COMPONENT_DECLARE(Ship);
extern ECS_COMPONENT_DECLARE(Rock);
extern ECS_COMPONENT_DECLARE(Bullet);
extern ECS_COMPONENT_DECLARE(Explosion);
extern ECS_COMPONENT_DECLARE(GameState);

/* Registers components, input bindings, textures and systems, then spawns
 * the ship and the first wave. */
void asteroids_setup(ecs_world_t *world);

/* Releases the query the game owns. Must run before mye_shutdown. */
void asteroids_teardown(ecs_world_t *world);

#endif /* ASTEROIDS_H */
