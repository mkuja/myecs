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

#endif /* MYE_NET_NET_H */
