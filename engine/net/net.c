/* Backend-independent half of the transport: queues, defaults, accessors.
 * See plan/12-networking.md. */
#include "net/net_internal.h"

void mye_net_config_defaults(mye_net_config *config)
{
    if (config->max_message_size == 0) {
        config->max_message_size = MYE_NET_DEFAULT_MAX_MESSAGE;
    }
    if (config->recv_queue_capacity <= 0) {
        config->recv_queue_capacity = MYE_NET_DEFAULT_QUEUE;
    }
    if (config->send_queue_capacity <= 0) {
        config->send_queue_capacity = MYE_NET_DEFAULT_QUEUE;
    }
    if (config->max_peers <= 0) {
        config->max_peers = MYE_NET_DEFAULT_PEERS;
    }
}

bool mye_net_queue_init(mye_net_queue *q, mye_allocator a, int capacity,
                        size_t stride)
{
    memset(q, 0, sizeof *q);

    size_t count = (size_t)capacity;
    q->bytes = (unsigned char *)mye_alloc(a, count * stride, MYE_DEFAULT_ALIGN);
    q->sizes = (size_t *)mye_alloc(a, count * sizeof(size_t), _Alignof(size_t));
    q->tags = (uint32_t *)mye_alloc(a, count * sizeof(uint32_t),
                                    _Alignof(uint32_t));
    if (q->bytes == NULL || q->sizes == NULL || q->tags == NULL) {
        mye_net_queue_free(q, a);
        return false;
    }

    q->stride = stride;
    q->capacity = capacity;
    return true;
}

void mye_net_queue_free(mye_net_queue *q, mye_allocator a)
{
    size_t count = (size_t)q->capacity;
    if (q->bytes != NULL) {
        mye_free(a, q->bytes, count * q->stride);
    }
    if (q->sizes != NULL) {
        mye_free(a, q->sizes, count * sizeof(size_t));
    }
    if (q->tags != NULL) {
        mye_free(a, q->tags, count * sizeof(uint32_t));
    }
    memset(q, 0, sizeof *q);
}

bool mye_net_queue_push(mye_net_queue *q, const void *data, size_t size,
                        uint32_t tag)
{
    if (size > q->stride || q->count >= q->capacity) {
        return false;
    }

    int slot = (q->head + q->count) % q->capacity;
    if (size > 0 && data != NULL) {
        memcpy(q->bytes + (size_t)slot * q->stride, data, size);
    }
    q->sizes[slot] = size;
    q->tags[slot] = tag;
    ++q->count;
    return true;
}

unsigned char *mye_net_queue_at(mye_net_queue *q, int logical, size_t *size,
                                uint32_t *tag)
{
    if (logical < 0 || logical >= q->count) {
        return NULL;
    }
    int slot = (q->head + logical) % q->capacity;
    if (size != NULL) {
        *size = q->sizes[slot];
    }
    if (tag != NULL) {
        *tag = q->tags[slot];
    }
    return q->bytes + (size_t)slot * q->stride;
}

void mye_net_queue_set_tag(mye_net_queue *q, int logical, uint32_t tag)
{
    if (logical < 0 || logical >= q->count) {
        return;
    }
    q->tags[(q->head + logical) % q->capacity] = tag;
}

void mye_net_queue_retire(mye_net_queue *q)
{
    while (q->count > 0 && q->tags[q->head] == 0) {
        mye_net_queue_pop(q);
    }
}

void mye_net_queue_pop(mye_net_queue *q)
{
    if (q->count == 0) {
        return;
    }
    q->head = (q->head + 1) % q->capacity;
    --q->count;
}

mye_net_conn *mye_net_conn_alloc(mye_allocator allocator,
                                 const mye_net_config *config, bool is_server)
{
    if (!mye_allocator_valid(allocator)) {
        mye_log_error("net: no allocator");
        return NULL;
    }

    mye_net_conn *conn = MYE_NEW(allocator, mye_net_conn);
    if (conn == NULL) {
        mye_log_error("net: out of memory");
        return NULL;
    }

    conn->allocator = allocator;
    conn->config = config != NULL ? *config : (mye_net_config){ 0 };
    mye_net_config_defaults(&conn->config);
    conn->is_server = is_server;
    conn->status = MYE_NET_CONNECTING;

    size_t stride = conn->config.max_message_size;
    if (!mye_net_queue_init(&conn->in, allocator,
                            conn->config.recv_queue_capacity, stride) ||
        !mye_net_queue_init(&conn->out, allocator,
                            conn->config.send_queue_capacity, stride)) {
        mye_log_error("net: could not allocate %d+%d message slots of %zu bytes",
                      conn->config.recv_queue_capacity,
                      conn->config.send_queue_capacity, stride);
        mye_net_conn_free(conn);
        return NULL;
    }

    return conn;
}

void mye_net_conn_free(mye_net_conn *conn)
{
    if (conn == NULL) {
        return;
    }
    mye_allocator a = conn->allocator;
    mye_net_queue_free(&conn->in, a);
    mye_net_queue_free(&conn->out, a);
    MYE_DELETE(a, conn);
}

/* ------------------------------------------------------------ accessors -- */

mye_net_status mye_net_status_of(const mye_net_conn *conn)
{
    return conn != NULL ? conn->status : MYE_NET_ERROR;
}

size_t mye_net_recv(mye_net_conn *conn, void *buffer, size_t capacity,
                    uint32_t *out_peer)
{
    if (conn == NULL || buffer == NULL) {
        return 0;
    }

    size_t size = 0;
    uint32_t peer = 0;
    const unsigned char *bytes = mye_net_queue_at(&conn->in, 0, &size, &peer);
    if (bytes == NULL) {
        return 0;
    }

    if (size > capacity) {
        /* Dropped rather than truncated: half a message is worse than none,
         * and silently losing the tail is the kind of bug that shows up as
         * corrupt game state days later. */
        mye_log_warn("net: dropping a %zu byte message; the buffer given to "
                     "mye_net_recv holds %zu",
                     size, capacity);
        mye_net_queue_pop(&conn->in);
        return 0;
    }

    memcpy(buffer, bytes, size);
    if (out_peer != NULL) {
        *out_peer = peer;
    }
    mye_net_queue_pop(&conn->in);
    return size;
}

int mye_net_recv_pending(const mye_net_conn *conn)
{
    return conn != NULL ? conn->in.count : 0;
}

int mye_net_send_pending(const mye_net_conn *conn)
{
    return conn != NULL ? conn->out.count : 0;
}

uint64_t mye_net_bytes_received(const mye_net_conn *conn)
{
    return conn != NULL ? conn->bytes_in : 0;
}

uint64_t mye_net_bytes_sent(const mye_net_conn *conn)
{
    return conn != NULL ? conn->bytes_out : 0;
}
