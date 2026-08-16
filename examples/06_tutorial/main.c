/* Orbit Collector -- the tutorial's capstone.
 *
 * Every engine feature TUTORIAL.md introduces, in one working game:
 * scenes, input actions, fixed timestep, prefabs, sprite animation, a
 * transform hierarchy, interpolation, audio, the frame allocator, logging.
 *
 * Play: WASD/arrows move. Collect orbs. Avoid mines. ENTER starts and
 * restarts, F3 shows the debug overlay.
 */
#include "asset/asset.h"
#include "audio/audio.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/rl_alloc.h"
#include "input/input.h"
#include "render/render2d.h"
#include "scene/scene.h"
#include "scene/transform.h"

#include <raylib.h>
#include <raymath.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 1000
#define SCREEN_H 640

#define PLAYER_ACCEL 900.0f
#define PLAYER_DRAG 3.0f
#define PLAYER_MAX_SPEED 340.0f
#define PLAYER_RADIUS 14.0f

#define ORB_COUNT 8
#define MINE_COUNT 5
#define ORB_RADIUS 10.0f
#define MINE_RADIUS 12.0f
#define SHIELD_DISTANCE 34.0f

enum { ACT_X, ACT_Y, ACT_CONFIRM };

/* --- components ---------------------------------------------------------- */

typedef struct Velocity { float x, y; } Velocity;
typedef struct Collider { float radius; } Collider;
typedef struct Player { float shield_angle; } Player;
typedef struct Orb { char unused; } Orb;
typedef struct Mine { float drift; } Mine;
typedef struct Shield { char unused; } Shield;

/* One singleton for everything the game needs globally. */
typedef struct Game {
    int score;
    int lives;
    float elapsed;
    bool over;

    mye_texture tex_ship, tex_orb, tex_mine, tex_shield;
    mye_sound sfx_pickup, sfx_hit;
    ecs_entity_t prefab_orb, prefab_mine;
} Game;

ECS_COMPONENT_DECLARE(Velocity);
ECS_COMPONENT_DECLARE(Collider);
ECS_COMPONENT_DECLARE(Player);
ECS_COMPONENT_DECLARE(Orb);
ECS_COMPONENT_DECLARE(Mine);
ECS_COMPONENT_DECLARE(Shield);
ECS_COMPONENT_DECLARE(Game);

/* --- generated art and audio --------------------------------------------- */

static Image disc_image(int radius, Color fill, Color rim)
{
    Image img = GenImageColor(radius * 2, radius * 2, BLANK);
    ImageDrawCircle(&img, radius, radius, radius - 1, fill);
    ImageDrawCircleLines(&img, radius, radius, radius - 1, rim);
    return img;
}

/* 4-frame pulse, laid out left to right: the flipbook the orbs play. */
static Image orb_atlas_image(void)
{
    const int size = 24;
    Image atlas = GenImageColor(size * 4, size, BLANK);
    for (int f = 0; f < 4; ++f) {
        float t = (float)f / 3.0f;
        int r = (int)(6.0f + t * 5.0f);
        unsigned char a = (unsigned char)(255 - (int)(t * 90.0f));
        ImageDrawCircle(&atlas, f * size + size / 2, size / 2, r,
                        (Color){ 120, 230, 190, a });
    }
    return atlas;
}

static Wave beep_wave(float seconds, float from_hz, float to_hz, float decay)
{
    unsigned int frames = (unsigned int)(seconds * 22050.0f);
    short *samples = (short *)mye_rl_malloc(frames * sizeof(short));
    if (samples == NULL) return (Wave){ 0 };

    for (unsigned int i = 0; i < frames; ++i) {
        float t = (float)i / (float)frames;
        float hz = from_hz + (to_hz - from_hz) * t;
        float envelope = expf(-decay * t);
        samples[i] = (short)(sinf(2.0f * PI * hz * ((float)i / 22050.0f)) *
                             envelope * 11000.0f);
    }
    return (Wave){ .frameCount = frames, .sampleRate = 22050,
                   .sampleSize = 16, .channels = 1, .data = samples };
}

/* --- simulation (fixed timestep) ----------------------------------------- */

static void PlayerControl(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    Velocity *vel = ecs_field(it, Velocity, 1);
    Player *player = ecs_field(it, Player, 2);
    ecs_world_t *world = it->world;
    float dt = (float)it->delta_time;

    float ax = mye_action_value(world, ACT_X);
    float ay = mye_action_value(world, ACT_Y);

    for (int i = 0; i < it->count; ++i) {
        vel[i].x += ax * PLAYER_ACCEL * dt;
        vel[i].y += ay * PLAYER_ACCEL * dt;

        vel[i].x -= vel[i].x * PLAYER_DRAG * dt;
        vel[i].y -= vel[i].y * PLAYER_DRAG * dt;

        float speed = sqrtf(vel[i].x * vel[i].x + vel[i].y * vel[i].y);
        if (speed > PLAYER_MAX_SPEED) {
            vel[i].x = vel[i].x / speed * PLAYER_MAX_SPEED;
            vel[i].y = vel[i].y / speed * PLAYER_MAX_SPEED;
        }

        pos[i].x += vel[i].x * dt;
        pos[i].y += vel[i].y * dt;

        /* Bounce off the edges rather than teleporting: no snap needed. */
        if (pos[i].x < PLAYER_RADIUS) { pos[i].x = PLAYER_RADIUS; vel[i].x = -vel[i].x * 0.5f; }
        if (pos[i].x > SCREEN_W - PLAYER_RADIUS) { pos[i].x = SCREEN_W - PLAYER_RADIUS; vel[i].x = -vel[i].x * 0.5f; }
        if (pos[i].y < PLAYER_RADIUS) { pos[i].y = PLAYER_RADIUS; vel[i].y = -vel[i].y * 0.5f; }
        if (pos[i].y > SCREEN_H - PLAYER_RADIUS) { pos[i].y = SCREEN_H - PLAYER_RADIUS; vel[i].y = -vel[i].y * 0.5f; }

        player[i].shield_angle += 2.4f * dt;
    }
}

/* The shield is parented to the player, so its position is a local offset and
 * the parent's Player component arrives as a term -- no lookup by name. */
static void ShieldOrbit(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const Player *parent = ecs_field(it, Player, 2); /* one value, the parent's */

    for (int i = 0; i < it->count; ++i) {
        pos[i].x = cosf(parent->shield_angle) * SHIELD_DISTANCE;
        pos[i].y = sinf(parent->shield_angle) * SHIELD_DISTANCE;
    }
}

static void MineDrift(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const Velocity *vel = ecs_field(it, Velocity, 1);
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        pos[i].x += vel[i].x * dt;
        pos[i].y += vel[i].y * dt;

        if (pos[i].x < 0 || pos[i].x > SCREEN_W) pos[i].x = pos[i].x < 0 ? SCREEN_W : 0;
        if (pos[i].y < 0 || pos[i].y > SCREEN_H) pos[i].y = pos[i].y < 0 ? SCREEN_H : 0;
        /* Wrapped: suppress the blend or it draws a streak. */
        mye_transform_snap(it->world, it->entities[i]);
    }
}

static bool overlaps(MyePosition2D a, float ra, MyePosition2D b, float rb)
{
    return CheckCollisionCircles((Vector2){ a.x, a.y }, ra,
                                 (Vector2){ b.x, b.y }, rb);
}

/* Built once in main: a query is a compiled plan, not something to rebuild
 * sixty times a second. */
static ecs_query_t *g_orbs;
static ecs_query_t *g_mines;

static void Collisions(ecs_iter_t *it)
{
    const MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    ecs_world_t *world = it->world;

    Game *game = ecs_singleton_ensure(world, Game);
    if (game == NULL || game->over) return;

    for (int p = 0; p < it->count; ++p) {
        ecs_iter_t orb_it = ecs_query_iter(world, g_orbs);
        while (ecs_query_next(&orb_it)) {
            const MyePosition2D *op = ecs_field(&orb_it, MyePosition2D, 0);
            const Collider *oc = ecs_field(&orb_it, Collider, 1);
            for (int o = 0; o < orb_it.count; ++o) {
                if (!overlaps(pos[p], PLAYER_RADIUS, op[o], oc[o].radius)) continue;
                game->score += 10;
                mye_sound_play_ex(world, game->sfx_pickup, 0.4f, 1.0f);
                ecs_delete(world, orb_it.entities[o]);
            }
        }

        ecs_iter_t mine_it = ecs_query_iter(world, g_mines);
        while (ecs_query_next(&mine_it)) {
            const MyePosition2D *mp = ecs_field(&mine_it, MyePosition2D, 0);
            const Collider *mc = ecs_field(&mine_it, Collider, 1);
            for (int m = 0; m < mine_it.count; ++m) {
                if (!overlaps(pos[p], PLAYER_RADIUS, mp[m], mc[m].radius)) continue;
                mye_sound_play_ex(world, game->sfx_hit, 0.6f, 1.0f);
                ecs_delete(world, mine_it.entities[m]);
                if (--game->lives <= 0) {
                    game->over = true;
                    mye_log_info("game over with %d points", game->score);
                }
            }
        }
    }
}

static void Clock(ecs_iter_t *it)
{
    Game *game = ecs_field(it, Game, 0);
    if (!game->over) game->elapsed += (float)it->delta_time;
}

/* --- presentation -------------------------------------------------------- */

/* Draw systems run every frame, so each checks whether its scene is the
 * active one. */
static bool scene_is(const ecs_world_t *world, const char *name)
{
    const char *current = mye_scene_current(world);
    return current != NULL && strcmp(current, name) == 0;
}

static void DrawHud(ecs_iter_t *it)
{
    const Game *game = ecs_field(it, Game, 0);
    ecs_world_t *world = it->world;
    if (!scene_is(world, "play")) return;

    /* Frame allocator: reclaimed next frame, so nothing to free. */
    char *line = MYE_NEW_ARRAY(mye_frame_allocator(world), char, 128);
    if (line != NULL) {
        snprintf(line, 128, "SCORE %d    LIVES %d    TIME %.1f", game->score,
                 game->lives, (double)game->elapsed);
        DrawText(line, 20, 18, 22, RAYWHITE);
    }

    if (game->over) {
        const char *over = "GAME OVER";
        const char *hint = "ENTER to play again";
        DrawText(over, SCREEN_W / 2 - MeasureText(over, 52) / 2, 250, 52, RED);
        DrawText(hint, SCREEN_W / 2 - MeasureText(hint, 20) / 2, 320, 20,
                 RAYWHITE);
    }
}

static void DrawMenu(ecs_iter_t *it)
{
    if (!scene_is(it->world, "menu")) return;
    const char *title = "ORBIT COLLECTOR";
    const char *hint = "ENTER to start    WASD to move    F3 for stats";
    DrawText(title, SCREEN_W / 2 - MeasureText(title, 48) / 2, 210, 48,
             (Color){ 120, 230, 190, 255 });
    DrawText(hint, SCREEN_W / 2 - MeasureText(hint, 20) / 2, 300, 20,
             (Color){ 170, 178, 195, 255 });
}

/* --- scenes -------------------------------------------------------------- */

static void SceneSwitcher(ecs_iter_t *it)
{
    ecs_world_t *world = it->world;
    if (!mye_action_pressed(world, ACT_CONFIRM)) return;

    const char *current = mye_scene_current(world);
    const Game *game = ecs_singleton_get(world, Game);

    if (current == NULL) return;
    if (strcmp(current, "menu") == 0) {
        mye_scene_switch(world, "play");
    } else if (game != NULL && game->over) {
        mye_scene_reload(world); /* fresh play scene */
    }
}

static void menu_load(ecs_world_t *world, void *user)
{
    (void)world;
    (void)user;
    mye_log_info("menu");
    /* Nothing to spawn: the menu is one draw system. Registered once in
     * main, not here, so it survives scene switches. */
}

static void play_load(ecs_world_t *world, void *user)
{
    (void)user;
    Game *game = ecs_singleton_ensure(world, Game);
    game->score = 0;
    game->lives = 3;
    game->elapsed = 0.0f;
    game->over = false;

    /* Player: interpolated, and parent of the shield. */
    ecs_entity_t player = mye_entity_new(world);
    ecs_set_name(world, player, "player");
    ecs_set(world, player, MyePosition2D, { SCREEN_W / 2.0f, SCREEN_H / 2.0f });
    ecs_set(world, player, Velocity, { 0.0f, 0.0f });
    ecs_set(world, player, Player, { 0.0f });
    ecs_set(world, player, Collider, { PLAYER_RADIUS });
    ecs_set(world, player, MyeInterpolate, { 0 });
    ecs_set(world, player, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, player, MyeWorldTransform, { MatrixIdentity() });
    ecs_set(world, player, MyeSprite,
            { .texture = game->tex_ship, .origin = { 16.0f, 16.0f },
              .tint = WHITE, .layer = 10 });

    /* Child: its position is relative to the player. */
    ecs_entity_t shield = mye_entity_new(world);
    ecs_set(world, shield, MyePosition2D, { SHIELD_DISTANCE, 0.0f });
    ecs_set(world, shield, Shield, { 0 });
    ecs_set(world, shield, MyeLocalTransform, { MatrixIdentity() });
    ecs_set(world, shield, MyeWorldTransform, { MatrixIdentity() });
    ecs_set(world, shield, MyeSprite,
            { .texture = game->tex_shield, .origin = { 8.0f, 8.0f },
              .tint = WHITE, .layer = 9 });
    mye_set_parent(world, shield, player);

    for (int i = 0; i < ORB_COUNT; ++i) {
        ecs_entity_t orb = ecs_new_w_pair(world, EcsIsA, game->prefab_orb);
        ecs_set(world, orb, MyePosition2D,
                { (float)GetRandomValue(60, SCREEN_W - 60),
                  (float)GetRandomValue(60, SCREEN_H - 60) });
        ecs_set(world, orb, MyeSpriteAnim,
                { .first_frame = { 0.0f, 0.0f, 24.0f, 24.0f },
                  .columns = 4, .frame_count = 4, .fps = 8.0f,
                  .loop = true, .playing = true });
    }

    for (int i = 0; i < MINE_COUNT; ++i) {
        ecs_entity_t mine = ecs_new_w_pair(world, EcsIsA, game->prefab_mine);
        ecs_set(world, mine, MyePosition2D,
                { (float)GetRandomValue(0, SCREEN_W),
                  (float)GetRandomValue(0, SCREEN_H) });
        ecs_set(world, mine, Velocity,
                { (float)GetRandomValue(-70, 70),
                  (float)GetRandomValue(-70, 70) });
        ecs_set(world, mine, MyeInterpolate, { 0 });
    }

    mye_log_info("play: %d orbs, %d mines", ORB_COUNT, MINE_COUNT);
}

/* --- setup --------------------------------------------------------------- */

static void build_prefabs(ecs_world_t *world, Game *game)
{
    game->prefab_orb = ecs_entity(world, { .name = "OrbPrefab",
                                           .add = ecs_ids(EcsPrefab) });
    ecs_set(world, game->prefab_orb, Orb, { 0 });
    ecs_set(world, game->prefab_orb, Collider, { ORB_RADIUS });
    ecs_set(world, game->prefab_orb, MyeSprite,
            { .texture = game->tex_orb,
              .source = { 0.0f, 0.0f, 24.0f, 24.0f },
              .origin = { 12.0f, 12.0f }, .tint = WHITE, .layer = 5 });

    game->prefab_mine = ecs_entity(world, { .name = "MinePrefab",
                                            .add = ecs_ids(EcsPrefab) });
    ecs_set(world, game->prefab_mine, Mine, { 0.0f });
    ecs_set(world, game->prefab_mine, Collider, { MINE_RADIUS });
    ecs_set(world, game->prefab_mine, MyeSprite,
            { .texture = game->tex_mine, .origin = { 12.0f, 12.0f },
              .tint = WHITE, .layer = 6 });
}

int main(void)
{
    ecs_world_t *world = mye_init(&(mye_config){
        .width = SCREEN_W, .height = SCREEN_H,
        .title = "myecs -- orbit collector",
        .frame_arena_bytes = 128 * 1024,
    });
    if (world == NULL) return 1;

    ECS_COMPONENT_DEFINE(world, Velocity);
    ECS_COMPONENT_DEFINE(world, Collider);
    ECS_COMPONENT_DEFINE(world, Player);
    ECS_COMPONENT_DEFINE(world, Orb);
    ECS_COMPONENT_DEFINE(world, Mine);
    ECS_COMPONENT_DEFINE(world, Shield);
    ECS_COMPONENT_DEFINE(world, Game);
    ecs_add_id(world, ecs_id(Game), EcsSingleton);

    mye_input_bind_axis_keys(world, ACT_X, KEY_A, KEY_D);
    mye_input_bind_axis_keys(world, ACT_X, KEY_LEFT, KEY_RIGHT);
    mye_input_bind_axis_keys(world, ACT_Y, KEY_W, KEY_S);
    mye_input_bind_axis_keys(world, ACT_Y, KEY_UP, KEY_DOWN);
    mye_input_bind_key(world, ACT_CONFIRM, KEY_ENTER);

    ecs_singleton_set(world, Game, { .lives = 3 });
    Game *game = ecs_singleton_ensure(world, Game);

    game->tex_ship = mye_texture_from_image(world, "gen:ship",
        disc_image(16, (Color){ 90, 160, 240, 255 }, RAYWHITE));
    game->tex_orb = mye_texture_from_image(world, "gen:orb", orb_atlas_image());
    game->tex_mine = mye_texture_from_image(world, "gen:mine",
        disc_image(12, (Color){ 210, 90, 80, 255 }, (Color){ 255, 200, 190, 255 }));
    game->tex_shield = mye_texture_from_image(world, "gen:shield",
        disc_image(8, (Color){ 240, 220, 120, 255 }, RAYWHITE));

    game->sfx_pickup = mye_sound_from_wave(world, "gen:pickup",
                                           beep_wave(0.12f, 600.0f, 1200.0f, 5.0f));
    game->sfx_hit = mye_sound_from_wave(world, "gen:hit",
                                        beep_wave(0.30f, 300.0f, 70.0f, 4.0f));

    build_prefabs(world, game);

    g_orbs = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyePosition2D), .inout = EcsIn },
                  { .id = ecs_id(Collider), .inout = EcsIn },
                  { .id = ecs_id(Orb) }},
    });
    g_mines = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyePosition2D), .inout = EcsIn },
                  { .id = ecs_id(Collider), .inout = EcsIn },
                  { .id = ecs_id(Mine) }},
    });

    /* Simulation: fixed timestep, so behaviour is framerate independent. */
    ECS_SYSTEM(world, PlayerControl, MyeOnFixedUpdate, MyePosition2D, Velocity, Player);
    ECS_SYSTEM(world, ShieldOrbit, MyeOnFixedUpdate, MyePosition2D, Shield,
               [in] Player(up ChildOf));
    ECS_SYSTEM(world, MineDrift, MyeOnFixedUpdate, MyePosition2D, [in] Velocity, [in] Mine);
    ECS_SYSTEM(world, Collisions, MyeOnFixedUpdate, [in] MyePosition2D, [in] Player);
    ECS_SYSTEM(world, Clock, MyeOnFixedUpdate, Game);

    /* Presentation and flow: variable rate. */
    ECS_SYSTEM(world, SceneSwitcher, EcsOnUpdate, Game);
    ECS_SYSTEM(world, DrawHud, MyeOnDrawUI, [in] Game);
    ECS_SYSTEM(world, DrawMenu, MyeOnDrawUI, [none] Game);

    mye_scene_register(world, &(mye_scene_desc){ .name = "menu", .load = menu_load });
    mye_scene_register(world, &(mye_scene_desc){ .name = "play", .load = play_load });
    mye_scene_switch(world, getenv("MYE_START_SCENE") ? getenv("MYE_START_SCENE") : "menu");

    MyeRenderConfig *render = ecs_singleton_ensure(world, MyeRenderConfig);
    render->clear_color = (Color){ 16, 18, 26, 255 };
    ecs_singleton_modified(world, MyeRenderConfig);

    SetRandomSeed(20260816);
    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }
    ecs_query_fini(g_orbs);
    ecs_query_fini(g_mines);
    return mye_shutdown(world);
}
