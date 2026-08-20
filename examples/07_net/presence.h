/* The presence protocol: one byte of kind, then a body. See
 * plan/12-networking.md ("Protocol: deliberately not the engine's business").
 *
 * THIS IS A PATTERN TO COPY, NOT AN ENGINE API. The engine ships no
 * serialization opinion -- a message is bytes -- so every game invents this
 * layer, and this is what the smallest reasonable version looks like:
 *
 *   [0]     message kind, one byte
 *   [1..4]  who it is about, a little-endian uint32
 *   [5..]   the body, whatever that kind means
 *
 * Three things are worth stealing from it:
 *
 * - The kind byte comes first and is always present, so a receiver can route
 *   a message before it understands it, and an unknown kind is skipped rather
 *   than misread. Without it, a chat line and a position would be told apart
 *   by length, which works right up until they are the same length.
 * - Every integer is written byte by byte, least significant first. Writing
 *   the struct straight out of memory would tie the wire format to one
 *   compiler's padding and one machine's byte order, and the web build is
 *   already a different compiler.
 * - Decoding validates and returns false. Malformed input is the normal case
 *   on a socket anyone can connect to, not an exceptional one, so it is a
 *   return value rather than an assert -- see the fuzz test.
 *
 * It lives in its own file, compiled as a small library, so the integration
 * test drives the same relay the example runs. A test server that is a
 * different implementation from the real one tests the wrong thing.
 */
#ifndef MYE_EXAMPLE_PRESENCE_H
#define MYE_EXAMPLE_PRESENCE_H

#include "net/net.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum presence_kind {
    PRESENCE_HELLO = 1,   /* client -> relay: I am here, who am I?     */
    PRESENCE_WELCOME = 2, /* relay -> one client: you are id N         */
    PRESENCE_STATE = 3,   /* client -> relay -> everyone: where I am   */
    PRESENCE_CHAT = 4,    /* client -> relay -> everyone: a line       */
    PRESENCE_PING = 5,    /* client -> relay -> back to that client    */
};

#define PRESENCE_MAX_CHAT 48   /* characters in one chat line, NUL aside */
#define PRESENCE_MAX_MESSAGE 64 /* the longest any of the above encodes to */
#define PRESENCE_PORT 9010
#define PRESENCE_URL "ws://localhost:9010/"

/* How often a client sends its position. Below the frame rate on purpose:
 * remote entities therefore arrive at a rate the display does not share,
 * which is exactly the gap MyeInterpolate exists to hide. */
#define PRESENCE_SEND_HZ 15

typedef struct presence_msg {
    uint8_t kind;
    uint32_t id;  /* the client this is about; 0 before a WELCOME arrives */

    float x, y;   /* STATE */
    double stamp; /* PING: the sender's clock when it left */
    char text[PRESENCE_MAX_CHAT + 1]; /* CHAT, always NUL-terminated */
} presence_msg;

/* Each returns the number of bytes written, or 0 if the buffer is too small
 * (or the text too long). Never partially writes. */
size_t presence_encode_hello(void *buffer, size_t capacity);
size_t presence_encode_welcome(void *buffer, size_t capacity, uint32_t id);
size_t presence_encode_state(void *buffer, size_t capacity, uint32_t id,
                             float x, float y);
size_t presence_encode_chat(void *buffer, size_t capacity, uint32_t id,
                            const char *text);
size_t presence_encode_ping(void *buffer, size_t capacity, uint32_t id,
                            double stamp);

/* False for anything not fully understood: an unknown kind, a truncated or
 * over-long body, a chat line carrying anything but printable ASCII. `out` is
 * only written on success. */
bool presence_decode(const void *data, size_t size, presence_msg *out);

/* The relay's entire job, one call per pump:
 *
 * - HELLO gets a WELCOME carrying the peer id the transport assigned, sent to
 *   that peer alone.
 * - PING goes straight back to its sender, so the number the client displays
 *   is a real round trip and not an estimate.
 * - STATE and CHAT are re-stamped with the sender's true peer id and
 *   broadcast to everyone, the sender included. Re-stamping is what stops a
 *   client claiming to be someone else; broadcasting back to the sender means
 *   every client sees chat in the same order, and costs one message a client
 *   already knows how to ignore.
 *
 * Anything else is dropped with a warning. The relay never sleeps, never
 * blocks, and holds no state beyond the connection: pumping is the caller's
 * job, whether that is the --serve loop or a test. */
void presence_relay_tick(mye_net_conn *server);

#endif /* MYE_EXAMPLE_PRESENCE_H */
