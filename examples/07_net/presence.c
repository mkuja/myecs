/* Encoding, decoding and relaying for the presence example. See presence.h --
 * this is a pattern to copy, not engine API. */
#include "presence.h"

#include "core/log.h"

#include <string.h>

/* Body sizes, kind byte and id included. */
#define PRESENCE_SIZE_HELLO 1
#define PRESENCE_SIZE_WELCOME 5
#define PRESENCE_SIZE_STATE 13
#define PRESENCE_SIZE_PING 13
#define PRESENCE_SIZE_CHAT_MIN 6 /* kind + id + at least one character */

/* Little-endian by hand. memcpy of the struct would put this machine's byte
 * order and this compiler's padding on the wire, and the other end may be a
 * browser built by a different compiler entirely. */
static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u64(unsigned char *p, uint64_t v)
{
    put_u32(p, (uint32_t)(v & 0xffffffffu));
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t get_u64(const unsigned char *p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

/* IEEE-754 bit patterns, which is what every target here actually uses. The
 * memcpy is the standard-blessed way to look at a float's bytes; a pointer
 * cast would be an aliasing violation the optimiser is entitled to punish. */
static void put_f32(unsigned char *p, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    put_u32(p, bits);
}

static float get_f32(const unsigned char *p)
{
    uint32_t bits = get_u32(p);
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

static void put_f64(unsigned char *p, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    put_u64(p, bits);
}

static double get_f64(const unsigned char *p)
{
    uint64_t bits = get_u64(p);
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

/* --------------------------------------------------------------- encode -- */

static unsigned char *header(void *buffer, size_t capacity, size_t needed,
                             uint8_t kind, uint32_t id)
{
    if (buffer == NULL || capacity < needed) {
        return NULL;
    }
    unsigned char *p = (unsigned char *)buffer;
    p[0] = kind;
    if (needed > 1) {
        put_u32(p + 1, id);
    }
    return p;
}

size_t presence_encode_hello(void *buffer, size_t capacity)
{
    unsigned char *p = header(buffer, capacity, PRESENCE_SIZE_HELLO,
                              PRESENCE_HELLO, 0);
    return p != NULL ? PRESENCE_SIZE_HELLO : 0;
}

size_t presence_encode_welcome(void *buffer, size_t capacity, uint32_t id)
{
    unsigned char *p = header(buffer, capacity, PRESENCE_SIZE_WELCOME,
                              PRESENCE_WELCOME, id);
    return p != NULL ? PRESENCE_SIZE_WELCOME : 0;
}

size_t presence_encode_state(void *buffer, size_t capacity, uint32_t id,
                             float x, float y)
{
    unsigned char *p = header(buffer, capacity, PRESENCE_SIZE_STATE,
                              PRESENCE_STATE, id);
    if (p == NULL) {
        return 0;
    }
    put_f32(p + 5, x);
    put_f32(p + 9, y);
    return PRESENCE_SIZE_STATE;
}

size_t presence_encode_ping(void *buffer, size_t capacity, uint32_t id,
                            double stamp)
{
    unsigned char *p = header(buffer, capacity, PRESENCE_SIZE_PING,
                              PRESENCE_PING, id);
    if (p == NULL) {
        return 0;
    }
    put_f64(p + 5, stamp);
    return PRESENCE_SIZE_PING;
}

size_t presence_encode_chat(void *buffer, size_t capacity, uint32_t id,
                            const char *text)
{
    if (text == NULL) {
        return 0;
    }
    size_t length = strlen(text);
    if (length == 0 || length > PRESENCE_MAX_CHAT) {
        return 0;
    }

    size_t needed = 5 + length;
    unsigned char *p = header(buffer, capacity, needed, PRESENCE_CHAT, id);
    if (p == NULL) {
        return 0;
    }
    /* No NUL on the wire: WebSocket framing already says where the message
     * ends, so a terminator would only be a second, disagreeing opinion. */
    memcpy(p + 5, text, length);
    return needed;
}

/* --------------------------------------------------------------- decode -- */

static bool printable_ascii(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < 0x20u || p[i] > 0x7eu) {
            return false;
        }
    }
    return true;
}

bool presence_decode(const void *data, size_t size, presence_msg *out)
{
    if (data == NULL || out == NULL || size < 1) {
        return false;
    }
    const unsigned char *p = (const unsigned char *)data;

    presence_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.kind = p[0];

    switch (msg.kind) {
    case PRESENCE_HELLO:
        if (size != PRESENCE_SIZE_HELLO) {
            return false;
        }
        break;

    case PRESENCE_WELCOME:
        if (size != PRESENCE_SIZE_WELCOME) {
            return false;
        }
        msg.id = get_u32(p + 1);
        break;

    case PRESENCE_STATE:
        if (size != PRESENCE_SIZE_STATE) {
            return false;
        }
        msg.id = get_u32(p + 1);
        msg.x = get_f32(p + 5);
        msg.y = get_f32(p + 9);
        break;

    case PRESENCE_PING:
        if (size != PRESENCE_SIZE_PING) {
            return false;
        }
        msg.id = get_u32(p + 1);
        msg.stamp = get_f64(p + 5);
        break;

    case PRESENCE_CHAT: {
        if (size < PRESENCE_SIZE_CHAT_MIN || size > 5 + PRESENCE_MAX_CHAT) {
            return false;
        }
        size_t length = size - 5;
        /* Refusing anything but printable ASCII keeps control characters and
         * half-decoded UTF-8 out of a string the game is about to draw. A
         * real game would take UTF-8 and validate it; the point is that
         * SOMETHING validates. */
        if (!printable_ascii(p + 5, length)) {
            return false;
        }
        msg.id = get_u32(p + 1);
        memcpy(msg.text, p + 5, length);
        msg.text[length] = '\0';
        break;
    }

    default:
        return false; /* an unknown kind is skipped, never guessed at */
    }

    *out = msg;
    return true;
}

/* ---------------------------------------------------------------- relay -- */

void presence_relay_tick(mye_net_conn *server)
{
    if (server == NULL) {
        return;
    }

    unsigned char buffer[PRESENCE_MAX_MESSAGE];
    uint32_t peer = 0;
    size_t size;

    while ((size = mye_net_recv(server, buffer, sizeof buffer, &peer)) > 0) {
        presence_msg msg;
        if (!presence_decode(buffer, size, &msg)) {
            mye_log_warn("relay: dropping %zu malformed bytes from peer %u",
                         size, peer);
            continue;
        }

        switch (msg.kind) {
        case PRESENCE_HELLO: {
            unsigned char welcome[PRESENCE_MAX_MESSAGE];
            size_t n = presence_encode_welcome(welcome, sizeof welcome, peer);
            mye_net_send_to(server, peer, welcome, n);
            mye_log_info("relay: peer %u said hello (%d connected)", peer,
                         mye_net_peer_count(server));
            break;
        }

        case PRESENCE_PING:
            /* Straight back, unchanged and to the sender alone: the client
             * subtracts its own stamp, so nothing depends on the two machines
             * agreeing about what time it is. */
            mye_net_send_to(server, peer, buffer, size);
            break;

        case PRESENCE_STATE:
        case PRESENCE_CHAT:
            /* The transport knows who sent this; the message merely claims
             * to. Overwrite the claim. */
            put_u32(buffer + 1, peer);
            mye_net_send(server, buffer, size);
            break;

        default:
            /* Decoded but not the relay's business (a WELCOME arriving from a
             * client, say). Dropped rather than forwarded. */
            break;
        }
    }
}
