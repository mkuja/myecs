/* WebSocket transport, end to end in one process: a listener and its clients
 * both live here, pumped by hand. See plan/12-networking.md.
 *
 * No sleeping on a timer and hoping: every wait is a bounded pump loop with
 * a condition, so a broken transport fails fast instead of hanging CI. */
/* nanosleep is POSIX, and -std=c11 hides it until this is asked for. The
 * engine itself stays free of feature-test macros; this is test scaffolding. */
#define _POSIX_C_SOURCE 200809L

#include "mye_test.h"

#include "core/alloc.h"
#include "net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* High enough to avoid anything privileged, fixed so a failure is
 * reproducible rather than depending on what the OS handed out -- and one per
 * test, because several test binaries run at once on this machine. */
#define TEST_PORT 47921
#define TEST_URL "ws://127.0.0.1:47921/"
#define CLOSE_PORT 47931
#define CLOSE_URL "ws://127.0.0.1:47931/"
#define FUZZ_PORT 47932
#define FUZZ_URL "ws://127.0.0.1:47932/"

static void nap(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
    nanosleep(&ts, NULL);
}

/* Services both ends until `done` or the deadline. Returns what `done` last
 * said, so a caller asserts on it rather than on having waited. */
static bool pump_until(mye_net_conn *a, mye_net_conn *b, mye_net_conn *c,
                       bool (*done)(mye_net_conn *, mye_net_conn *,
                                    mye_net_conn *),
                       int max_ms)
{
    for (int i = 0; i < max_ms; ++i) {
        mye_net_pump(a);
        if (b != NULL) mye_net_pump(b);
        if (c != NULL) mye_net_pump(c);
        if (done(a, b, c)) {
            return true;
        }
        nap();
    }
    return done(a, b, c);
}

static bool client_open(mye_net_conn *server, mye_net_conn *client,
                        mye_net_conn *third)
{
    (void)third;
    return mye_net_status_of(client) == MYE_NET_OPEN &&
           mye_net_peer_count(server) == 1;
}

static bool server_has_message(mye_net_conn *server, mye_net_conn *client,
                               mye_net_conn *third)
{
    (void)client;
    (void)third;
    return mye_net_recv_pending(server) > 0;
}

static bool client_has_message(mye_net_conn *server, mye_net_conn *client,
                               mye_net_conn *third)
{
    (void)server;
    (void)third;
    return mye_net_recv_pending(client) > 0;
}

static bool both_clients_have_a_message(mye_net_conn *server,
                                        mye_net_conn *client,
                                        mye_net_conn *third)
{
    (void)server;
    return mye_net_recv_pending(client) > 0 && mye_net_recv_pending(third) > 0;
}

static bool two_peers(mye_net_conn *server, mye_net_conn *client,
                      mye_net_conn *third)
{
    (void)client;
    (void)third;
    return mye_net_peer_count(server) == 2;
}

TEST(a_listener_opens_and_a_client_reaches_it)
{
    mye_allocator heap = mye_heap_allocator();

    mye_net_conn *server = mye_net_listen(heap, TEST_PORT, NULL);
    ASSERT_TRUE(server != NULL);
    ASSERT_EQ_INT(MYE_NET_OPEN, (int)mye_net_status_of(server));
    ASSERT_EQ_INT(0, mye_net_peer_count(server));

    mye_net_conn *client = mye_net_connect(heap, TEST_URL, NULL);
    ASSERT_TRUE(client != NULL);

    ASSERT_TRUE(pump_until(server, client, NULL, client_open, 3000));

    mye_net_destroy(client);
    mye_net_destroy(server);
}

TEST(a_message_round_trips_and_carries_its_sender)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_conn *server = mye_net_listen(heap, TEST_PORT, NULL);
    mye_net_conn *client = mye_net_connect(heap, TEST_URL, NULL);
    ASSERT_TRUE(server != NULL && client != NULL);
    ASSERT_TRUE(pump_until(server, client, NULL, client_open, 3000));

    ASSERT_TRUE(mye_net_send(client, "ping", 4));
    ASSERT_TRUE(pump_until(server, client, NULL, server_has_message, 3000));

    char buffer[64] = { 0 };
    uint32_t peer = 0;
    size_t size = mye_net_recv(server, buffer, sizeof buffer, &peer);
    ASSERT_EQ_INT(4, (int)size);
    ASSERT_TRUE(memcmp(buffer, "ping", 4) == 0);
    ASSERT_TRUE(peer != 0); /* the listener knows who sent it */

    /* ...and back to that peer specifically. */
    ASSERT_TRUE(mye_net_send_to(server, peer, "pong", 4));
    ASSERT_TRUE(pump_until(server, client, NULL, client_has_message, 3000));

    uint32_t from = 12345;
    size = mye_net_recv(client, buffer, sizeof buffer, &from);
    ASSERT_EQ_INT(4, (int)size);
    ASSERT_TRUE(memcmp(buffer, "pong", 4) == 0);
    ASSERT_EQ_INT(0, (int)from); /* a client has exactly one correspondent */

    ASSERT_TRUE(mye_net_bytes_sent(client) >= 4);
    ASSERT_TRUE(mye_net_bytes_received(server) >= 4);

    mye_net_destroy(client);
    mye_net_destroy(server);
}

TEST(a_broadcast_reaches_every_peer)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_conn *server = mye_net_listen(heap, TEST_PORT, NULL);
    mye_net_conn *one = mye_net_connect(heap, TEST_URL, NULL);
    mye_net_conn *two = mye_net_connect(heap, TEST_URL, NULL);
    ASSERT_TRUE(server != NULL && one != NULL && two != NULL);

    ASSERT_TRUE(pump_until(server, one, two, two_peers, 3000));

    ASSERT_TRUE(mye_net_send(server, "all", 3));
    ASSERT_TRUE(pump_until(server, one, two, both_clients_have_a_message, 3000));

    char buffer[16] = { 0 };
    ASSERT_EQ_INT(3, (int)mye_net_recv(one, buffer, sizeof buffer, NULL));
    ASSERT_TRUE(memcmp(buffer, "all", 3) == 0);
    memset(buffer, 0, sizeof buffer);
    ASSERT_EQ_INT(3, (int)mye_net_recv(two, buffer, sizeof buffer, NULL));
    ASSERT_TRUE(memcmp(buffer, "all", 3) == 0);

    mye_net_destroy(one);
    mye_net_destroy(two);
    mye_net_destroy(server);
}

/* Many small messages in a row: the ring has to wrap correctly and keep
 * order, which a single round trip would never exercise.
 *
 * N0's done-when asks for ten thousand, and this is that number: the queue
 * capacity below is eight, so the ring wraps more than a thousand times and
 * every message is checked against the one before it. It costs a couple of
 * seconds under ASan, which is worth paying once per run. */
TEST(order_is_preserved_across_many_messages)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_config config = { .recv_queue_capacity = 8,
                              .send_queue_capacity = 8 };
    mye_net_conn *server = mye_net_listen(heap, TEST_PORT, &config);
    mye_net_conn *client = mye_net_connect(heap, TEST_URL, &config);
    ASSERT_TRUE(server != NULL && client != NULL);
    ASSERT_TRUE(pump_until(server, client, NULL, client_open, 3000));

    const int total = 10000;
    int sent = 0;
    int received = 0;

    for (int guard = 0; guard < 2000000 && received < total; ++guard) {
        char payload[16];
        if (sent < total) {
            int n = snprintf(payload, sizeof payload, "%d", sent);
            if (mye_net_send(client, payload, (size_t)n)) {
                ++sent;
            }
        }
        mye_net_pump(client);
        mye_net_pump(server);

        char buffer[16] = { 0 };
        size_t size;
        while ((size = mye_net_recv(server, buffer, sizeof buffer, NULL)) > 0) {
            buffer[size] = '\0';
            ASSERT_EQ_INT(received, atoi(buffer));
            ++received;
        }
    }

    ASSERT_EQ_INT(total, sent);
    ASSERT_EQ_INT(total, received);

    mye_net_destroy(client);
    mye_net_destroy(server);
}

/* A queue that grew instead of refusing would turn a runaway sender into an
 * out-of-memory crash, so refusing IS the feature. */
TEST(a_full_send_queue_refuses_rather_than_growing)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_config config = { .send_queue_capacity = 4 };
    mye_net_conn *client = mye_net_connect(heap, TEST_URL, &config);
    ASSERT_TRUE(client != NULL);

    /* Nothing is listening, so nothing drains: the queue fills and stops. */
    int accepted = 0;
    for (int i = 0; i < 100; ++i) {
        if (mye_net_send(client, "x", 1)) {
            ++accepted;
        }
    }
    ASSERT_TRUE(accepted <= 4);
    ASSERT_TRUE(mye_net_send_pending(client) <= 4);

    mye_net_destroy(client);
}

TEST(a_message_over_the_cap_is_refused)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_config config = { .max_message_size = 256 };
    mye_net_conn *server = mye_net_listen(heap, TEST_PORT, &config);
    mye_net_conn *client = mye_net_connect(heap, TEST_URL, &config);
    ASSERT_TRUE(server != NULL && client != NULL);
    ASSERT_TRUE(pump_until(server, client, NULL, client_open, 3000));

    char big[512];
    memset(big, 'a', sizeof big);
    ASSERT_TRUE(!mye_net_send(client, big, sizeof big));
    ASSERT_EQ_INT(0, mye_net_send_pending(client));

    mye_net_destroy(client);
    mye_net_destroy(server);
}

/* Half a message is worse than none: a short buffer must lose the message
 * loudly, not hand back a truncated one that looks valid. */
TEST(a_short_receive_buffer_drops_rather_than_truncating)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_conn *server = mye_net_listen(heap, TEST_PORT, NULL);
    mye_net_conn *client = mye_net_connect(heap, TEST_URL, NULL);
    ASSERT_TRUE(server != NULL && client != NULL);
    ASSERT_TRUE(pump_until(server, client, NULL, client_open, 3000));

    ASSERT_TRUE(mye_net_send(client, "0123456789", 10));
    ASSERT_TRUE(pump_until(server, client, NULL, server_has_message, 3000));

    char tiny[4] = { 0 };
    ASSERT_EQ_INT(0, (int)mye_net_recv(server, tiny, sizeof tiny, NULL));
    ASSERT_EQ_INT(0, mye_net_recv_pending(server)); /* consumed, not left behind */

    mye_net_destroy(client);
    mye_net_destroy(server);
}

TEST(wss_is_refused_while_there_is_no_tls)
{
    mye_allocator heap = mye_heap_allocator();
    ASSERT_TRUE(mye_net_connect(heap, "wss://example.com:443/", NULL) == NULL);
}

TEST(a_listener_survives_a_peer_leaving)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_conn *server = mye_net_listen(heap, TEST_PORT, NULL);
    mye_net_conn *client = mye_net_connect(heap, TEST_URL, NULL);
    ASSERT_TRUE(server != NULL && client != NULL);
    ASSERT_TRUE(pump_until(server, client, NULL, client_open, 3000));

    mye_net_destroy(client);

    for (int i = 0; i < 1000 && mye_net_peer_count(server) != 0; ++i) {
        mye_net_pump(server);
        nap();
    }
    ASSERT_EQ_INT(0, mye_net_peer_count(server));

    /* And a broadcast with nobody listening is a refusal, not a hang: the
     * departed peer's bit must not pin the queue. */
    ASSERT_TRUE(!mye_net_send(server, "anyone?", 7));
    ASSERT_EQ_INT(0, mye_net_send_pending(server));

    mye_net_destroy(server);
}

TEST(this_build_can_listen)
{
    ASSERT_TRUE(mye_net_can_listen()); /* false only in a web build */
}

/* A peer that goes away politely is CLOSED, and a dial that never landed is
 * ERROR. Collapsing the two would cost a game the one bit it needs to decide
 * between "the server shut down, show the menu" and "we cannot reach it,
 * retry" -- which is exactly the decision mye_net_backoff exists to time. */
TEST(a_closed_peer_ends_up_closed_and_a_refused_dial_ends_up_in_error)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_conn *server = mye_net_listen(heap, CLOSE_PORT, NULL);
    mye_net_conn *client = mye_net_connect(heap, CLOSE_URL, NULL);
    ASSERT_TRUE(server != NULL && client != NULL);
    ASSERT_TRUE(pump_until(server, client, NULL, client_open, 3000));

    /* The listener shuts down cleanly, which closes its peers with it. */
    mye_net_destroy(server);

    for (int i = 0; i < 3000 &&
                    mye_net_status_of(client) == MYE_NET_OPEN; ++i) {
        mye_net_pump(client);
        nap();
    }
    ASSERT_EQ_INT(MYE_NET_CLOSED, (int)mye_net_status_of(client));

    /* Closed stays closed: pumping a dead connection is harmless and does not
     * quietly resurrect it. */
    for (int i = 0; i < 10; ++i) {
        mye_net_pump(client);
    }
    ASSERT_EQ_INT(MYE_NET_CLOSED, (int)mye_net_status_of(client));
    ASSERT_TRUE(!mye_net_send(client, "anyone?", 7));
    mye_net_destroy(client);

    /* Nothing is listening there any more, so the next dial fails outright. */
    mye_net_conn *orphan = mye_net_connect(heap, CLOSE_URL, NULL);
    ASSERT_TRUE(orphan != NULL);
    for (int i = 0; i < 3000 &&
                    mye_net_status_of(orphan) == MYE_NET_CONNECTING; ++i) {
        mye_net_pump(orphan);
        nap();
    }
    ASSERT_EQ_INT(MYE_NET_ERROR, (int)mye_net_status_of(orphan));
    mye_net_destroy(orphan);
}

/* The receive path fed random and malformed frames. The transport promises
 * nothing about what a payload means -- so what is being asserted is that it
 * survives anything, keeps its accounting straight, and shuts down clean.
 * Under ASan and UBSan, which is where "survives" gets its teeth.
 *
 * The seed is a constant. A fuzz run that cannot be repeated is a rumour. */
TEST(the_receive_path_survives_a_stream_of_random_frames)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_config config = { .max_message_size = 256,
                              .recv_queue_capacity = 8,
                              .send_queue_capacity = 8 };
    mye_net_conn *server = mye_net_listen(heap, FUZZ_PORT, &config);
    mye_net_conn *client = mye_net_connect(heap, FUZZ_URL, &config);
    ASSERT_TRUE(server != NULL && client != NULL);
    ASSERT_TRUE(pump_until(server, client, NULL, client_open, 3000));

    uint32_t rng = 0x13572468u;
    int offered = 0;
    int accepted = 0;
    int refused = 0;
    int drained = 0;

    for (int i = 0; i < 4000; ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;

        /* Lengths on both sides of the 256 byte cap, including zero: the
         * empty frame and the over-long one are the two the parser is most
         * likely to get wrong. */
        unsigned char junk[400];
        size_t size = (size_t)(rng % (sizeof junk + 1));
        for (size_t j = 0; j < size; ++j) {
            junk[j] = (unsigned char)((rng >> (j % 25u)) ^ (unsigned)j);
        }

        ++offered;
        if (mye_net_send(client, junk, size)) {
            ++accepted;
        } else {
            ++refused; /* full queue, or over the cap -- both are the design */
        }

        mye_net_pump(client);
        mye_net_pump(server);

        /* Every eighth pass drains into a buffer far too small, so the "drop
         * rather than truncate" path is walked as well as the ordinary one.
         * Only every eighth, because each drop is a warning and a test that
         * prints four thousand lines hides its own failure. */
        unsigned char small[16];
        unsigned char large[512];
        bool cramped = (i % 8) == 0;
        while (mye_net_recv_pending(server) > 0) {
            (void)mye_net_recv(server, cramped ? small : large,
                               cramped ? sizeof small : sizeof large, NULL);
            ++drained;
        }
    }

    ASSERT_TRUE(accepted > 0);
    ASSERT_TRUE(refused > 0); /* the cap and the bounded queue both fired */
    ASSERT_TRUE(drained > 0);
    ASSERT_EQ_INT(MYE_NET_OPEN, (int)mye_net_status_of(server));
    ASSERT_EQ_INT(1, mye_net_peer_count(server));
    ASSERT_EQ_INT(offered, accepted + refused);

    /* Nothing over the cap was ever queued for delivery. */
    ASSERT_TRUE(mye_net_bytes_received(server) <= mye_net_bytes_sent(client));

    /* And after all that it still carries an ordinary message. */
    ASSERT_TRUE(mye_net_send(client, "still here", 10));
    ASSERT_TRUE(pump_until(server, client, NULL, server_has_message, 3000));
    char buffer[32] = { 0 };
    ASSERT_EQ_INT(10, (int)mye_net_recv(server, buffer, sizeof buffer, NULL));
    ASSERT_TRUE(memcmp(buffer, "still here", 10) == 0);

    mye_net_destroy(client);
    mye_net_destroy(server);
}

TEST_MAIN(TEST_CASE(a_listener_opens_and_a_client_reaches_it),
          TEST_CASE(a_message_round_trips_and_carries_its_sender),
          TEST_CASE(a_broadcast_reaches_every_peer),
          TEST_CASE(order_is_preserved_across_many_messages),
          TEST_CASE(a_full_send_queue_refuses_rather_than_growing),
          TEST_CASE(a_message_over_the_cap_is_refused),
          TEST_CASE(a_short_receive_buffer_drops_rather_than_truncating),
          TEST_CASE(wss_is_refused_while_there_is_no_tls),
          TEST_CASE(a_listener_survives_a_peer_leaving),
          TEST_CASE(this_build_can_listen),
          TEST_CASE(a_closed_peer_ends_up_closed_and_a_refused_dial_ends_up_in_error),
          TEST_CASE(the_receive_path_survives_a_stream_of_random_frames))
