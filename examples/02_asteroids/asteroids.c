/* Asteroids game logic. See asteroids.h. */
#include "asteroids.h"

#include "core/rl_alloc.h"

#include <raylib.h>
#include <raymath.h>

#include <stdio.h>

ECS_COMPONENT_DECLARE(Velocity);
ECS_COMPONENT_DECLARE(Collider);
ECS_COMPONENT_DECLARE(Lifetime);
ECS_COMPONENT_DECLARE(Ship);
ECS_COMPONENT_DECLARE(Rock);
ECS_COMPONENT_DECLARE(Bullet);
ECS_COMPONENT_DECLARE(Explosion);
ECS_COMPONENT_DECLARE(GameState);

/* ------------------------------------------------------- generated art -- */

/* A triangular ship pointing right (angle 0), drawn into an image so the
 * example carries no binary assets. */
static Image make_ship_image(void)
{
    const int size = 32;
    Image image = GenImageColor(size, size, BLANK);
    ImageDrawTriangle(&image, (Vector2){ 30, 16 }, (Vector2){ 4, 4 },
                      (Vector2){ 10, 16 }, SKYBLUE);
    ImageDrawTriangle(&image, (Vector2){ 30, 16 }, (Vector2){ 10, 16 },
                      (Vector2){ 4, 28 }, SKYBLUE);
    return image;
}

static Image make_rock_image(int radius, Color color)
{
    int size = radius * 2;
    Image image = GenImageColor(size, size, BLANK);
    ImageDrawCircle(&image, radius, radius, radius - 1, color);
    ImageDrawCircleLines(&image, radius, radius, radius - 1, RAYWHITE);
    return image;
}

/* A 4-frame explosion laid out left to right: an expanding, fading ring.
 * Built in code so the example still ships no art files. */
static Image make_explosion_atlas(void)
{
    const int size = EXPLOSION_FRAME_SIZE;
    Image atlas = GenImageColor(size * EXPLOSION_FRAMES, size, BLANK);

    for (int f = 0; f < EXPLOSION_FRAMES; ++f) {
        float t = (float)f / (float)(EXPLOSION_FRAMES - 1); /* 0 .. 1 */
        int cx = f * size + size / 2;
        int cy = size / 2;
        int outer = (int)(4.0f + t * ((float)size * 0.45f));
        int inner = (int)((float)outer * (0.25f + 0.55f * t));

        unsigned char alpha = (unsigned char)(255.0f * (1.0f - t * 0.75f));
        Color hot = { 255, (unsigned char)(220 - (int)(t * 150.0f)), 60, alpha };

        ImageDrawCircle(&atlas, cx, cy, outer, hot);
        if (inner > 0) {
            /* Punch the middle out so later frames read as a ring. */
            ImageDrawCircle(&atlas, cx, cy, inner, BLANK);
        }
    }
    return atlas;
}

/* --------------------------------------------------------- synth audio -- */

/* Sounds are generated rather than loaded, for the same reason as the art.
 * 16-bit mono PCM at 22050 Hz is plenty for arcade blips. */
#define SFX_RATE 22050

static Wave make_wave(float seconds, float start_hz, float end_hz,
                      float noise_mix, float decay)
{
    unsigned int frames = (unsigned int)(seconds * (float)SFX_RATE);
    /* Allocated through the raylib shim, because UnloadWave will free it with
     * RL_FREE. Using plain malloc here would hand raylib's free a pointer our
     * header-prefixed allocator never produced. */
    short *samples = (short *)mye_rl_malloc(frames * sizeof(short));
    if (samples == NULL) {
        return (Wave){ 0 };
    }

    unsigned int seed = 12345;
    for (unsigned int i = 0; i < frames; ++i) {
        float t = (float)i / (float)frames;

        /* Frequency sweep gives a "pew" or a "thud" depending on direction. */
        float hz = start_hz + (end_hz - start_hz) * t;
        float phase = 2.0f * PI * hz * ((float)i / (float)SFX_RATE);
        float tone = sinf(phase);

        /* Cheap deterministic noise, for the explosive part. */
        seed = seed * 1103515245u + 12345u;
        float noise = (float)((seed >> 16) & 0x7FFF) / 16383.5f - 1.0f;

        float mixed = tone * (1.0f - noise_mix) + noise * noise_mix;
        float envelope = expf(-decay * t);

        samples[i] = (short)(mixed * envelope * 12000.0f);
    }

    return (Wave){
        .frameCount = frames,
        .sampleRate = SFX_RATE,
        .sampleSize = 16,
        .channels = 1,
        .data = samples,
    };
}

static Image make_bullet_image(void)
{
    Image image = GenImageColor(8, 8, BLANK);
    ImageDrawCircle(&image, 4, 4, 3, GOLD);
    return image;
}

/* -------------------------------------------------------------- prefabs -- */

/* Everything shared by all instances of a kind lives here, set once.
 *
 * The distinction that matters is ecs_set vs ecs_set_override:
 *
 *   ecs_set          -- instances SHARE this component with the prefab, but
 *                       ONLY if the component type is marked
 *                       (OnInstantiate, Inherit) -- see build_prefabs.
 *                       One copy in memory. Correct only when nothing writes
 *                       it per instance, because a write would hit every
 *                       instance at once.
 *   ecs_set_override -- each instance is given its own copy on creation, with
 *                       the prefab's value as the default. Needed for
 *                       anything a system mutates.
 *
 * Getting this backwards is subtle: sharing a component that a system writes
 * looks fine until two instances visibly move in lockstep. */
static void build_prefabs(ecs_world_t *world, GameState *state)
{
    float half_ship = 16.0f;


    state->prefab_ship = ecs_entity(world, { .name = "ShipPrefab",
                                             .add = ecs_ids(EcsPrefab) });
    ecs_set(world, state->prefab_ship, Collider, { SHIP_RADIUS }); /* shared */
    /* Per-instance: the ship counts down its own cooldown, and the blink
     * system writes its tint every frame. */
    ecs_set_override(world, state->prefab_ship, Ship,
                     { .fire_cooldown = 0.0f,
                       .invulnerable = SHIP_INVULN_TIME });
    /* MyeSprite is copied to each instance (engine default), which is what
     * the blink system needs anyway. */
    ecs_set(world, state->prefab_ship, MyeSprite,
            { .texture = state->tex_ship,
              .origin = { half_ship, half_ship },
              .tint = WHITE,
              .layer = 10 });

    state->prefab_bullet = ecs_entity(world, { .name = "BulletPrefab",
                                               .add = ecs_ids(EcsPrefab) });
    /* Shared: nothing writes a bullet's collider or sprite. Hundreds of
     * bullets reference one MyeSprite. */
    ecs_set(world, state->prefab_bullet, Collider, { BULLET_RADIUS });
    ecs_set(world, state->prefab_bullet, Bullet, { 0 });
    ecs_set(world, state->prefab_bullet, MyeSprite,
            { .texture = state->tex_bullet,
              .origin = { 4.0f, 4.0f },
              .tint = WHITE,
              .layer = 8 });
    /* Per-instance: each bullet counts down its own lifetime. */
    ecs_set_override(world, state->prefab_bullet, Lifetime,
                     { BULLET_LIFETIME });

    float explosion_half = (float)EXPLOSION_FRAME_SIZE * 0.5f;
    state->prefab_explosion = ecs_entity(world, { .name = "ExplosionPrefab",
                                                  .add = ecs_ids(EcsPrefab) });
    ecs_set(world, state->prefab_explosion, Explosion, { 0 }); /* shared tag */
    /* Both are written every frame by MyeSpriteAnimUpdate -- the playhead
     * advances and the source rect is rewritten -- so each instance needs its
     * own copy, which is the default for both types. */
    ecs_set(world, state->prefab_explosion, MyeSprite,
            { .texture = state->tex_explosion,
              .source = { 0.0f, 0.0f, (float)EXPLOSION_FRAME_SIZE,
                          (float)EXPLOSION_FRAME_SIZE },
              .origin = { explosion_half, explosion_half },
              .tint = WHITE,
              .layer = 20 });
    ecs_set(world, state->prefab_explosion, MyeSpriteAnim,
            { .first_frame = { 0.0f, 0.0f, (float)EXPLOSION_FRAME_SIZE,
                               (float)EXPLOSION_FRAME_SIZE },
              .columns = EXPLOSION_FRAMES,
              .frame_count = EXPLOSION_FRAMES,
              .fps = EXPLOSION_FPS,
              .loop = false,
              .playing = true });

    static const char *rock_names[3] = { "SmallRockPrefab", "MediumRockPrefab",
                                         "LargeRockPrefab" };
    for (int i = 0; i < 3; ++i) {
        int size = i + 1;
        float radius = (float)size * 12.0f;

        state->prefab_rock[i] = ecs_entity(world, { .name = rock_names[i],
                                                    .add = ecs_ids(EcsPrefab) });
        /* All shared: a rock's collider, size and sprite are read-only for
         * its whole life. This is the case prefabs are made for. */
        ecs_set(world, state->prefab_rock[i], Collider, { radius });
        ecs_set(world, state->prefab_rock[i], Rock, { size });
        ecs_set(world, state->prefab_rock[i], MyeSprite,
                { .texture = state->tex_rock[i],
                  .origin = { radius, radius },
                  .tint = WHITE,
                  .layer = 5 });
    }
}

/* ------------------------------------------------------------- spawning -- */

/* Returns true if the entity was teleported across the screen. Interpolated
 * entities must be snapped when that happens, or the renderer blends from
 * one edge to the other and draws a streak through everything between. */
static bool wrap_position(MyePosition2D *p)
{
    bool wrapped = false;
    if (p->x < 0.0f) { p->x += (float)SCREEN_W; wrapped = true; }
    if (p->x > (float)SCREEN_W) { p->x -= (float)SCREEN_W; wrapped = true; }
    if (p->y < 0.0f) { p->y += (float)SCREEN_H; wrapped = true; }
    if (p->y > (float)SCREEN_H) { p->y -= (float)SCREEN_H; wrapped = true; }
    return wrapped;
}

/* Every gameplay entity is born here, and it matters that it is
 * mye_entity_new rather than the shorter ecs_new_w_pair: only mye_entity_new
 * tags the entity for the active scene, and untagged entities survive a scene
 * switch. That would leave the old ship and half a wave of rocks drifting
 * through the menu. Adding the EcsIsA pair afterwards costs nothing --
 * MyeSceneOf is (OnInstantiate, DontInherit), so instantiating the prefab
 * cannot disturb the ownership tag. */
static ecs_entity_t spawn_from_prefab(ecs_world_t *world, ecs_entity_t prefab)
{
    ecs_entity_t e = mye_entity_new(world);
    ecs_add_pair(world, e, EcsIsA, prefab);
    return e;
}

static ecs_entity_t spawn_ship(ecs_world_t *world, const GameState *state)
{
    /* Collider, Ship and MyeSprite all arrive from the prefab; Ship and
     * MyeSprite as private copies thanks to ecs_set_override. */
    ecs_entity_t e = spawn_from_prefab(world, state->prefab_ship);
    ecs_set(world, e, MyePosition2D, { SCREEN_W * 0.5f, SCREEN_H * 0.5f });
    ecs_set(world, e, MyeRotation2D, { -PI * 0.5f }); /* nose up */
    ecs_set(world, e, Velocity, { 0.0f, 0.0f });
    /* Opt in to smoothing: the ship is the thing a player watches most. */
    ecs_set(world, e, MyeInterpolate, { 0 });
    return e;
}

static void spawn_rock(ecs_world_t *world, GameState *state, int size, float x,
                       float y)
{
    float angle = (float)GetRandomValue(0, 359) * DEG2RAD;
    float speed = (float)GetRandomValue((int)ROCK_MIN_SPEED,
                                        (int)ROCK_MAX_SPEED);
    /* Smaller fragments move faster -- keeps late waves lively. */
    speed *= (4.0f - (float)size) * 0.5f;

    /* Collider, Rock and MyeSprite all come from the prefab. */
    ecs_entity_t e = spawn_from_prefab(world, state->prefab_rock[size - 1]);
    ecs_set(world, e, MyePosition2D, { x, y });
    ecs_set(world, e, MyeRotation2D, { 0.0f });
    ecs_set(world, e, Velocity, { cosf(angle) * speed, sinf(angle) * speed });
    ecs_set(world, e, MyeInterpolate, { 0 });
    ++state->rocks_alive;
}

static void spawn_explosion(ecs_world_t *world, const GameState *state,
                            float x, float y, float scale)
{
    if (!mye_texture_valid(world, state->tex_explosion)) {
        return;
    }

    /* The sprite and the animation playhead arrive as private copies, so
     * each explosion runs its own flipbook. */
    ecs_entity_t e = spawn_from_prefab(world, state->prefab_explosion);
    ecs_set(world, e, MyePosition2D, { x, y });
    ecs_set(world, e, MyeScale2D, { scale, scale });
}

/* Deletes explosions once their animation has played out. */
static void DespawnFinishedExplosions(ecs_iter_t *it)
{
    const MyeSpriteAnim *anims = ecs_field(it, MyeSpriteAnim, 0);

    for (int i = 0; i < it->count; ++i) {
        if (anims[i].finished) {
            ecs_delete(it->world, it->entities[i]);
        }
    }
}

static void spawn_wave(ecs_world_t *world, GameState *state, int count)
{
    for (int i = 0; i < count; ++i) {
        /* Never spawn on top of the ship: keep to the screen edges. */
        float x = (float)GetRandomValue(0, SCREEN_W);
        float y = (float)GetRandomValue(0, 120);
        if (GetRandomValue(0, 1)) {
            x = (float)GetRandomValue(0, 120);
            y = (float)GetRandomValue(0, SCREEN_H);
        }
        spawn_rock(world, state, 3, x, y);
    }
}

/* ---------------------------------------------------- simulation systems -- */

/* Everything below runs in MyeOnFixedUpdate: a constant 1/60 s step, so the
 * game plays identically at 60 fps and 240 fps. */

static void ShipControl(ecs_iter_t *it)
{
    MyeRotation2D *rot = ecs_field(it, MyeRotation2D, 0);
    Velocity *vel = ecs_field(it, Velocity, 1);
    Ship *ship = ecs_field(it, Ship, 2);
    const MyePosition2D *pos = ecs_field(it, MyePosition2D, 3);

    ecs_world_t *world = it->world;
    float dt = (float)it->delta_time;
    GameState *state = ecs_singleton_ensure(world, GameState);
    if (state == NULL || state->game_over) {
        return;
    }

    float turn = mye_action_value(world, ACT_TURN);
    bool thrusting = mye_action_down(world, ACT_THRUST);
    bool firing = mye_action_down(world, ACT_FIRE);

    for (int i = 0; i < it->count; ++i) {
        rot[i].angle += turn * SHIP_TURN_SPEED * dt;

        if (thrusting) {
            vel[i].x += cosf(rot[i].angle) * SHIP_THRUST * dt;
            vel[i].y += sinf(rot[i].angle) * SHIP_THRUST * dt;
        }

        /* Drag, then clamp: keeps the ship controllable. */
        vel[i].x -= vel[i].x * SHIP_DRAG * dt;
        vel[i].y -= vel[i].y * SHIP_DRAG * dt;
        float speed = sqrtf(vel[i].x * vel[i].x + vel[i].y * vel[i].y);
        if (speed > SHIP_MAX_SPEED) {
            vel[i].x = vel[i].x / speed * SHIP_MAX_SPEED;
            vel[i].y = vel[i].y / speed * SHIP_MAX_SPEED;
        }

        if (ship[i].invulnerable > 0.0f) {
            ship[i].invulnerable -= dt;
        }
        if (ship[i].fire_cooldown > 0.0f) {
            ship[i].fire_cooldown -= dt;
        }

        if (firing && ship[i].fire_cooldown <= 0.0f) {
            ship[i].fire_cooldown = FIRE_COOLDOWN;

            float nose_x = pos[i].x + cosf(rot[i].angle) * SHIP_RADIUS;
            float nose_y = pos[i].y + sinf(rot[i].angle) * SHIP_RADIUS;

            /* Collider, Bullet and MyeSprite are inherited and shared;
             * Lifetime arrives as a private copy. Only the trajectory
             * differs per shot. */
            ecs_entity_t bullet =
                spawn_from_prefab(world, state->prefab_bullet);
            ecs_set(world, bullet, MyePosition2D, { nose_x, nose_y });
            ecs_set(world, bullet, Velocity,
                    { cosf(rot[i].angle) * BULLET_SPEED + vel[i].x,
                      sinf(rot[i].angle) * BULLET_SPEED + vel[i].y });

            /* Queued, not played here: several fixed steps can run in one
             * frame and the queue collapses the duplicates. */
            mye_sound_play_ex(world, state->sfx_fire, 0.35f, 1.0f);
        }
    }
}

static void MoveAndWrap(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const Velocity *vel = ecs_field(it, Velocity, 1);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        pos[i].x += vel[i].x * dt;
        pos[i].y += vel[i].y * dt;
        if (wrap_position(&pos[i])) {
            mye_transform_snap(it->world, it->entities[i]);
        }
    }
}

static void SpinRocks(ecs_iter_t *it)
{
    MyeRotation2D *rot = ecs_field(it, MyeRotation2D, 0);
    const Rock *rock = ecs_field(it, Rock, 1);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        rot[i].angle += (0.4f + 0.2f * (float)rock[i].size) * dt;
    }
}

static void ExpireLifetimes(ecs_iter_t *it)
{
    Lifetime *life = ecs_field(it, Lifetime, 0);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        life[i].remaining -= dt;
        if (life[i].remaining <= 0.0f) {
            /* Deferred by flecs until the end of the step. */
            ecs_delete(it->world, it->entities[i]);
        }
    }
}

/* Bullets vs rocks. Uses raylib's collision helper rather than hand-rolled
 * math -- it is pure CPU code and works headless. */
static void BulletsHitRocks(ecs_iter_t *it)
{
    const MyePosition2D *bullet_pos = ecs_field(it, MyePosition2D, 0);
    const Collider *bullet_col = ecs_field(it, Collider, 1);

    ecs_world_t *world = it->world;
    GameState *state = ecs_singleton_ensure(world, GameState);
    if (state == NULL) {
        return;
    }

    ecs_query_t *rocks = state->rocks;
    if (rocks == NULL) {
        return;
    }

    for (int b = 0; b < it->count; ++b) {
        ecs_entity_t bullet = it->entities[b];
        bool bullet_spent = false;

        ecs_iter_t rock_it = ecs_query_iter(world, rocks);
        while (!bullet_spent && ecs_query_next(&rock_it)) {
            const MyePosition2D *rp = ecs_field(&rock_it, MyePosition2D, 0);
            const Collider *rc = ecs_field(&rock_it, Collider, 1);
            const Rock *rock = ecs_field(&rock_it, Rock, 2);

            for (int r = 0; r < rock_it.count; ++r) {
                if (!CheckCollisionCircles(
                        (Vector2){ bullet_pos[b].x, bullet_pos[b].y },
                        bullet_col[b].radius,
                        (Vector2){ rp[r].x, rp[r].y }, rc[r].radius)) {
                    continue;
                }

                ecs_entity_t rock_entity = rock_it.entities[r];
                int size = rock[r].size;
                float x = rp[r].x;
                float y = rp[r].y;

                state->score += (4 - size) * 20;
                spawn_explosion(world, state, x, y, (float)size * 0.5f);
                mye_sound_play_ex(world, state->sfx_boom,
                                  0.35f + 0.15f * (float)size,
                                  1.4f - 0.2f * (float)size);
                ecs_delete(world, rock_entity);
                --state->rocks_alive;

                /* Large and medium rocks split into two smaller ones. */
                if (size > 1) {
                    spawn_rock(world, state, size - 1, x, y);
                    spawn_rock(world, state, size - 1, x, y);
                }

                ecs_delete(world, bullet);
                bullet_spent = true;
                break;
            }
        }
        /* Only finalise when we broke out early: a loop that ran to
         * completion has already been finalised by flecs, and finalising
         * twice aborts. */
        if (bullet_spent) {
            ecs_iter_fini(&rock_it);
        }
    }

}

static void RocksHitShip(ecs_iter_t *it)
{
    MyePosition2D *ship_pos = ecs_field(it, MyePosition2D, 0);
    const Collider *ship_col = ecs_field(it, Collider, 1);
    Ship *ship = ecs_field(it, Ship, 2);
    Velocity *ship_vel = ecs_field(it, Velocity, 3);

    ecs_world_t *world = it->world;
    GameState *state = ecs_singleton_ensure(world, GameState);
    if (state == NULL || state->game_over) {
        return;
    }

    ecs_query_t *rocks = state->rocks;
    if (rocks == NULL) {
        return;
    }

    for (int s = 0; s < it->count; ++s) {
        if (ship[s].invulnerable > 0.0f) {
            continue;
        }

        bool hit = false;
        ecs_iter_t rock_it = ecs_query_iter(world, rocks);
        while (!hit && ecs_query_next(&rock_it)) {
            const MyePosition2D *rp = ecs_field(&rock_it, MyePosition2D, 0);
            const Collider *rc = ecs_field(&rock_it, Collider, 1);

            for (int r = 0; r < rock_it.count; ++r) {
                if (CheckCollisionCircles(
                        (Vector2){ ship_pos[s].x, ship_pos[s].y },
                        ship_col[s].radius,
                        (Vector2){ rp[r].x, rp[r].y }, rc[r].radius)) {
                    hit = true;
                    break;
                }
            }
        }
        if (hit) {
            ecs_iter_fini(&rock_it); /* broke out early; see note above */
        } else {
            continue;
        }

        spawn_explosion(world, state, ship_pos[s].x, ship_pos[s].y, 1.5f);
        mye_sound_play_ex(world, state->sfx_hurt, 0.7f, 1.0f);

        if (--state->lives <= 0) {
            state->game_over = true;
        }

        /* Respawn in the middle, briefly invulnerable. */
        ship_pos[s] = (MyePosition2D){ SCREEN_W * 0.5f, SCREEN_H * 0.5f };
        ship_vel[s] = (Velocity){ 0.0f, 0.0f };
        ship[s].invulnerable = SHIP_INVULN_TIME;
        /* Respawning in the middle is a teleport like any other. */
        mye_transform_snap(world, it->entities[s]);
    }

}

static void NextWaveWhenClear(ecs_iter_t *it)
{
    (void)it;
    ecs_world_t *world = it->world;
    GameState *state = ecs_singleton_ensure(world, GameState);
    /* The `playing` check is what keeps the menu's backdrop rocks from being
     * mistaken for a cleared wave and topped up. */
    if (state == NULL || !state->playing || state->game_over ||
        state->rocks_alive > 0) {
        return;
    }

    ++state->wave;
    spawn_wave(world, state, STARTING_ROCKS + state->wave);
}

/* -------------------------------------------------------------------- UI -- */

static void DrawHud(ecs_iter_t *it)
{
    const GameState *state = ecs_field(it, GameState, 0);
    ecs_world_t *world = it->world;

    /* The score belongs to the game; the menu draws its own screen. Rather
     * than ask which scene is up -- this file does not know scenes exist --
     * the HUD simply draws while a game is running. */
    if (!state->playing) {
        return;
    }

    /* Per-frame scratch: freed automatically at the top of the next frame. */
    mye_allocator frame = mye_frame_allocator(world);
    char *line = MYE_NEW_ARRAY(frame, char, 128);
    if (line != NULL) {
        snprintf(line, 128, "SCORE %d    LIVES %d    WAVE %d", state->score,
                 state->lives, state->wave);
        DrawText(line, 20, 18, 24, RAYWHITE);
    }

    if (state->game_over) {
        const char *over = "GAME OVER";
        const char *hint = "returning to the menu...";
        DrawText(over, SCREEN_W / 2 - MeasureText(over, 56) / 2,
                 SCREEN_H / 2 - 60, 56, RED);
        DrawText(hint, SCREEN_W / 2 - MeasureText(hint, 22) / 2,
                 SCREEN_H / 2 + 10, 22, RAYWHITE);
    }

    DrawFPS(SCREEN_W - 90, 18);
}

/* Blink the ship while it is invulnerable, so the player can tell. */
static void BlinkInvulnerableShip(ecs_iter_t *it)
{
    const Ship *ship = ecs_field(it, Ship, 0);
    MyeSprite *sprite = ecs_field(it, MyeSprite, 1);

    for (int i = 0; i < it->count; ++i) {
        bool blink_off = ship[i].invulnerable > 0.0f &&
                         fmodf(ship[i].invulnerable, 0.25f) > 0.125f;
        sprite[i].tint = blink_off ? (Color){ 255, 255, 255, 60 } : WHITE;
    }
}

/* ------------------------------------------------------------------ setup -- */

static void state_rocks_query_init(ecs_world_t *world)
{
    GameState *state = ecs_singleton_ensure(world, GameState);
    state->rocks = ecs_query(world, {
        .terms = {
            { .id = ecs_id(MyePosition2D), .inout = EcsIn },
            { .id = ecs_id(Collider), .inout = EcsIn },
            { .id = ecs_id(Rock), .inout = EcsIn },
        },
    });
}

void asteroids_register(ecs_world_t *world)
{
    ECS_COMPONENT_DEFINE(world, Velocity);
    ECS_COMPONENT_DEFINE(world, Collider);
    ECS_COMPONENT_DEFINE(world, Lifetime);
    ECS_COMPONENT_DEFINE(world, Ship);
    ECS_COMPONENT_DEFINE(world, Rock);
    ECS_COMPONENT_DEFINE(world, Bullet);
    ECS_COMPONENT_DEFINE(world, Explosion);
    ECS_COMPONENT_DEFINE(world, GameState);
    ecs_add_id(world, ecs_id(GameState), EcsSingleton);

    /* A note on what prefabs do and do not buy here.
     *
     * flecs 4 COPIES components to instances by default ((OnInstantiate,
     * Override)). True sharing is opt-in per component type via
     * (OnInstantiate, Inherit), and it was tried here and deliberately backed
     * out, because a shared field is ONE value rather than a per-entity
     * array: every system reading it must then branch on
     * ecs_field_is_self() and index [0] instead of [row]. Forgetting that is
     * an out-of-bounds read -- which is exactly how it was found.
     *
     * That burden lands on every reader, forever, to save a few hundred bytes
     * at this entity count. Not worth it. If a future game spawns tens of
     * thousands of identical entities, revisit -- and fix every system that
     * reads the shared component at the same time. See plan/07-roadmap.md.
     *
     * So prefabs here are templates whose values are copied: one definition
     * per kind of thing, smaller spawn functions, and a name for scene files
     * to reference in M6. */

    mye_input_bind_axis_keys(world, ACT_TURN, KEY_LEFT, KEY_RIGHT);
    mye_input_bind_axis_keys(world, ACT_TURN, KEY_A, KEY_D);
    mye_input_bind_key(world, ACT_THRUST, KEY_UP);
    mye_input_bind_key(world, ACT_THRUST, KEY_W);
    mye_input_bind_key(world, ACT_FIRE, KEY_SPACE);
    mye_input_bind_key(world, ACT_CONFIRM, KEY_ENTER);

    ecs_singleton_set(world, GameState, { .lives = STARTING_LIVES });
    state_rocks_query_init(world);

    /* Textures are generated at runtime, so the game ships no art files. */
    GameState *state = ecs_singleton_ensure(world, GameState);
    state->tex_ship = mye_texture_from_image(world, "gen:ship",
                                             make_ship_image());
    state->tex_bullet = mye_texture_from_image(world, "gen:bullet",
                                               make_bullet_image());
    state->tex_rock[2] = mye_texture_from_image(world, "gen:rock3",
                                                make_rock_image(36, GRAY));
    state->tex_rock[1] = mye_texture_from_image(world, "gen:rock2",
                                                make_rock_image(24, GRAY));
    state->tex_rock[0] = mye_texture_from_image(world, "gen:rock1",
                                                make_rock_image(12, LIGHTGRAY));
    state->tex_explosion = mye_texture_from_image(world, "gen:explosion",
                                                  make_explosion_atlas());

    /* Sounds are synthesized too: a rising blip to fire, a noisy thud for a
     * rock, a lower one for taking a hit. */
    state->sfx_fire = mye_sound_from_wave(world, "gen:fire",
                                          make_wave(0.10f, 900.0f, 300.0f,
                                                    0.15f, 6.0f));
    state->sfx_boom = mye_sound_from_wave(world, "gen:boom",
                                          make_wave(0.35f, 220.0f, 60.0f,
                                                    0.75f, 4.0f));
    state->sfx_hurt = mye_sound_from_wave(world, "gen:hurt",
                                          make_wave(0.50f, 320.0f, 40.0f,
                                                    0.35f, 3.0f));

    build_prefabs(world, state);

    /* Simulation: fixed timestep, so gameplay is framerate independent. */
    ECS_SYSTEM(world, ShipControl, MyeOnFixedUpdate, MyeRotation2D, Velocity,
               Ship, [in] MyePosition2D);
    ECS_SYSTEM(world, MoveAndWrap, MyeOnFixedUpdate, MyePosition2D,
               [in] Velocity);
    ECS_SYSTEM(world, SpinRocks, MyeOnFixedUpdate, MyeRotation2D, [in] Rock);
    ECS_SYSTEM(world, ExpireLifetimes, MyeOnFixedUpdate, Lifetime);
    ECS_SYSTEM(world, BulletsHitRocks, MyeOnFixedUpdate, [in] MyePosition2D,
               [in] Collider, [in] Bullet);
    ECS_SYSTEM(world, RocksHitShip, MyeOnFixedUpdate, MyePosition2D,
               [in] Collider, Ship, Velocity);
    ECS_SYSTEM(world, NextWaveWhenClear, MyeOnFixedUpdate, GameState);
    ECS_SYSTEM(world, DespawnFinishedExplosions, EcsPostUpdate,
               [in] MyeSpriteAnim, [in] Explosion);

    /* Presentation: variable rate. Drawing systems are only registered when
     * there is a window; headless worlds run the simulation alone. */
    const mye_engine *engine = mye_engine_get(world);
    if (engine == NULL || !engine->headless) {
        ECS_SYSTEM(world, BlinkInvulnerableShip, EcsPreStore, [in] Ship,
                   MyeSprite);
        ECS_SYSTEM(world, DrawHud, MyeOnDrawUI, [in] GameState);
    }
}

void asteroids_start(ecs_world_t *world)
{
    GameState *state = ecs_singleton_ensure(world, GameState);
    if (state == NULL) {
        return;
    }

    state->score = 0;
    state->lives = STARTING_LIVES;
    state->rocks_alive = 0;
    state->wave = 0;
    state->game_over = false;
    state->playing = true;

    spawn_ship(world, state);
    spawn_wave(world, state, STARTING_ROCKS);
}

void asteroids_stop(ecs_world_t *world)
{
    GameState *state = ecs_singleton_ensure(world, GameState);
    if (state == NULL) {
        return;
    }
    state->playing = false;
    /* No ecs_delete_with here, deliberately. The ship, the rocks, the bullets
     * in flight and the explosions still burning belong to the play scene,
     * and unloading it deletes exactly them. That includes the ones gameplay
     * spawned mid-run, which are precisely the ones a hand-written cleanup
     * list forgets -- this function used to be four ecs_delete_with calls,
     * and it was one kind of entity away from being wrong. */
}

void asteroids_spawn_backdrop(ecs_world_t *world, int count)
{
    GameState *state = ecs_singleton_ensure(world, GameState);
    if (state == NULL) {
        return;
    }
    /* These count towards rocks_alive like any other rock. Harmless: nothing
     * reads it while `playing` is false, and asteroids_start zeroes it. */
    for (int i = 0; i < count; ++i) {
        spawn_rock(world, state, GetRandomValue(1, 3),
                   (float)GetRandomValue(0, SCREEN_W),
                   (float)GetRandomValue(0, SCREEN_H));
    }
}

void asteroids_setup(ecs_world_t *world)
{
    asteroids_register(world);
    asteroids_start(world);
}

void asteroids_teardown(ecs_world_t *world)
{
    /* Queries must be released before the world they belong to. */
    GameState *state = ecs_singleton_ensure(world, GameState);
    if (state != NULL && state->rocks != NULL) {
        ecs_query_fini(state->rocks);
        state->rocks = NULL;
    }
}
