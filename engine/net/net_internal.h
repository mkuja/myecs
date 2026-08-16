/* Shared between the backends: the queues, the counters, the connection
 * struct. Not a public header. */
#ifndef MYE_NET_NET_INTERNAL_H
#define MYE_NET_NET_INTERNAL_H

#include "core/log.h"
#include "net/net.h"

#include <string.h>

/* Defaults sized so a connection costs about a megabyte. Both queues are
 * slot rings of max_message_size, so raising the message cap raises memory
 * by capacity times the difference -- deliberate, and the reason the default
 * cap is modest rather than generous. */
#define MYE_NET_DEFAULT_MAX_MESSAGE (16u * 1024u)
#define MYE_NET_DEFAULT_QUEUE 32
#define MYE_NET_DEFAULT_PEERS 32

/* The WebSocket subprotocol both ends name. It has to be explicit on the
 * browser side too: a page that opens a socket without one gives the server
 * nothing to match, and libwebsockets then leaves the handshake unanswered
 * -- which looks exactly like a hung connection. */
#define MYE_NET_SUBPROTOCOL "mye"

/* A ring of fixed-size slots rather than a malloc per message: bounded
 * memory, no allocator traffic on the hot path, and a full queue refuses
 * instead of growing.
 *
 * `tag` means different things per direction, which is why it is not called
 * `peer`: on the receive queue it is the sender's id, and on the send queue
 * it is a bitmask of the peers that still have to be written to. One shared
 * send queue with a recipient mask keeps a listener's memory flat instead of
 * multiplying it by the peer count. */
typedef struct mye_net_queue {
    unsigned char *bytes; /* capacity * stride */
    size_t *sizes;
    uint32_t *tags;
    size_t stride;
    int capacity;
    int head;
    int count;
} mye_net_queue;

struct mye_net_conn {
    mye_allocator allocator;
    mye_net_config config;
    mye_net_status status;
    bool is_server;

    mye_net_queue in;
    mye_net_queue out;

    uint64_t bytes_in;
    uint64_t bytes_out;

    void *backend;
};

bool mye_net_queue_init(mye_net_queue *q, mye_allocator a, int capacity,
                        size_t stride);
void mye_net_queue_free(mye_net_queue *q, mye_allocator a);

/* False when full, or when the message does not fit a slot. */
bool mye_net_queue_push(mye_net_queue *q, const void *data, size_t size,
                        uint32_t tag);

/* Logical index 0 is the oldest. Returns NULL past the end. Borrowing rather
 * than copying lets a backend hand the bytes straight to the socket and only
 * drop them once the write has actually happened. */
unsigned char *mye_net_queue_at(mye_net_queue *q, int logical, size_t *size,
                                uint32_t *tag);
void mye_net_queue_set_tag(mye_net_queue *q, int logical, uint32_t tag);
void mye_net_queue_pop(mye_net_queue *q);

/* Drops finished messages from the front. A message is finished when its
 * recipient mask is empty; only the head can be dropped, so a slow peer
 * holds the queue and eventually refuses new sends -- backpressure. */
void mye_net_queue_retire(mye_net_queue *q);

void mye_net_config_defaults(mye_net_config *config);

mye_net_conn *mye_net_conn_alloc(mye_allocator allocator,
                                 const mye_net_config *config, bool is_server);
void mye_net_conn_free(mye_net_conn *conn);

#endif /* MYE_NET_NET_INTERNAL_H */
