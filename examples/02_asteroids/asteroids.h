/* Asteroids: game logic, separated from main() so it can be driven headlessly
 * by tests/integration/test_int_asteroids.c. See plan/09-testing.md.
 *
 * Nothing here knows about scenes. The menu<->play flow lives next door in
 * scenes.c, which calls asteroids_register once and asteroids_start every
 * time a game begins. The one concession is GameState.playing, below. */
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
    ACT_CONFIRM, /* ENTER: start a game, or skip the game-over wait */
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

    /* True between asteroids_start and asteroids_stop -- i.e. while the play
     * scene is up. The systems that act on the world rather than on gameplay
     * entities (wave refills, the HUD) consult it, so the menu does not
     * quietly grow a wave of rocks behind the title. The game logic knows
     * nothing about scenes; this flag is the whole of the coupling. */
    bool playing;

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

/* Registers components, input bindings, generated art and audio, prefabs and
 * systems. Spawns nothing: all of that is global, survives scene switches,
 * and must be done exactly once. */
void asteroids_register(ecs_world_t *world);

/* Starts a fresh game: score, lives and wave back to the beginning, ship and
 * first wave on the field. This is the play scene's load callback, and it is
 * the whole of what "restart" used to mean -- minus the entity deletion,
 * which the scene unload now does. */
void asteroids_start(ecs_world_t *world);

/* Marks the game stopped. Deliberately deletes nothing: the ship, rocks and
 * bullets belong to the play scene and go when it unloads. */
void asteroids_stop(ecs_world_t *world);

/* A few slow rocks with no gameplay meaning, for the menu to drift behind its
 * title. Scene-owned like everything else, so they vanish on the way in. */
void asteroids_spawn_backdrop(ecs_world_t *world, int count);

/* asteroids_register + asteroids_start: a whole game in one call, with no
 * scene involved. The headless gameplay tests use this. */
void asteroids_setup(ecs_world_t *world);

/* Releases the query the game owns. Must run before mye_shutdown. */
void asteroids_teardown(ecs_world_t *world);

#endif /* ASTEROIDS_H */
