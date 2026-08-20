/* MyeNetModule: what the engine does with a connection, and -- more
 * importantly -- what it refuses to do with one it was never given. See
 * engine/net/net_module.h and plan/12-networking.md ("The flecs module").
 *
 * Every wait here is a bounded loop over mye_progress with a condition, not a
 * sleep and a hope, so a broken pump fails fast instead of hanging CI.
 *
 * Ports are fixed (reproducible failures) and unique per test (this machine
 * runs several test binaries at once). */
#define _POSIX_C_SOURCE 200809L

#include "mye_test.h"

#include "core/engine.h"
#include "net/net_module.h"

#include <time.h>

#define PORT_PUMPED 47941
#define URL_PUMPED "ws://127.0.0.1:47941/"
#define PORT_UNREGISTERED 47942
#define URL_UNREGISTERED "ws://127.0.0.1:47942/"
#define PORT_UNREGISTER 47943
#define URL_UNREGISTER "ws://127.0.0.1:47943/"
#define URL_NOBODY "ws://127.0.0.1:47944/"

#define FRAME (1.0f / 60.0f)

static void nap(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
    nanosleep(&ts, NULL);
}

static ecs_world_t *headless_world(void)
{
    return mye_init(&(mye_config){ .headless = true, .max_frames = 0 });
}

/* Records what the pump counter said the first time a fixed step ran. The
 * plan puts the pump in the input-polling slot precisely so this is 1 and not
 * 0: the simulation sees the messages that arrived for THIS frame. */
static uint64_t g_pumps_at_first_step;
static int g_steps_seen;

static void ObservePumpOrder(ecs_iter_t *it)
{
    const MyeNetStatus *net = ecs_field(it, MyeNetStatus, 0);
    if (g_steps_seen == 0) {
        g_pumps_at_first_step = net->pumps;
    }
    ++g_steps_seen;
}

TEST(the_status_singleton_follows_a_registered_connection)
{
    ecs_world_t *world = headless_world();
    ASSERT_NOT_NULL(world);
    mye_allocator alloc = mye_allocator_of(world);

    g_pumps_at_first_step = 0;
    g_steps_seen = 0;
    ECS_SYSTEM(world, ObservePumpOrder, MyeOnFixedUpdate, [in] MyeNetStatus);

    /* Nothing registered: the module exists, and does nothing. */
    const MyeNetStatus *net = ecs_singleton_get(world, MyeNetStatus);
    ASSERT_NOT_NULL(net);
    ASSERT_EQ_INT(0, net->count);
    ASSERT_EQ_INT(MYE_NET_IDLE, (int)net->status);

    mye_net_conn *server = mye_net_listen(alloc, PORT_PUMPED, NULL);
    mye_net_conn *client = mye_net_connect(alloc, URL_PUMPED, NULL);
    ASSERT_NOT_NULL(server);
    ASSERT_NOT_NULL(client);

    /* The client is registered first, so conns[0] -- the status a
     * single-connection game means when it asks -- is the client's. */
    ASSERT_TRUE(mye_net_register(world, client));
    ASSERT_TRUE(mye_net_register(world, server));

    net = ecs_singleton_get(world, MyeNetStatus);
    ASSERT_EQ_INT(2, net->count);

    for (int i = 0; i < 3000; ++i) {
        mye_progress(world, FRAME);
        net = ecs_singleton_get(world, MyeNetStatus);
        if (net->status == MYE_NET_OPEN && net->peers == 1) {
            break;
        }
        nap();
    }

    net = ecs_singleton_get(world, MyeNetStatus);
    ASSERT_EQ_INT(MYE_NET_OPEN, (int)net->status);
    ASSERT_EQ_INT(1, net->peers);
    ASSERT_TRUE(net->pumps > 0);

    /* The pump ran before the first fixed step, not after it. */
    ASSERT_TRUE(g_steps_seen > 0);
    ASSERT_EQ_U64(1, g_pumps_at_first_step);

    /* And the byte counters are the module's, gathered from both ends. */
    ASSERT_TRUE(mye_net_send(client, "hello", 5));
    for (int i = 0; i < 3000; ++i) {
        mye_progress(world, FRAME);
        if (mye_net_recv_pending(server) > 0) {
            break;
        }
        nap();
    }
    ASSERT_EQ_INT(1, mye_net_recv_pending(server));

    net = ecs_singleton_get(world, MyeNetStatus);
    ASSERT_EQ_INT(1, net->recv_pending);
    ASSERT_TRUE(net->bytes_in >= 5);
    ASSERT_TRUE(net->bytes_out >= 5);

    mye_net_unregister(world, client);
    mye_net_unregister(world, server);
    mye_net_destroy(client);
    mye_net_destroy(server);
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The rule the whole design rests on: the engine services the connections a
 * game handed it, and no others. A transport that quietly pumped every socket
 * it could find would make a tool, a test, or a second connection on a
 * different schedule impossible to write. */
TEST(an_unregistered_connection_is_never_touched)
{
    ecs_world_t *world = headless_world();
    ASSERT_NOT_NULL(world);
    mye_allocator alloc = mye_allocator_of(world);

    mye_net_conn *server = mye_net_listen(alloc, PORT_UNREGISTERED, NULL);
    mye_net_conn *client = mye_net_connect(alloc, URL_UNREGISTERED, NULL);
    ASSERT_NOT_NULL(server);
    ASSERT_NOT_NULL(client);

    ASSERT_TRUE(mye_net_register(world, server));
    /* The client is deliberately NOT registered. */

    for (int i = 0; i < 120; ++i) {
        mye_progress(world, FRAME);
        nap();
    }

    /* Its handshake never advanced, because nobody moved its bytes. */
    ASSERT_EQ_INT(MYE_NET_CONNECTING, (int)mye_net_status_of(client));
    ASSERT_EQ_INT(0, mye_net_peer_count(server));

    const MyeNetStatus *net = ecs_singleton_get(world, MyeNetStatus);
    ASSERT_EQ_INT(1, net->count);
    ASSERT_EQ_INT(0, net->peers);
    /* Exactly once per frame: the pump ran (it simply had one job), and it
     * ran once. A system that the pipeline also picked up would count twice,
     * which would mean the status a game reads was refreshed after gameplay
     * rather than before it. */
    ASSERT_EQ_U64(120, net->pumps);

    /* Nothing was wrong with the connection -- pump it by hand and it opens. */
    for (int i = 0; i < 3000; ++i) {
        mye_net_pump(client);
        mye_progress(world, FRAME);
        if (mye_net_status_of(client) == MYE_NET_OPEN) {
            break;
        }
        nap();
    }
    ASSERT_EQ_INT(MYE_NET_OPEN, (int)mye_net_status_of(client));

    mye_net_unregister(world, server);
    mye_net_destroy(client);
    mye_net_destroy(server);
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(unregistering_hands_the_connection_back)
{
    ecs_world_t *world = headless_world();
    ASSERT_NOT_NULL(world);
    mye_allocator alloc = mye_allocator_of(world);

    mye_net_conn *server = mye_net_listen(alloc, PORT_UNREGISTER, NULL);
    mye_net_conn *client = mye_net_connect(alloc, URL_UNREGISTER, NULL);
    ASSERT_NOT_NULL(server);
    ASSERT_NOT_NULL(client);
    ASSERT_TRUE(mye_net_register(world, client));
    ASSERT_TRUE(mye_net_register(world, server));

    for (int i = 0; i < 3000 && mye_net_peer_count(server) == 0; ++i) {
        mye_progress(world, FRAME);
        nap();
    }
    ASSERT_EQ_INT(1, mye_net_peer_count(server));

    ASSERT_TRUE(mye_net_unregister(world, client));
    ASSERT_FALSE(mye_net_unregister(world, client)); /* only once */

    const MyeNetStatus *net = ecs_singleton_get(world, MyeNetStatus);
    ASSERT_EQ_INT(1, net->count);

    /* The server keeps being pumped and keeps sending; the client, no longer
     * pumped, keeps receiving nothing. */
    ASSERT_TRUE(mye_net_send(server, "still here", 10));
    for (int i = 0; i < 120; ++i) {
        mye_progress(world, FRAME);
        nap();
    }
    ASSERT_EQ_INT(0, mye_net_recv_pending(client));

    /* Registered again, the same connection catches up. */
    ASSERT_TRUE(mye_net_register(world, client));
    for (int i = 0; i < 3000 && mye_net_recv_pending(client) == 0; ++i) {
        mye_progress(world, FRAME);
        nap();
    }
    ASSERT_EQ_INT(1, mye_net_recv_pending(client));

    mye_net_unregister(world, client);
    mye_net_unregister(world, server);
    mye_net_destroy(client);
    mye_net_destroy(server);
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(registering_twice_is_not_an_error)
{
    ecs_world_t *world = headless_world();
    ASSERT_NOT_NULL(world);

    /* Nothing is listening on this port; the connection's fate is beside the
     * point, since this is about the registry. */
    mye_net_conn *conn = mye_net_connect(mye_allocator_of(world), URL_NOBODY,
                                         NULL);
    ASSERT_NOT_NULL(conn);

    ASSERT_TRUE(mye_net_register(world, conn));
    ASSERT_TRUE(mye_net_register(world, conn));
    const MyeNetStatus *net = ecs_singleton_get(world, MyeNetStatus);
    ASSERT_EQ_INT(1, net->count);

    ASSERT_FALSE(mye_net_register(world, NULL));
    ASSERT_FALSE(mye_net_unregister(world, NULL));

    ASSERT_TRUE(mye_net_unregister(world, conn));
    net = ecs_singleton_get(world, MyeNetStatus);
    ASSERT_EQ_INT(0, net->count);
    ASSERT_EQ_INT(MYE_NET_IDLE, (int)net->status);

    mye_net_destroy(conn);
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(the_status_singleton_follows_a_registered_connection),
          TEST_CASE(an_unregistered_connection_is_never_touched),
          TEST_CASE(unregistering_hands_the_connection_back),
          TEST_CASE(registering_twice_is_not_an_error))
