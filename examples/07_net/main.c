/* N1 -- presence. Everyone connected owns a dot; the dots move on everyone's
 * screen, and there is a chat line. See plan/12-networking.md.
 *
 *   ./example_07_net --serve        the relay: native, headless, no window
 *   ./example_07_net                a client; the web build is always this
 *
 * The relay is a native build of this same file, so the "server" is the
 * engine rather than a second stack in another language.
 *
 * What this example is actually for:
 *
 * - The engine pumps the socket, not the game. mye_net_register() hands the
 *   connection to MyeNetModule, which services it once per frame in the same
 *   slot as input -- so the fixed simulation steps see this frame's messages.
 *   Nothing else on this machine is pumped: the relay above registers
 *   nothing and pumps by hand, which is the same code path a tool or a test
 *   uses.
 * - Remote dots carry MyeInterpolate. They are told where they are 15 times a
 *   second while the screen refreshes 60 or 144 times a second, and that gap
 *   is exactly what render interpolation exists to hide.
 * - Messages carry a one-byte kind prefix (presence.h). The engine has no
 *   opinion about payloads, so every game writes that layer; presence.h is
 *   the smallest honest version of it, meant to be copied.
 * - The round-trip time is on screen from the first frame. WebSocket is TCP:
 *   a lost packet stalls everything behind it, and the number showing that
 *   belongs in front of you while you build, not in a bug report later.
 */
#define _POSIX_C_SOURCE 200809L

#include "presence.h"

#include "core/engine.h"
#include "core/log.h"
#include "input/input.h"
#include "net/net_module.h"
#include "render/render2d.h"
#include "scene/transform.h"

#include <raylib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCREEN_W 900
#define SCREEN_H 560

#define MAX_REMOTES 8
#define CHAT_LOG_LINES 6
#define CHAT_LOG_WIDTH 96

/* A remote nobody has heard from for this long is gone. The transport
 * reports how many peers a listener has, not which of them just left, so
 * departure is a gameplay decision here rather than an engine event -- and a
 * timeout is what a game would need anyway the first time somebody's wifi
 * drops without closing the socket politely. */
#define REMOTE_TIMEOUT 3.0

#define MOVE_SPEED 260.0f
#define DOT_RADIUS 14.0f

/* How hard a remote dot is pulled towards the position that last arrived,
 * per fixed step. The engine's interpolation smooths between fixed steps;
 * this smooths between packets, which is the game's half of the problem. A
 * constant fraction is safe here only because a fixed step is a constant
 * length -- the same line in a frame-rate-dependent system would be a bug. */
#define REMOTE_EASE 0.25f

/* Actions, not key codes: gameplay never mentions a key (input/input.h). */
enum { ACT_MOVE_X, ACT_MOVE_Y };

/* --------------------------------------------------------------- relay -- */

static int serve(void)
{
    /* No world, no module, no registration: this process pumps its one
     * connection by hand. That is the point of registration being explicit --
     * the engine services what it was given, and a headless tool that never
     * builds a world is not a second-class citizen. */
    mye_net_conn *server = mye_net_listen(mye_heap_allocator(), PRESENCE_PORT,
                                          NULL);
    if (server == NULL) {
        return 1;
    }

    mye_log_info("relay: ws://localhost:%d -- Ctrl-C to stop", PRESENCE_PORT);

    /* Bounded so a smoke test terminates; a real relay would loop until
     * signalled. MYE_MAX_FRAMES is reused rather than inventing a second
     * knob, since every other example already honours it. */
    const char *limit = getenv("MYE_MAX_FRAMES");
    long ticks = limit != NULL ? atol(limit) : 0;

    for (long i = 0; ticks == 0 || i < ticks; ++i) {
        mye_net_pump(server);
        presence_relay_tick(server);

        /* Plain nanosleep, not raylib's WaitTime: the relay never opens a
         * window, so it must not depend on anything raylib initialises. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 };
        nanosleep(&ts, NULL);
    }

    mye_net_destroy(server);
    return 0;
}

/* ------------------------------------------------------------ components -- */

typedef struct Player {
    uint32_t id;   /* 0 until the relay says who we are */
    Color color;
    bool local;
} Player;

/* On remote dots only. The position that last arrived, and when. */
typedef struct Remote {
    float target_x, target_y;
    double last_seen; /* MyeTime.elapsed */
} Remote;

typedef struct Presence {
    mye_net_conn *conn;
    uint32_t self_id;
    ecs_entity_t self;

    /* id -> entity, so an arriving position finds its dot without a lookup
     * by name. Small and linear on purpose: eight dots is a party, and a
     * hash table here would be more machinery than the whole example. */
    struct {
        uint32_t id;
        ecs_entity_t entity;
    } remotes[MAX_REMOTES];
    int remote_count;

    double ping_sent; /* clock when the ping in flight left; 0 = none */
    double rtt_ms;    /* < 0 means the last ping never came back */
    int sent;
    int received;

    char typing[PRESENCE_MAX_CHAT + 1];
    int typing_len;
    char log[CHAT_LOG_LINES][CHAT_LOG_WIDTH];
    int log_count;

    /* The engine never reconnects by itself; this is the game doing it, with
     * the engine's timing helper. See net.h. */
    mye_net_backoff backoff;
    int reconnects;
} Presence;

ECS_COMPONENT_DECLARE(Player);
ECS_COMPONENT_DECLARE(Remote);
ECS_COMPONENT_DECLARE(Presence);

/* ---------------------------------------------------------------- helpers -- */

static Color color_for(uint32_t id)
{
    static const Color palette[] = {
        { 120, 210, 255, 255 }, { 255, 170, 120, 255 }, { 170, 255, 160, 255 },
        { 255, 140, 190, 255 }, { 220, 200, 120, 255 }, { 180, 160, 255, 255 },
    };
    return palette[id % (sizeof palette / sizeof palette[0])];
}

static void chat_log_push(Presence *p, const char *line)
{
    if (p->log_count == CHAT_LOG_LINES) {
        for (int i = 1; i < CHAT_LOG_LINES; ++i) {
            memcpy(p->log[i - 1], p->log[i], CHAT_LOG_WIDTH);
        }
        --p->log_count;
    }
    snprintf(p->log[p->log_count], CHAT_LOG_WIDTH, "%s", line);
    ++p->log_count;
}

static ecs_entity_t remote_entity(ecs_world_t *world, Presence *p, uint32_t id,
                                  float x, float y)
{
    for (int i = 0; i < p->remote_count; ++i) {
        if (p->remotes[i].id == id) {
            return p->remotes[i].entity;
        }
    }
    if (p->remote_count == MAX_REMOTES) {
        return 0;
    }

    ecs_entity_t e = mye_spawn_2d(world, (Vector2){ x, y });
    /* prev = current and snap set, so the first frame draws the dot where it
     * actually is instead of blending it in from the origin. At runtime the
     * same thing is said with mye_transform_snap(); at spawn the component is
     * not there yet to snap. */
    ecs_set(world, e, MyeInterpolate, { .prev_x = x, .prev_y = y,
                                        .snap = true });
    ecs_set(world, e, Player, { .id = id, .color = color_for(id),
                                .local = false });
    ecs_set(world, e, Remote, { .target_x = x, .target_y = y });

    char name[32];
    snprintf(name, sizeof name, "peer_%u", id);
    ecs_set_name(world, e, name);

    p->remotes[p->remote_count].id = id;
    p->remotes[p->remote_count].entity = e;
    ++p->remote_count;
    mye_log_info("client: peer %u joined (%d remote dots)", id,
                 p->remote_count);
    return e;
}

static void forget_remote(Presence *p, ecs_entity_t entity)
{
    for (int i = 0; i < p->remote_count; ++i) {
        if (p->remotes[i].entity != entity) {
            continue;
        }
        p->remotes[i] = p->remotes[p->remote_count - 1];
        --p->remote_count;
        return;
    }
}

static void forget_everyone(ecs_world_t *world, Presence *p)
{
    for (int i = 0; i < p->remote_count; ++i) {
        ecs_delete(world, p->remotes[i].entity);
    }
    p->remote_count = 0;
    p->self_id = 0;
    p->ping_sent = 0.0;
    p->rtt_ms = 0.0;
}

/* ------------------------------------------------------------- receiving -- */

/* Runs in a fixed step, which is why the module pumps before them: a message
 * that arrived this frame is applied in this frame's simulation rather than
 * the next one. */
static void PresenceReceive(ecs_iter_t *it)
{
    Presence *p = ecs_field(it, Presence, 0);
    ecs_world_t *world = it->world;

    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    double now = time != NULL ? time->elapsed : 0.0;

    unsigned char buffer[PRESENCE_MAX_MESSAGE];
    size_t size;
    while ((size = mye_net_recv(p->conn, buffer, sizeof buffer, NULL)) > 0) {
        ++p->received;

        presence_msg msg;
        if (!presence_decode(buffer, size, &msg)) {
            mye_log_warn("client: dropping %zu bytes it cannot parse", size);
            continue;
        }

        /* Routing on the kind byte, which is the whole reason it is there:
         * this switch decides what the rest of the bytes even mean. */
        switch (msg.kind) {
        case PRESENCE_WELCOME: {
            p->self_id = msg.id;
            ecs_set(world, p->self, Player, { .id = msg.id,
                                              .color = color_for(msg.id),
                                              .local = true });

            /* A spawn point per identity, so two clients on one machine do
             * not start exactly on top of each other. Position and previous
             * position are set together, which is what mye_transform_snap
             * does at runtime -- without it the dot would be drawn smeared
             * across the screen for one frame on the way to its spot. */
            float sx = 120.0f + (float)(msg.id % 5u) * 160.0f;
            float sy = 180.0f + (float)((msg.id / 5u) % 3u) * 120.0f;
            ecs_set(world, p->self, MyePosition2D, { sx, sy });
            ecs_set(world, p->self, MyeInterpolate, { .prev_x = sx,
                                                      .prev_y = sy,
                                                      .snap = true });
            mye_log_info("client: the relay calls us #%u", msg.id);
            break;
        }

        case PRESENCE_STATE: {
            if (msg.id == p->self_id) {
                break; /* our own position, broadcast back to us */
            }
            ecs_entity_t e = remote_entity(world, p, msg.id, msg.x, msg.y);
            if (e != 0) {
                ecs_set(world, e, Remote, { .target_x = msg.x,
                                            .target_y = msg.y,
                                            .last_seen = now });
            }
            break;
        }

        case PRESENCE_CHAT: {
            char line[CHAT_LOG_WIDTH];
            snprintf(line, sizeof line, "#%u  %s", msg.id, msg.text);
            chat_log_push(p, line);
            break;
        }

        case PRESENCE_PING:
            /* Our own ping, back from the relay. Both stamps come from this
             * machine's clock, so the two ends need not agree about time. */
            if (p->ping_sent > 0.0) {
                p->rtt_ms = (mye_time_now() - msg.stamp) * 1000.0;
                p->ping_sent = 0.0;
            }
            break;

        default:
            break;
        }
    }
}

/* -------------------------------------------------------------- sending -- */

static void PresenceSend(ecs_iter_t *it)
{
    Presence *p = ecs_field(it, Presence, 0);
    ecs_world_t *world = it->world;

    if (mye_net_status_of(p->conn) != MYE_NET_OPEN) {
        return;
    }
    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    if (time == NULL) {
        return;
    }
    unsigned char buffer[PRESENCE_MAX_MESSAGE];

    /* Introduce ourselves until the relay answers. Retried rather than sent
     * once: the connection may have opened between two steps, and a lost
     * hello would otherwise leave this client nameless forever. */
    if (p->self_id == 0) {
        if (time->fixed_step % 30 == 0) {
            size_t n = presence_encode_hello(buffer, sizeof buffer);
            if (mye_net_send(p->conn, buffer, n)) {
                ++p->sent;
            }
        }
        return;
    }

    /* Position, at the network rate rather than the step rate. Sending every
     * step would be four times the traffic for motion nobody could see. */
    const uint64_t every = 60 / PRESENCE_SEND_HZ;
    if (time->fixed_step % every == 0) {
        const MyePosition2D *pos = ecs_get(world, p->self, MyePosition2D);
        if (pos != NULL) {
            size_t n = presence_encode_state(buffer, sizeof buffer, p->self_id,
                                             pos->x, pos->y);
            if (mye_net_send(p->conn, buffer, n)) {
                ++p->sent;
            }
        }
    }

    /* Round trip, twice a second, one in flight at a time. */
    if (p->ping_sent > 0.0 && mye_time_now() - p->ping_sent > 2.0) {
        p->ping_sent = 0.0;
        p->rtt_ms = -1.0; /* it never came back; say so rather than freeze */
    }
    if (p->ping_sent == 0.0 && time->fixed_step % 30 == 0) {
        double stamp = mye_time_now();
        size_t n = presence_encode_ping(buffer, sizeof buffer, p->self_id,
                                        stamp);
        if (mye_net_send(p->conn, buffer, n)) {
            p->ping_sent = stamp;
            ++p->sent;
        }
    }
}

/* ------------------------------------------------------------ simulation -- */

static void LocalMove(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const Player *player = ecs_field(it, Player, 1);
    float dt = (float)it->delta_time;

    float x = mye_action_value(it->world, ACT_MOVE_X);
    float y = mye_action_value(it->world, ACT_MOVE_Y);

    for (int i = 0; i < it->count; ++i) {
        if (!player[i].local) {
            continue;
        }
        pos[i].x += x * MOVE_SPEED * dt;
        pos[i].y += y * MOVE_SPEED * dt;
        if (pos[i].x < DOT_RADIUS) pos[i].x = DOT_RADIUS;
        if (pos[i].y < DOT_RADIUS) pos[i].y = DOT_RADIUS;
        if (pos[i].x > (float)SCREEN_W - DOT_RADIUS) {
            pos[i].x = (float)SCREEN_W - DOT_RADIUS;
        }
        if (pos[i].y > (float)SCREEN_H - DOT_RADIUS) {
            pos[i].y = (float)SCREEN_H - DOT_RADIUS;
        }
    }
}

static void RemoteEase(ecs_iter_t *it)
{
    MyePosition2D *pos = ecs_field(it, MyePosition2D, 0);
    const Remote *remote = ecs_field(it, Remote, 1);

    for (int i = 0; i < it->count; ++i) {
        pos[i].x += (remote[i].target_x - pos[i].x) * REMOTE_EASE;
        pos[i].y += (remote[i].target_y - pos[i].y) * REMOTE_EASE;
    }
}

static void RemoteExpire(ecs_iter_t *it)
{
    const Remote *remote = ecs_field(it, Remote, 0);
    ecs_world_t *world = it->world;

    Presence *p = ecs_singleton_ensure(world, Presence);
    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    if (p == NULL || time == NULL) {
        return;
    }

    for (int i = 0; i < it->count; ++i) {
        if (remote[i].last_seen <= 0.0 ||
            time->elapsed - remote[i].last_seen < REMOTE_TIMEOUT) {
            continue;
        }
        mye_log_info("client: peer went quiet; removing its dot");
        forget_remote(p, it->entities[i]);
        ecs_delete(world, it->entities[i]);
    }
}

/* Reconnecting is the game's decision, not the engine's: this system is what
 * the engine deliberately does not do for you. All it borrows is the timing
 * ladder (net.h), so nobody has to re-derive "wait, then wait longer". */
static void Reconnect(ecs_iter_t *it)
{
    Presence *p = ecs_field(it, Presence, 0);
    ecs_world_t *world = it->world;
    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    double dt = time != NULL ? (double)time->delta : 0.0;

    mye_net_status status = mye_net_status_of(p->conn);
    if (status == MYE_NET_OPEN) {
        mye_net_backoff_connected(&p->backoff);
        return;
    }
    if (status == MYE_NET_ERROR || status == MYE_NET_CLOSED) {
        mye_net_backoff_failed(&p->backoff);
    }
    if (!mye_net_backoff_ready(&p->backoff, dt)) {
        return;
    }

    /* Unregister before destroying, always: the module would otherwise pump
     * a freed connection next frame. */
    mye_net_unregister(world, p->conn);
    mye_net_destroy(p->conn);
    forget_everyone(world, p);

    p->conn = mye_net_connect(mye_allocator_of(world), PRESENCE_URL, NULL);
    if (p->conn != NULL) {
        mye_net_register(world, p->conn);
    }
    ++p->reconnects;
    mye_log_info("client: redialling (attempt %d)", p->reconnects);
}

static void ChatInput(ecs_iter_t *it)
{
    Presence *p = ecs_field(it, Presence, 0);

    /* Arrow keys move, so typing can stay unmoded: every printable character
     * goes into the line, and nothing a player types steers the dot. */
    for (int c = GetCharPressed(); c != 0; c = GetCharPressed()) {
        if (c >= 32 && c < 127 && p->typing_len < PRESENCE_MAX_CHAT) {
            p->typing[p->typing_len++] = (char)c;
            p->typing[p->typing_len] = '\0';
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) && p->typing_len > 0) {
        p->typing[--p->typing_len] = '\0';
    }
    if (!IsKeyPressed(KEY_ENTER) || p->typing_len == 0) {
        return;
    }

    unsigned char buffer[PRESENCE_MAX_MESSAGE];
    size_t n = presence_encode_chat(buffer, sizeof buffer, p->self_id,
                                    p->typing);
    if (n > 0 && mye_net_send(p->conn, buffer, n)) {
        ++p->sent;
        /* Not echoed locally: it comes back from the relay like everyone
         * else's, so every client sees the same conversation in the same
         * order. One wasted message buys one fewer thing to get wrong. */
        p->typing_len = 0;
        p->typing[0] = '\0';
    }
}

/* -------------------------------------------------------------- drawing -- */

static void DrawPlayers(ecs_iter_t *it)
{
    const Player *player = ecs_field(it, Player, 0);
    ecs_world_t *world = it->world;

    for (int i = 0; i < it->count; ++i) {
        /* The DRAWN position: for a remote dot this is the blended one, which
         * is the whole reason MyeInterpolate is on it. */
        Vector3 at = mye_render_position(world, it->entities[i]);
        Vector2 v = { at.x, at.y };

        DrawCircleV(v, DOT_RADIUS, player[i].color);
        if (player[i].local) {
            DrawCircleLinesV(v, DOT_RADIUS + 5.0f, RAYWHITE);
        }

        char label[16];
        snprintf(label, sizeof label, "#%u", player[i].id);
        DrawText(label, (int)v.x - 10, (int)(v.y - DOT_RADIUS - 20.0f), 18,
                 player[i].color);
    }
}

static const char *status_text(mye_net_status status)
{
    switch (status) {
    case MYE_NET_CONNECTING: return "connecting";
    case MYE_NET_OPEN:       return "open";
    case MYE_NET_CLOSED:     return "closed";
    case MYE_NET_ERROR:      return "error";
    default:                 return "idle";
    }
}

static void DrawHud(ecs_iter_t *it)
{
    const Presence *p = ecs_field(it, Presence, 0);
    /* The engine's own view of the network, as data. Nothing here calls the
     * transport: the module already gathered it this frame. */
    const MyeNetStatus *net = ecs_field(it, MyeNetStatus, 1);

    char *line = MYE_NEW_ARRAY(mye_frame_allocator(it->world), char, 160);
    if (line == NULL) {
        return;
    }
    const Color dim = { 150, 152, 165, 255 };

    bool open = net->status == MYE_NET_OPEN;
    if (p->self_id != 0) {
        snprintf(line, 160, "%s  %s  you are #%u", PRESENCE_URL,
                 status_text(net->status), p->self_id);
    } else {
        snprintf(line, 160, "%s  %s", PRESENCE_URL, status_text(net->status));
    }
    DrawText(line, 16, 14, 20, open ? (Color){ 120, 230, 190, 255 } : ORANGE);

    if (p->rtt_ms < 0.0) {
        snprintf(line, 160, "round trip: lost");
    } else if (p->rtt_ms > 0.0) {
        snprintf(line, 160, "round trip: %.1f ms", p->rtt_ms);
    } else {
        snprintf(line, 160, "round trip: -");
    }
    DrawText(line, 16, 40, 20, RAYWHITE);

    snprintf(line, 160,
             "sent %d  received %d  in %llu B  out %llu B  queued %d/%d"
             "  redials %d",
             p->sent, p->received, (unsigned long long)net->bytes_in,
             (unsigned long long)net->bytes_out, net->recv_pending,
             net->send_pending, p->reconnects);
    DrawText(line, 16, 64, 18, dim);

    int y = SCREEN_H - 40 - CHAT_LOG_LINES * 20;
    for (int i = 0; i < p->log_count; ++i) {
        DrawText(p->log[i], 16, y, 18, RAYWHITE);
        y += 20;
    }

    snprintf(line, 160, "> %s_", p->typing);
    DrawText(line, 16, SCREEN_H - 34, 20,
             open ? (Color){ 200, 220, 255, 255 } : dim);

    if (!open) {
        double wait = mye_net_backoff_remaining(&p->backoff);
        if (wait > 0.0) {
            snprintf(line, 160, "no relay -- retrying in %.1f s", wait);
        } else {
            snprintf(line, 160,
                     "no relay -- start one with:  ./example_07_net --serve");
        }
        DrawText(line, 16, 96, 18, ORANGE);
    } else {
        snprintf(line, 160, "arrows move   type + Enter to chat");
        DrawText(line, 16, 96, 18, dim);
    }
}

/* -------------------------------------------------------------- lifecycle -- */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--serve") == 0) {
            return serve();
        }
    }

    ecs_world_t *world = mye_init(&(mye_config){
        .width = SCREEN_W, .height = SCREEN_H,
        .title = "myecs -- websocket presence" });
    if (world == NULL) {
        return 1;
    }

    ECS_COMPONENT_DEFINE(world, Player);
    ECS_COMPONENT_DEFINE(world, Remote);
    ECS_COMPONENT_DEFINE(world, Presence);
    ecs_add_id(world, ecs_id(Presence), EcsSingleton);

    mye_net_conn *conn = mye_net_connect(mye_allocator_of(world), PRESENCE_URL,
                                         NULL);
    if (conn == NULL) {
        mye_shutdown(world);
        return 1;
    }
    /* From here the engine pumps it, once per frame, before the fixed steps.
     * Nothing else on this machine is touched. */
    mye_net_register(world, conn);

    ecs_entity_t self = mye_spawn_2d(world,
                                     (Vector2){ SCREEN_W * 0.5f,
                                                SCREEN_H * 0.5f });
    ecs_set(world, self, MyeInterpolate, { .prev_x = SCREEN_W * 0.5f,
                                           .prev_y = SCREEN_H * 0.5f,
                                           .snap = true });
    ecs_set(world, self, Player, { .id = 0, .color = (Color){ 200, 200, 210,
                                                              255 },
                                   .local = true });
    ecs_set_name(world, self, "me");

    ecs_singleton_set(world, Presence, { .conn = conn, .self = self });
    Presence *presence = ecs_singleton_ensure(world, Presence);
    mye_net_backoff_init(&presence->backoff, &(mye_net_backoff_config){
        .first_delay = 0.5, .max_delay = 5.0, .factor = 2.0, .jitter = 0.2,
        .seed = 20260820u });
    ecs_singleton_modified(world, Presence);

    mye_input_bind_axis_keys(world, ACT_MOVE_X, KEY_LEFT, KEY_RIGHT);
    mye_input_bind_axis_keys(world, ACT_MOVE_Y, KEY_UP, KEY_DOWN);

    /* Registration order fixes the order within a phase, and it matters:
     * receive applies what arrived, then the dots move, then this step's
     * position goes out. */
    ECS_SYSTEM(world, PresenceReceive, MyeOnFixedUpdate, Presence);
    ECS_SYSTEM(world, LocalMove, MyeOnFixedUpdate, MyePosition2D, [in] Player);
    ECS_SYSTEM(world, RemoteEase, MyeOnFixedUpdate, MyePosition2D, [in] Remote);
    ECS_SYSTEM(world, PresenceSend, MyeOnFixedUpdate, Presence);

    ECS_SYSTEM(world, ChatInput, EcsOnUpdate, Presence);
    ECS_SYSTEM(world, RemoteExpire, EcsOnUpdate, [in] Remote);
    ECS_SYSTEM(world, Reconnect, EcsOnUpdate, Presence);

    ECS_SYSTEM(world, DrawPlayers, MyeOnDraw2D, [in] Player);
    ECS_SYSTEM(world, DrawHud, MyeOnDrawUI, [in] Presence, [in] MyeNetStatus);

    MyeRenderConfig *render = ecs_singleton_ensure(world, MyeRenderConfig);
    render->clear_color = (Color){ 16, 18, 26, 255 };
    ecs_singleton_modified(world, MyeRenderConfig);

    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }

    /* Reconnecting may have replaced the connection, so close the one that is
     * live now -- and unregister before destroying. */
    const Presence *final = ecs_singleton_get(world, Presence);
    mye_net_conn *live = final != NULL ? final->conn : conn;
    mye_net_unregister(world, live);
    mye_net_destroy(live);
    return mye_shutdown(world);
}
