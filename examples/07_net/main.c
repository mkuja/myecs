/* Echo: the smallest thing that proves the transport works on both targets.
 *
 *   ./example_07_net --serve        native relay, no window, echoes to all
 *   ./example_07_net                client; the web build is always this
 *
 * The relay is a native build of this same file, so the test server is the
 * engine rather than a second stack. See plan/12-networking.md.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/engine.h"
#include "core/log.h"
#include "net/net.h"
#include "render/render2d.h"

#include <raylib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PORT 9010
#define URL "ws://localhost:9010/"

/* --------------------------------------------------------------- relay -- */

static int serve(void)
{
    mye_net_conn *server = mye_net_listen(mye_heap_allocator(), PORT, NULL);
    if (server == NULL) {
        return 1;
    }

    mye_log_info("relay: ws://localhost:%d -- Ctrl-C to stop", PORT);

    /* Bounded so the smoke test terminates; a real relay would loop until
     * signalled. MYE_MAX_FRAMES is reused rather than inventing a second
     * knob, since every other example already honours it. */
    const char *limit = getenv("MYE_MAX_FRAMES");
    long ticks = limit != NULL ? atol(limit) : 0;

    for (long i = 0; ticks == 0 || i < ticks; ++i) {
        mye_net_pump(server);

        unsigned char buffer[1024];
        uint32_t peer = 0;
        size_t size;
        while ((size = mye_net_recv(server, buffer, sizeof buffer, &peer)) > 0) {
            mye_log_info("relay: %zu bytes from peer %u -> echoing to %d peers",
                         size, peer, mye_net_peer_count(server));
            if (getenv("MYE_NET_NO_ECHO") == NULL) {
                mye_net_send(server, buffer, size);
            }
        }
        /* Plain nanosleep, not raylib's WaitTime: the relay never opens a
         * window, so it must not depend on anything raylib initialises. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 };
        nanosleep(&ts, NULL);
    }

    mye_net_destroy(server);
    return 0;
}

/* -------------------------------------------------------------- client -- */

typedef struct Net {
    mye_net_conn *conn;
    int sent;
    int received;
    double next_send;
    char last[128];
} Net;

ECS_COMPONENT_DECLARE(Net);

static void NetPump(ecs_iter_t *it)
{
    Net *net = ecs_field(it, Net, 0);
    mye_net_pump(net->conn);

    unsigned char buffer[256];
    size_t size;
    while ((size = mye_net_recv(net->conn, buffer, sizeof buffer, NULL)) > 0) {
        size_t copy = size < sizeof net->last - 1 ? size : sizeof net->last - 1;
        memcpy(net->last, buffer, copy);
        net->last[copy] = '\0';
        ++net->received;
    }

    if (mye_net_status_of(net->conn) != MYE_NET_OPEN) {
        return;
    }
    double now = mye_time_now();
    if (now >= net->next_send) {
        char line[64];
        int n = snprintf(line, sizeof line, "ping %d", net->sent);
        if (mye_net_send(net->conn, line, (size_t)n)) {
            ++net->sent;
        }
        net->next_send = now + 1.0;
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

static void DrawNet(ecs_iter_t *it)
{
    const Net *net = ecs_field(it, Net, 0);
    mye_net_status status = mye_net_status_of(net->conn);

    char *line = MYE_NEW_ARRAY(mye_frame_allocator(it->world), char, 160);
    if (line == NULL) {
        return;
    }

    snprintf(line, 160, "%s  %s", URL, status_text(status));
    DrawText(line, 20, 20, 22,
             status == MYE_NET_OPEN ? (Color){ 120, 230, 190, 255 } : ORANGE);

    snprintf(line, 160, "sent %d   received %d", net->sent, net->received);
    DrawText(line, 20, 56, 20, RAYWHITE);

    snprintf(line, 160, "last echo: %s", net->last[0] != '\0' ? net->last : "-");
    DrawText(line, 20, 84, 20, (Color){ 170, 178, 195, 255 });

    if (status != MYE_NET_OPEN) {
        DrawText("start the relay:  ./example_07_net --serve", 20, 130, 18,
                 (Color){ 150, 150, 160, 255 });
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--serve") == 0) {
            return serve();
        }
    }

    ecs_world_t *world = mye_init(&(mye_config){
        .width = 720, .height = 300, .title = "myecs -- websocket echo" });
    if (world == NULL) {
        return 1;
    }

    ECS_COMPONENT_DEFINE(world, Net);
    ecs_add_id(world, ecs_id(Net), EcsSingleton);

    mye_net_conn *conn = mye_net_connect(mye_allocator_of(world), URL, NULL);
    if (conn == NULL) {
        mye_shutdown(world);
        return 1;
    }
    ecs_singleton_set(world, Net, { .conn = conn });

    ECS_SYSTEM(world, NetPump, EcsOnUpdate, Net);
    ECS_SYSTEM(world, DrawNet, MyeOnDrawUI, [in] Net);

    MyeRenderConfig *render = ecs_singleton_ensure(world, MyeRenderConfig);
    render->clear_color = (Color){ 16, 18, 26, 255 };
    ecs_singleton_modified(world, MyeRenderConfig);

    while (mye_running(world)) {
        mye_progress(world, GetFrameTime());
    }

    mye_net_destroy(conn);
    return mye_shutdown(world);
}
