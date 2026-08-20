/* WebSocket transport. See plan/12-networking.md.
 *
 * One API, two backends. On desktop it is libwebsockets; in the browser it
 * is the browser's own WebSocket, because a web build cannot open sockets
 * and WebSocket is the only game-shaped channel it is offered.
 *
 *   mye_net_conn *c = mye_net_connect(alloc, "ws://localhost:9010", NULL);
 *   while (running) {
 *       mye_net_pump(c);                      // once per frame, never blocks
 *       unsigned char buf[1024];
 *       size_t n;
 *       while ((n = mye_net_recv(c, buf, sizeof buf, NULL)) > 0) { ... }
 *       mye_net_send(c, "hello", 5);
 *   }
 *   mye_net_destroy(c);
 *
 * Two things to know before building on it:
 *
 * - Delivery is reliable and ordered, and that is not a choice we made: a
 *   browser offers nothing else without WebRTC or WebTransport. A lost
 *   packet stalls everything behind it. Fine for co-op, turn-based, chat and
 *   lobbies; wrong for twitch action at high tick rates.
 * - A browser can only ever be a client. mye_net_listen fails on the web
 *   build, so servers are native builds.
 *
 * Nothing here interprets a message. Payloads are bytes; what they mean is
 * gameplay's business.
 */
#ifndef MYE_NET_NET_H
#define MYE_NET_NET_H

#include "core/alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mye_net_conn mye_net_conn;

typedef enum mye_net_status {
    MYE_NET_IDLE = 0,
    MYE_NET_CONNECTING, /* dialling, or a listener with no peers yet */
    MYE_NET_OPEN,       /* client: connected. server: listening. */
    MYE_NET_CLOSED,     /* finished cleanly */
    MYE_NET_ERROR,      /* dial failed, or the peer vanished */
} mye_net_status;

/* Zero-initialise and set only what you care about; every field has a
 * default. Queues are bounded on purpose: a sender that outruns the socket
 * gets a refused send, not unbounded memory (the audio queue makes the same
 * trade). */
typedef struct mye_net_config {
    size_t max_message_size;   /* default 64 KiB; larger arrivals are dropped */
    int recv_queue_capacity;   /* default 64 messages */
    int send_queue_capacity;   /* default 64 messages */
    int max_peers;             /* listeners only; default 32 */
} mye_net_config;

/* Both return immediately -- nothing here ever blocks. A connection starts
 * CONNECTING; poll mye_net_status_of. NULL means the arguments or the
 * allocator were bad, and is already logged.
 *
 * `url` is ws://host:port/path. wss:// is not built yet (no TLS -- N2). */
mye_net_conn *mye_net_connect(mye_allocator allocator, const char *url,
                              const mye_net_config *config);

/* Native only: returns NULL in a web build, where a page cannot listen. */
mye_net_conn *mye_net_listen(mye_allocator allocator, uint16_t port,
                             const mye_net_config *config);

void mye_net_destroy(mye_net_conn *conn);

/* Moves bytes: fills the receive queue, drains the send queue, advances
 * connect and close handshakes. Call once per frame. Never blocks. */
void mye_net_pump(mye_net_conn *conn);

mye_net_status mye_net_status_of(const mye_net_conn *conn);

/* Queues a message. False when the send queue is full, the message exceeds
 * max_message_size, or the connection is not open. On a listener this
 * broadcasts to every peer. */
bool mye_net_send(mye_net_conn *conn, const void *data, size_t size);

/* Listener-side: one peer, identified by the id that came back from
 * mye_net_recv. */
bool mye_net_send_to(mye_net_conn *conn, uint32_t peer, const void *data,
                     size_t size);

/* Copies the oldest message into `buffer` and returns its size, or 0 when
 * the queue is empty. `out_peer` (optional) receives the sender's id, which
 * is 0 on a client. A message longer than `capacity` is dropped rather than
 * truncated, so a short buffer loses data loudly instead of silently
 * corrupting it. */
size_t mye_net_recv(mye_net_conn *conn, void *buffer, size_t capacity,
                    uint32_t *out_peer);

/* Observability, for a debug overlay or a test. */
int mye_net_peer_count(const mye_net_conn *conn);
int mye_net_recv_pending(const mye_net_conn *conn);
int mye_net_send_pending(const mye_net_conn *conn);
uint64_t mye_net_bytes_received(const mye_net_conn *conn);
uint64_t mye_net_bytes_sent(const mye_net_conn *conn);

/* True when this build can act as a server, so a game can say so rather
 * than discovering it from a NULL. */
bool mye_net_can_listen(void);

/* --------------------------------------------------------------- reconnect -- */

/* Reconnect timing, as a state machine with no socket in it.
 *
 * The engine never reconnects on your behalf: a game that lost its server
 * may want a lobby screen, a save, or a clean exit, and an engine that
 * quietly redialled would have made that choice for it. What the engine can
 * usefully own is the arithmetic nobody enjoys re-deriving -- wait a little,
 * then longer, then give up -- so this is that arithmetic and nothing else.
 *
 *   mye_net_backoff back;
 *   mye_net_backoff_init(&back, NULL);
 *   ...
 *   if (mye_net_status_of(conn) == MYE_NET_ERROR) {
 *       mye_net_backoff_failed(&back);       // start (or lengthen) the wait
 *   }
 *   if (mye_net_backoff_ready(&back, dt)) {  // true once, when it is time
 *       mye_net_destroy(conn);
 *       conn = mye_net_connect(alloc, url, NULL);
 *   }
 *   if (mye_net_status_of(conn) == MYE_NET_OPEN) {
 *       mye_net_backoff_connected(&back);    // back to the first delay
 *   }
 *
 * Pure: no clock, no allocator, no I/O. dt comes from the caller, which is
 * what makes it testable in a loop with no sockets and no sleeping. */
typedef struct mye_net_backoff_config {
    double first_delay; /* seconds before the first retry; default 0.5 */
    double max_delay;   /* ceiling on the wait; default 8 */
    double factor;      /* multiplier per failure; default 2 */
    int max_attempts;   /* 0 = keep trying forever */
    /* Fraction of the delay to spread the retry over, 0..1; default 0. The
     * wait then lands somewhere in [(1 - jitter) * delay, delay]. With many
     * clients reconnecting to one server, an unjittered ladder makes them all
     * dial in the same instant, again, at every rung. The spread is drawn
     * from `seed` below, so it is scattered but not unpredictable -- a test
     * can still assert exact timings. */
    double jitter;
    uint32_t seed; /* 0 means 1; the sequence is fixed by this value */
} mye_net_backoff_config;

typedef struct mye_net_backoff {
    mye_net_backoff_config config;
    int attempts;     /* retries handed out so far */
    double delay;     /* the current rung of the ladder */
    double remaining; /* seconds left to wait; only meaningful while waiting */
    uint32_t rng;
    bool waiting;
} mye_net_backoff;

/* `config` may be NULL, and any zero field takes its default. */
void mye_net_backoff_init(mye_net_backoff *backoff,
                          const mye_net_backoff_config *config);

/* The connection failed or dropped: begin waiting, one rung further up the
 * ladder than last time. Calling it again while already waiting does not
 * shorten or lengthen the current wait -- a status polled every frame must
 * not restart the timer sixty times a second. */
void mye_net_backoff_failed(mye_net_backoff *backoff);

/* The connection is up again: back to the bottom of the ladder. */
void mye_net_backoff_connected(mye_net_backoff *backoff);

/* Advances the wait by `dt` and returns true exactly once, on the frame the
 * next attempt is due. False while waiting, while not waiting at all, and
 * forever once max_attempts is used up. */
bool mye_net_backoff_ready(mye_net_backoff *backoff, double dt);

/* Seconds until the next attempt, for a "reconnecting in 3s" line. Zero when
 * nothing is pending. */
double mye_net_backoff_remaining(const mye_net_backoff *backoff);

/* True once max_attempts retries have been handed out: the game decides what
 * that means -- a menu, a message, an exit. */
bool mye_net_backoff_exhausted(const mye_net_backoff *backoff);

#endif /* MYE_NET_NET_H */
