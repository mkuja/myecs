/* N1's done-when: two clients and one relay, all three in this process, and
 * each client sees the other move. Plus the protocol the example teaches --
 * the one-byte kind prefix -- held to the properties that make it worth
 * copying. See examples/07_net/presence.h and plan/12-networking.md.
 *
 * The relay under test is the relay the example runs: presence_relay_tick is
 * the same function --serve calls. A test server written twice would be a
 * test of the second implementation.
 *
 * Every wait is a bounded pump loop with a condition, never a sleep and a
 * hope. Ports are fixed and unique per test; other test binaries share this
 * machine. */
#define _POSIX_C_SOURCE 200809L

#include "mye_test.h"

#include "core/alloc.h"
#include "presence.h"

#include <string.h>
#include <time.h>

#define PORT_MOVEMENT 47951
#define URL_MOVEMENT "ws://127.0.0.1:47951/"
#define PORT_SPOOF 47952
#define URL_SPOOF "ws://127.0.0.1:47952/"
#define PORT_CHAT 47953
#define URL_CHAT "ws://127.0.0.1:47953/"
#define PORT_PING 47954
#define URL_PING "ws://127.0.0.1:47954/"
#define PORT_FUZZ 47955
#define URL_FUZZ "ws://127.0.0.1:47955/"

static void nap(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
    nanosleep(&ts, NULL);
}

/* One full turn of the crank: clients move their bytes, the relay reads and
 * routes, the relay writes, the clients read. Doing it in that order means a
 * message needs one cycle rather than three. */
static void cycle(mye_net_conn *relay, mye_net_conn *a, mye_net_conn *b)
{
    if (a != NULL) mye_net_pump(a);
    if (b != NULL) mye_net_pump(b);
    mye_net_pump(relay);
    presence_relay_tick(relay);
    mye_net_pump(relay);
    if (a != NULL) mye_net_pump(a);
    if (b != NULL) mye_net_pump(b);
}

#define CYCLE_UNTIL(relay_, a_, b_, cond_, max_)                              \
    do {                                                                      \
        for (int mye_i_ = 0; mye_i_ < (max_) && !(cond_); ++mye_i_) {         \
            cycle((relay_), (a_), (b_));                                      \
            nap();                                                            \
        }                                                                     \
    } while (0)

/* Drains `conn` looking for one message of `kind` about `id` (0 = any). Keeps
 * cranking until it turns up or the budget runs out; returns false rather
 * than blocking, so a failure names the message that never arrived. */
static bool wait_for(mye_net_conn *relay, mye_net_conn *a, mye_net_conn *b,
                     mye_net_conn *conn, uint8_t kind, uint32_t id,
                     presence_msg *out, int max_cycles)
{
    for (int i = 0; i < max_cycles; ++i) {
        unsigned char buffer[PRESENCE_MAX_MESSAGE];
        size_t size;
        while ((size = mye_net_recv(conn, buffer, sizeof buffer, NULL)) > 0) {
            presence_msg msg;
            if (!presence_decode(buffer, size, &msg)) {
                continue;
            }
            if (msg.kind == kind && (id == 0 || msg.id == id)) {
                *out = msg;
                return true;
            }
        }
        cycle(relay, a, b);
        nap();
    }
    return false;
}

/* HELLO, then the WELCOME that names this client. */
static uint32_t join(mye_net_conn *relay, mye_net_conn *a, mye_net_conn *b,
                     mye_net_conn *who)
{
    unsigned char buffer[PRESENCE_MAX_MESSAGE];
    size_t n = presence_encode_hello(buffer, sizeof buffer);
    if (n == 0 || !mye_net_send(who, buffer, n)) {
        return 0;
    }
    presence_msg msg;
    if (!wait_for(relay, a, b, who, PRESENCE_WELCOME, 0, &msg, 3000)) {
        return 0;
    }
    return msg.id;
}

static bool send_state(mye_net_conn *conn, uint32_t id, float x, float y)
{
    unsigned char buffer[PRESENCE_MAX_MESSAGE];
    size_t n = presence_encode_state(buffer, sizeof buffer, id, x, y);
    return n > 0 && mye_net_send(conn, buffer, n);
}

/* ---------------------------------------------------------- the milestone -- */

TEST(two_clients_and_a_relay_see_each_others_movement)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_conn *relay = mye_net_listen(heap, PORT_MOVEMENT, NULL);
    mye_net_conn *a = mye_net_connect(heap, URL_MOVEMENT, NULL);
    mye_net_conn *b = mye_net_connect(heap, URL_MOVEMENT, NULL);
    ASSERT_TRUE(relay != NULL && a != NULL && b != NULL);

    CYCLE_UNTIL(relay, a, b, mye_net_peer_count(relay) == 2, 3000);
    ASSERT_EQ_INT(2, mye_net_peer_count(relay));

    uint32_t id_a = join(relay, a, b, a);
    uint32_t id_b = join(relay, a, b, b);
    ASSERT_TRUE(id_a != 0);
    ASSERT_TRUE(id_b != 0);
    ASSERT_TRUE(id_a != id_b); /* the relay hands out distinct identities */

    /* A moves; B sees it. */
    ASSERT_TRUE(send_state(a, id_a, 100.5f, -50.25f));
    presence_msg seen;
    ASSERT_TRUE(wait_for(relay, a, b, b, PRESENCE_STATE, id_a, &seen, 3000));
    ASSERT_NEAR(100.5, seen.x, 1e-6);
    ASSERT_NEAR(-50.25, seen.y, 1e-6);

    /* B moves; A sees it. Two separate assertions on purpose: a relay that
     * forwarded in one direction only would pass the first. */
    ASSERT_TRUE(send_state(b, id_b, 7.0f, 640.0f));
    ASSERT_TRUE(wait_for(relay, a, b, a, PRESENCE_STATE, id_b, &seen, 3000));
    ASSERT_NEAR(7.0, seen.x, 1e-6);
    ASSERT_NEAR(640.0, seen.y, 1e-6);

    /* And movement keeps flowing, not just the first packet. */
    for (int step = 1; step <= 5; ++step) {
        ASSERT_TRUE(send_state(a, id_a, (float)step * 10.0f, 0.0f));
        ASSERT_TRUE(wait_for(relay, a, b, b, PRESENCE_STATE, id_a, &seen,
                             3000));
        ASSERT_NEAR((double)step * 10.0, seen.x, 1e-6);
    }

    mye_net_destroy(a);
    mye_net_destroy(b);
    mye_net_destroy(relay);
}

/* The transport knows who sent a message; the message merely claims to. */
TEST(the_relay_stamps_the_true_sender_over_a_claimed_one)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_conn *relay = mye_net_listen(heap, PORT_SPOOF, NULL);
    mye_net_conn *a = mye_net_connect(heap, URL_SPOOF, NULL);
    mye_net_conn *b = mye_net_connect(heap, URL_SPOOF, NULL);
    ASSERT_TRUE(relay != NULL && a != NULL && b != NULL);

    CYCLE_UNTIL(relay, a, b, mye_net_peer_count(relay) == 2, 3000);
    uint32_t id_a = join(relay, a, b, a);
    uint32_t id_b = join(relay, a, b, b);
    ASSERT_TRUE(id_a != 0 && id_b != 0);

    /* A claims to be B, and teleports "B" somewhere silly. */
    ASSERT_TRUE(send_state(a, id_b, 999.0f, 999.0f));

    presence_msg seen;
    ASSERT_TRUE(wait_for(relay, a, b, b, PRESENCE_STATE, 0, &seen, 3000));
    ASSERT_EQ_INT((int)id_a, (int)seen.id); /* stamped, not believed */

    mye_net_destroy(a);
    mye_net_destroy(b);
    mye_net_destroy(relay);
}

/* The reason the kind byte is first and always present: a chat line and a
 * position are told apart before either is parsed. Route on length instead
 * and the day two kinds are the same length is the day chat starts teleporting
 * players. */
TEST(chat_and_position_stay_distinct_kinds)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_conn *relay = mye_net_listen(heap, PORT_CHAT, NULL);
    mye_net_conn *a = mye_net_connect(heap, URL_CHAT, NULL);
    mye_net_conn *b = mye_net_connect(heap, URL_CHAT, NULL);
    ASSERT_TRUE(relay != NULL && a != NULL && b != NULL);

    CYCLE_UNTIL(relay, a, b, mye_net_peer_count(relay) == 2, 3000);
    uint32_t id_a = join(relay, a, b, a);
    ASSERT_TRUE(id_a != 0);
    ASSERT_TRUE(join(relay, a, b, b) != 0);

    ASSERT_TRUE(send_state(a, id_a, 42.0f, 24.0f));

    unsigned char buffer[PRESENCE_MAX_MESSAGE];
    size_t n = presence_encode_chat(buffer, sizeof buffer, id_a, "hello world");
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_INT(PRESENCE_CHAT, (int)buffer[0]); /* kind first, always */
    ASSERT_TRUE(mye_net_send(a, buffer, n));

    /* B receives both, and each arrives as what it is. Drained in one pass:
     * asking for one kind and then the other would throw away whichever
     * turned up first. */
    presence_msg chat;
    presence_msg state;
    bool got_chat = false;
    bool got_state = false;
    for (int i = 0; i < 3000 && !(got_chat && got_state); ++i) {
        unsigned char in[PRESENCE_MAX_MESSAGE];
        size_t size;
        while ((size = mye_net_recv(b, in, sizeof in, NULL)) > 0) {
            presence_msg msg;
            if (!presence_decode(in, size, &msg) || msg.id != id_a) {
                continue;
            }
            if (msg.kind == PRESENCE_CHAT) {
                chat = msg;
                got_chat = true;
            } else if (msg.kind == PRESENCE_STATE) {
                state = msg;
                got_state = true;
            }
        }
        cycle(relay, a, b);
        nap();
    }
    ASSERT_TRUE(got_chat);
    ASSERT_TRUE(got_state);

    ASSERT_STR_EQ("hello world", chat.text);
    ASSERT_NEAR(0.0, chat.x, 1e-9); /* a chat line has no position */
    ASSERT_NEAR(0.0, chat.y, 1e-9);
    ASSERT_NEAR(42.0, state.x, 1e-6);
    ASSERT_NEAR(24.0, state.y, 1e-6);
    ASSERT_STR_EQ("", state.text); /* a position carries no text */

    mye_net_destroy(a);
    mye_net_destroy(b);
    mye_net_destroy(relay);
}

/* A round trip has to be a round trip: the ping goes back to whoever sent it
 * and to nobody else, or the number on screen is measuring the wrong thing. */
TEST(a_ping_comes_back_only_to_its_sender)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_conn *relay = mye_net_listen(heap, PORT_PING, NULL);
    mye_net_conn *a = mye_net_connect(heap, URL_PING, NULL);
    mye_net_conn *b = mye_net_connect(heap, URL_PING, NULL);
    ASSERT_TRUE(relay != NULL && a != NULL && b != NULL);

    CYCLE_UNTIL(relay, a, b, mye_net_peer_count(relay) == 2, 3000);
    uint32_t id_a = join(relay, a, b, a);
    uint32_t id_b = join(relay, a, b, b);
    ASSERT_TRUE(id_a != 0 && id_b != 0);

    unsigned char buffer[PRESENCE_MAX_MESSAGE];
    size_t n = presence_encode_ping(buffer, sizeof buffer, id_a, 1234.5);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(mye_net_send(a, buffer, n));

    presence_msg back;
    ASSERT_TRUE(wait_for(relay, a, b, a, PRESENCE_PING, id_a, &back, 3000));
    ASSERT_NEAR(1234.5, back.stamp, 1e-12); /* unchanged: the sender's clock */

    /* B is given every chance to receive it, and must not. */
    presence_msg leaked;
    ASSERT_FALSE(wait_for(relay, a, b, b, PRESENCE_PING, 0, &leaked, 200));

    mye_net_destroy(a);
    mye_net_destroy(b);
    mye_net_destroy(relay);
}

/* ------------------------------------------------------------ the format -- */

TEST(every_kind_round_trips_through_its_own_encoder)
{
    unsigned char buffer[PRESENCE_MAX_MESSAGE];
    presence_msg msg;

    size_t n = presence_encode_hello(buffer, sizeof buffer);
    ASSERT_TRUE(n > 0 && presence_decode(buffer, n, &msg));
    ASSERT_EQ_INT(PRESENCE_HELLO, (int)msg.kind);

    n = presence_encode_welcome(buffer, sizeof buffer, 4242u);
    ASSERT_TRUE(n > 0 && presence_decode(buffer, n, &msg));
    ASSERT_EQ_INT(PRESENCE_WELCOME, (int)msg.kind);
    ASSERT_EQ_U64(4242u, msg.id);

    n = presence_encode_state(buffer, sizeof buffer, 7u, -1.5f, 2048.25f);
    ASSERT_TRUE(n > 0 && presence_decode(buffer, n, &msg));
    ASSERT_EQ_INT(PRESENCE_STATE, (int)msg.kind);
    ASSERT_EQ_U64(7u, msg.id);
    ASSERT_NEAR(-1.5, msg.x, 1e-9);
    ASSERT_NEAR(2048.25, msg.y, 1e-9);

    n = presence_encode_ping(buffer, sizeof buffer, 9u, 0.125);
    ASSERT_TRUE(n > 0 && presence_decode(buffer, n, &msg));
    ASSERT_EQ_INT(PRESENCE_PING, (int)msg.kind);
    ASSERT_NEAR(0.125, msg.stamp, 1e-12);

    n = presence_encode_chat(buffer, sizeof buffer, 3u, "gg");
    ASSERT_TRUE(n > 0 && presence_decode(buffer, n, &msg));
    ASSERT_EQ_INT(PRESENCE_CHAT, (int)msg.kind);
    ASSERT_STR_EQ("gg", msg.text);

    /* Little-endian on the wire, spelled out: byte 0 is the kind, then the id
     * least significant byte first, whatever this machine prefers. */
    n = presence_encode_welcome(buffer, sizeof buffer, 0x04030201u);
    ASSERT_EQ_INT(5, (int)n);
    ASSERT_EQ_INT(PRESENCE_WELCOME, (int)buffer[0]);
    ASSERT_EQ_INT(0x01, (int)buffer[1]);
    ASSERT_EQ_INT(0x04, (int)buffer[4]);
}

TEST(the_encoders_refuse_rather_than_overrun)
{
    unsigned char tiny[4];
    ASSERT_EQ_INT(0, (int)presence_encode_welcome(tiny, sizeof tiny, 1u));
    ASSERT_EQ_INT(0, (int)presence_encode_state(tiny, sizeof tiny, 1u, 0, 0));
    ASSERT_EQ_INT(0, (int)presence_encode_ping(tiny, sizeof tiny, 1u, 0.0));

    unsigned char buffer[PRESENCE_MAX_MESSAGE];
    char too_long[PRESENCE_MAX_CHAT + 8];
    memset(too_long, 'x', sizeof too_long);
    too_long[sizeof too_long - 1] = '\0';
    ASSERT_EQ_INT(0, (int)presence_encode_chat(buffer, sizeof buffer, 1u,
                                               too_long));
    ASSERT_EQ_INT(0, (int)presence_encode_chat(buffer, sizeof buffer, 1u, ""));
    ASSERT_EQ_INT(0, (int)presence_encode_chat(buffer, sizeof buffer, 1u,
                                               NULL));
}

/* A public socket receives malformed input as a matter of course, so the
 * decoder's normal answer to nonsense is `false`. Fixed seed: a fuzz test
 * that cannot be re-run is a rumour, not a test. */
TEST(random_bytes_are_refused_and_never_half_understood)
{
    uint32_t rng = 0x5eed1234u;
    int accepted = 0;

    for (int i = 0; i < 50000; ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;

        unsigned char buffer[PRESENCE_MAX_MESSAGE + 16];
        size_t size = (size_t)(rng % (sizeof buffer + 1));
        for (size_t j = 0; j < size; ++j) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            /* Biased towards valid kind bytes, so the body parsers are
             * actually reached instead of every message dying on byte 0. */
            buffer[j] = (j == 0) ? (unsigned char)(rng % 8u)
                                 : (unsigned char)(rng & 0xffu);
        }

        presence_msg msg;
        memset(&msg, 0xcd, sizeof msg);
        if (!presence_decode(buffer, size, &msg)) {
            continue;
        }
        ++accepted;

        /* Whatever it accepted, it understood completely: re-encoding the
         * decoded message reproduces the bytes exactly. A decoder that
         * silently ignored a trailing field would fail here. */
        unsigned char again[PRESENCE_MAX_MESSAGE];
        size_t n = 0;
        switch (msg.kind) {
        case PRESENCE_HELLO:
            n = presence_encode_hello(again, sizeof again);
            break;
        case PRESENCE_WELCOME:
            n = presence_encode_welcome(again, sizeof again, msg.id);
            break;
        case PRESENCE_STATE:
            n = presence_encode_state(again, sizeof again, msg.id, msg.x,
                                      msg.y);
            break;
        case PRESENCE_PING:
            n = presence_encode_ping(again, sizeof again, msg.id, msg.stamp);
            break;
        case PRESENCE_CHAT:
            n = presence_encode_chat(again, sizeof again, msg.id, msg.text);
            break;
        default:
            MYE_FAIL_("decoded an unknown kind %d", (int)msg.kind);
        }
        ASSERT_EQ_INT((int)size, (int)n);
        ASSERT_TRUE(memcmp(again, buffer, n) == 0);
    }

    /* If nothing was ever accepted the property above proved nothing, and the
     * generator -- not the decoder -- is what needs fixing. */
    ASSERT_TRUE(accepted > 100);
}

/* The same nonsense, but arriving over a real socket at the real relay: the
 * relay must survive it, keep its peers, and still be relaying afterwards. */
TEST(the_relay_survives_a_stream_of_malformed_frames)
{
    mye_allocator heap = mye_heap_allocator();
    mye_net_conn *relay = mye_net_listen(heap, PORT_FUZZ, NULL);
    mye_net_conn *a = mye_net_connect(heap, URL_FUZZ, NULL);
    mye_net_conn *b = mye_net_connect(heap, URL_FUZZ, NULL);
    ASSERT_TRUE(relay != NULL && a != NULL && b != NULL);

    CYCLE_UNTIL(relay, a, b, mye_net_peer_count(relay) == 2, 3000);
    uint32_t id_a = join(relay, a, b, a);
    ASSERT_TRUE(id_a != 0);
    ASSERT_TRUE(join(relay, a, b, b) != 0);

    uint32_t rng = 0xfa11u;
    int sent = 0;
    for (int i = 0; i < 400; ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;

        unsigned char junk[PRESENCE_MAX_MESSAGE];
        size_t size = (size_t)(rng % (sizeof junk + 1));
        for (size_t j = 0; j < size; ++j) {
            junk[j] = (unsigned char)((rng >> (j % 24u)) & 0xffu);
        }
        if (mye_net_send(a, junk, size)) {
            ++sent;
        }
        /* Drain both clients so a queue full of forwarded nonsense does not
         * stall the crank. */
        unsigned char drop[PRESENCE_MAX_MESSAGE];
        while (mye_net_recv(a, drop, sizeof drop, NULL) > 0) { }
        while (mye_net_recv(b, drop, sizeof drop, NULL) > 0) { }
        cycle(relay, a, b);
    }
    ASSERT_TRUE(sent > 100);

    /* Still alive, still relaying. */
    ASSERT_EQ_INT(MYE_NET_OPEN, (int)mye_net_status_of(relay));
    ASSERT_EQ_INT(2, mye_net_peer_count(relay));

    ASSERT_TRUE(send_state(a, id_a, 3.5f, 4.5f));
    presence_msg seen;
    ASSERT_TRUE(wait_for(relay, a, b, b, PRESENCE_STATE, id_a, &seen, 3000));
    ASSERT_NEAR(3.5, seen.x, 1e-6);

    /* Clean shutdown after all that is half the point of running this under
     * ASan: a receive path that scribbled would be caught here. */
    mye_net_destroy(a);
    mye_net_destroy(b);
    mye_net_destroy(relay);
}

TEST_MAIN(TEST_CASE(two_clients_and_a_relay_see_each_others_movement),
          TEST_CASE(the_relay_stamps_the_true_sender_over_a_claimed_one),
          TEST_CASE(chat_and_position_stay_distinct_kinds),
          TEST_CASE(a_ping_comes_back_only_to_its_sender),
          TEST_CASE(every_kind_round_trips_through_its_own_encoder),
          TEST_CASE(the_encoders_refuse_rather_than_overrun),
          TEST_CASE(random_bytes_are_refused_and_never_half_understood),
          TEST_CASE(the_relay_survives_a_stream_of_malformed_frames))
