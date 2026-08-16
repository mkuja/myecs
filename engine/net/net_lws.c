/* Native WebSocket backend: libwebsockets. See plan/12-networking.md.
 *
 * libwebsockets is callback-driven and insists that writes happen inside a
 * WRITEABLE callback, so nothing here writes directly. Sending queues the
 * bytes and asks for a writeable callback; the callback drains the queue.
 * mye_net_pump then only has to service the context, which is what keeps
 * the whole thing single-threaded and lock-free.
 */
/* poll() is POSIX, and -std=c11 hides it. This is the platform backend, so
 * this is where a feature-test macro belongs. */
#define _POSIX_C_SOURCE 200809L

#include "net/net_internal.h"

#include <libwebsockets.h>

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct peer_slot {
    struct lws *wsi;
    uint32_t id;
    bool used;
} peer_slot;

/* Per-connection state libwebsockets allocates for us. Incoming messages can
 * arrive in fragments, so each connection reassembles into its own buffer
 * before anything is queued -- a game must never see half a message. */
typedef struct session {
    int slot;              /* index into backend->peers; -1 for a client */
    unsigned char *assembly;
    size_t assembled;
} session;

#define MYE_NET_MAX_FDS 64

typedef struct lws_backend {
    struct lws_context *ctx;

    /* The descriptors lws has asked us to watch. Kept here rather than
     * letting lws sleep on them itself: mye_net_pump has to return within a
     * frame, so the engine owns the wait and makes it zero-length. */
    struct pollfd fds[MYE_NET_MAX_FDS];
    int fd_count;

    struct lws *client_wsi;
    mye_net_conn *conn;

    peer_slot *peers;
    int peer_capacity;
    int peer_count;
    uint32_t next_peer_id;

    bool destroying;
} lws_backend;

static int find_fd(const lws_backend *b, int fd)
{
    for (int i = 0; i < b->fd_count; ++i) {
        if (b->fds[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

static void add_fd(lws_backend *b, int fd, short events)
{
    if (find_fd(b, fd) >= 0 || b->fd_count >= MYE_NET_MAX_FDS) {
        if (b->fd_count >= MYE_NET_MAX_FDS) {
            mye_log_warn("net: watching more than %d sockets is not supported",
                         MYE_NET_MAX_FDS);
        }
        return;
    }
    b->fds[b->fd_count].fd = fd;
    b->fds[b->fd_count].events = events;
    b->fds[b->fd_count].revents = 0;
    ++b->fd_count;
}

static void remove_fd(lws_backend *b, int fd)
{
    int i = find_fd(b, fd);
    if (i < 0) {
        return;
    }
    b->fds[i] = b->fds[b->fd_count - 1];
    --b->fd_count;
}

static lws_backend *backend_of(struct lws *wsi)
{
    return (lws_backend *)lws_context_user(lws_get_context(wsi));
}

/* ------------------------------------------------------------- helpers -- */

static int peer_slot_of_id(const lws_backend *b, uint32_t id)
{
    for (int i = 0; i < b->peer_capacity; ++i) {
        if (b->peers[i].used && b->peers[i].id == id) {
            return i;
        }
    }
    return -1;
}

static uint32_t connected_mask(const lws_backend *b)
{
    uint32_t mask = 0;
    for (int i = 0; i < b->peer_capacity; ++i) {
        if (b->peers[i].used) {
            mask |= (uint32_t)1u << i;
        }
    }
    return mask;
}

/* Nudges everyone still owed a message. Cheap: libwebsockets coalesces. */
static void request_writes(lws_backend *b)
{
    mye_net_conn *conn = b->conn;
    if (!conn->is_server) {
        if (b->client_wsi != NULL && conn->out.count > 0) {
            lws_callback_on_writable(b->client_wsi);
        }
        return;
    }

    uint32_t wanted = 0;
    for (int i = 0; i < conn->out.count; ++i) {
        uint32_t mask = 0;
        (void)mye_net_queue_at(&conn->out, i, NULL, &mask);
        wanted |= mask;
    }
    for (int i = 0; i < b->peer_capacity; ++i) {
        if (b->peers[i].used && (wanted & ((uint32_t)1u << i)) != 0) {
            lws_callback_on_writable(b->peers[i].wsi);
        }
    }
}

/* Writes the oldest message this peer still owes. Returns true if it wrote
 * one, so the caller can ask for another callback. */
static bool write_pending_for(lws_backend *b, struct lws *wsi, int slot)
{
    mye_net_conn *conn = b->conn;
    uint32_t bit = slot < 0 ? 1u : ((uint32_t)1u << slot);

    for (int i = 0; i < conn->out.count; ++i) {
        size_t size = 0;
        uint32_t mask = 0;
        unsigned char *bytes = mye_net_queue_at(&conn->out, i, &size, &mask);
        if (bytes == NULL || (mask & bit) == 0) {
            continue;
        }

        /* libwebsockets writes in place and needs LWS_PRE bytes of headroom
         * in front of the payload, so the slot carries that padding and the
         * payload starts after it. */
        int written = lws_write(wsi, bytes + LWS_PRE, size, LWS_WRITE_BINARY);
        if (written < 0) {
            mye_log_warn("net: write failed; dropping the connection");
            return false;
        }

        mye_net_queue_set_tag(&conn->out, i, mask & ~bit);
        conn->bytes_out += size;
        mye_net_queue_retire(&conn->out);
        return true;
    }
    return false;
}

static void deliver(lws_backend *b, session *s, void *in, size_t len,
                    bool final)
{
    mye_net_conn *conn = b->conn;
    size_t cap = conn->config.max_message_size;

    if (s->assembled + len > cap) {
        mye_log_warn("net: dropping a message over max_message_size (%zu)", cap);
        s->assembled = cap + 1; /* poison: drop the rest of this message */
    } else if (s->assembly != NULL) {
        memcpy(s->assembly + s->assembled, in, len);
        s->assembled += len;
    }

    if (!final) {
        return;
    }

    if (s->assembled <= cap) {
        uint32_t peer = s->slot >= 0 ? b->peers[s->slot].id : 0;
        if (!mye_net_queue_push(&conn->in, s->assembly, s->assembled, peer)) {
            mye_log_warn("net: receive queue full; dropped a message");
        } else {
            conn->bytes_in += s->assembled;
        }
    }
    s->assembled = 0;
}

/* ------------------------------------------------------------ callback -- */

static int protocol_cb(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    lws_backend *b = backend_of(wsi);
    if (b == NULL) {
        return 0;
    }
    mye_net_conn *conn = b->conn;
    session *s = (session *)user;

    switch (reason) {
    /* --- client --- */
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        conn->status = MYE_NET_OPEN;
        s->slot = -1;
        s->assembled = 0;
        s->assembly = (unsigned char *)mye_alloc(
            conn->allocator, conn->config.max_message_size, MYE_DEFAULT_ALIGN);
        mye_log_info("net: connected");
        return 0;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        deliver(b, s, in, len, lws_is_final_fragment(wsi) != 0);
        return 0;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (write_pending_for(b, wsi, -1) && conn->out.count > 0) {
            lws_callback_on_writable(wsi);
        }
        return 0;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        conn->status = MYE_NET_ERROR;
        b->client_wsi = NULL;
        mye_log_warn("net: connection failed: %s",
                     in != NULL ? (const char *)in : "no reason given");
        return 0;

    case LWS_CALLBACK_CLIENT_CLOSED:
        if (conn->status != MYE_NET_ERROR) {
            conn->status = MYE_NET_CLOSED;
        }
        b->client_wsi = NULL;
        if (s->assembly != NULL) {
            mye_free(conn->allocator, s->assembly,
                     conn->config.max_message_size);
            s->assembly = NULL;
        }
        return 0;

    /* --- server --- */
    case LWS_CALLBACK_ESTABLISHED: {
        int slot = -1;
        for (int i = 0; i < b->peer_capacity; ++i) {
            if (!b->peers[i].used) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            mye_log_warn("net: refusing a peer; max_peers (%d) reached",
                         b->peer_capacity);
            return -1;
        }
        b->peers[slot].used = true;
        b->peers[slot].wsi = wsi;
        b->peers[slot].id = ++b->next_peer_id;
        ++b->peer_count;

        s->slot = slot;
        s->assembled = 0;
        s->assembly = (unsigned char *)mye_alloc(
            conn->allocator, conn->config.max_message_size, MYE_DEFAULT_ALIGN);
        mye_log_info("net: peer %u connected (%d total)", b->peers[slot].id,
                     b->peer_count);
        return 0;
    }

    case LWS_CALLBACK_RECEIVE:
        deliver(b, s, in, len, lws_is_final_fragment(wsi) != 0);
        return 0;

    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (write_pending_for(b, wsi, s->slot) && conn->out.count > 0) {
            lws_callback_on_writable(wsi);
        }
        return 0;

    case LWS_CALLBACK_CLOSED:
        if (s->slot >= 0 && s->slot < b->peer_capacity &&
            b->peers[s->slot].used) {
            uint32_t bit = (uint32_t)1u << s->slot;
            /* Stop owing this peer anything, or its bit would pin every
             * queued broadcast forever and the queue would never drain. */
            for (int i = 0; i < conn->out.count; ++i) {
                uint32_t mask = 0;
                (void)mye_net_queue_at(&conn->out, i, NULL, &mask);
                mye_net_queue_set_tag(&conn->out, i, mask & ~bit);
            }
            mye_net_queue_retire(&conn->out);

            mye_log_info("net: peer %u disconnected", b->peers[s->slot].id);
            b->peers[s->slot].used = false;
            b->peers[s->slot].wsi = NULL;
            --b->peer_count;
        }
        if (s->assembly != NULL) {
            mye_free(conn->allocator, s->assembly,
                     conn->config.max_message_size);
            s->assembly = NULL;
        }
        return 0;

    /* --- external poll: lws tells us which sockets to watch --- */
    case LWS_CALLBACK_ADD_POLL_FD: {
        struct lws_pollargs *pa = (struct lws_pollargs *)in;
        add_fd(b, pa->fd, (short)pa->events);
        return 0;
    }

    case LWS_CALLBACK_DEL_POLL_FD: {
        struct lws_pollargs *pa = (struct lws_pollargs *)in;
        remove_fd(b, pa->fd);
        return 0;
    }

    case LWS_CALLBACK_CHANGE_MODE_POLL_FD: {
        struct lws_pollargs *pa = (struct lws_pollargs *)in;
        int i = find_fd(b, pa->fd);
        if (i >= 0) {
            b->fds[i].events = (short)pa->events;
        }
        return 0;
    }

    default:
        /* Everything else is HTTP plumbing -- and the upgrade handshake
         * itself arrives as HTTP reasons. Without delegating them the server
         * accepts the TCP connection and then never answers, which looks
         * exactly like a hung client. lws ships the default handling; this
         * protocol just has to not swallow it. */
        return lws_callback_http_dummy(wsi, reason, user, in, len);
    }
}

static struct lws_protocols protocols[] = {
    { MYE_NET_SUBPROTOCOL, protocol_cb, sizeof(session), 0, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

/* --------------------------------------------------------- lifecycle -- */

static void quiet_lws_log(int level, const char *line)
{
    /* libwebsockets' own logging, routed into the engine's single sink so a
     * crash log has one format (see core/log.h). */
    mye_log_write(level <= LLL_ERR ? MYE_LOG_ERROR : MYE_LOG_WARN, "lws", "%s",
                  line != NULL ? line : "");
}

static lws_backend *backend_alloc(mye_net_conn *conn)
{
    lws_backend *b = MYE_NEW(conn->allocator, lws_backend);
    if (b == NULL) {
        return NULL;
    }
    b->conn = conn;
    b->peer_capacity = conn->config.max_peers;
    if (b->peer_capacity > 32) {
        /* The recipient mask is a uint32_t, so 32 is the ceiling. Clamping
         * beats silently dropping peers past bit 31. */
        mye_log_warn("net: max_peers clamped from %d to 32", b->peer_capacity);
        b->peer_capacity = 32;
    }
    b->peers = MYE_NEW_ARRAY(conn->allocator, peer_slot,
                             (size_t)b->peer_capacity);
    if (b->peers == NULL) {
        MYE_DELETE(conn->allocator, b);
        return NULL;
    }
    return b;
}

static void backend_free(mye_net_conn *conn)
{
    lws_backend *b = (lws_backend *)conn->backend;
    if (b == NULL) {
        return;
    }
    if (b->ctx != NULL) {
        lws_context_destroy(b->ctx);
    }
    MYE_DELETE_ARRAY(conn->allocator, b->peers, (size_t)b->peer_capacity);
    MYE_DELETE(conn->allocator, b);
    conn->backend = NULL;
}

bool mye_net_can_listen(void)
{
    return true;
}

mye_net_conn *mye_net_connect(mye_allocator allocator, const char *url,
                              const mye_net_config *config)
{
    if (url == NULL) {
        mye_log_error("net: no url");
        return NULL;
    }

    mye_net_conn *conn = mye_net_conn_alloc(allocator, config, false);
    if (conn == NULL) {
        return NULL;
    }

    lws_backend *b = backend_alloc(conn);
    if (b == NULL) {
        mye_net_conn_free(conn);
        return NULL;
    }
    conn->backend = b;

    lws_set_log_level(LLL_ERR | LLL_WARN, quiet_lws_log);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.user = b;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT * 0;

    b->ctx = lws_create_context(&info);
    if (b->ctx == NULL) {
        mye_log_error("net: could not create the websocket context");
        backend_free(conn);
        mye_net_conn_free(conn);
        return NULL;
    }

    /* lws parses the url in place, so hand it a copy it may chew on. */
    char scratch[512];
    snprintf(scratch, sizeof scratch, "%s", url);

    const char *protocol = NULL;
    const char *address = NULL;
    const char *path = NULL;
    int port = 0;
    char *mutable_url = scratch;
    if (lws_parse_uri(mutable_url, &protocol, &address, &port, &path) != 0) {
        mye_log_error("net: could not parse '%s'", url);
        backend_free(conn);
        mye_net_conn_free(conn);
        return NULL;
    }
    if (protocol != NULL && strcmp(protocol, "wss") == 0) {
        mye_log_error("net: wss:// needs TLS, which is not built yet "
                      "(see plan/12-networking.md, N2). Use ws://");
        backend_free(conn);
        mye_net_conn_free(conn);
        return NULL;
    }

    /* lws_parse_uri strips the leading slash from the path. */
    char path_with_slash[256];
    snprintf(path_with_slash, sizeof path_with_slash, "/%s",
             path != NULL ? path : "");

    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof ccinfo);
    ccinfo.context = b->ctx;
    ccinfo.address = address;
    ccinfo.port = port;
    ccinfo.path = path_with_slash;
    ccinfo.host = address;
    ccinfo.origin = address;
    ccinfo.protocol = protocols[0].name;
    ccinfo.pwsi = &b->client_wsi;

    if (lws_client_connect_via_info(&ccinfo) == NULL) {
        mye_log_error("net: could not start connecting to %s", url);
        conn->status = MYE_NET_ERROR;
        return conn; /* the caller still owns it, and can read the status */
    }

    mye_log_info("net: connecting to %s", url);
    return conn;
}

mye_net_conn *mye_net_listen(mye_allocator allocator, uint16_t port,
                             const mye_net_config *config)
{
    mye_net_conn *conn = mye_net_conn_alloc(allocator, config, true);
    if (conn == NULL) {
        return NULL;
    }

    lws_backend *b = backend_alloc(conn);
    if (b == NULL) {
        mye_net_conn_free(conn);
        return NULL;
    }
    conn->backend = b;

    lws_set_log_level(LLL_ERR | LLL_WARN, quiet_lws_log);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = (int)port;
    info.protocols = protocols;
    info.user = b;

    b->ctx = lws_create_context(&info);
    if (b->ctx == NULL) {
        mye_log_error("net: could not listen on port %u", (unsigned)port);
        backend_free(conn);
        mye_net_conn_free(conn);
        return NULL;
    }

    conn->status = MYE_NET_OPEN; /* a listener is open with zero peers */
    mye_log_info("net: listening on ws://0.0.0.0:%u", (unsigned)port);
    return conn;
}

void mye_net_destroy(mye_net_conn *conn)
{
    if (conn == NULL) {
        return;
    }
    backend_free(conn);
    mye_net_conn_free(conn);
}

void mye_net_pump(mye_net_conn *conn)
{
    if (conn == NULL || conn->backend == NULL) {
        return;
    }
    lws_backend *b = (lws_backend *)conn->backend;

    request_writes(b);

    /* Zero timeout: poll reports what is ready right now and returns. lws
     * then services exactly those descriptors. lws_service() would instead
     * sleep until something happened, which is correct for a dedicated
     * network thread and wrong inside a frame. */
    if (b->fd_count > 0) {
        int ready = poll(b->fds, (nfds_t)b->fd_count, 0);
        for (int i = 0; ready > 0 && i < b->fd_count; ++i) {
            if (b->fds[i].revents == 0) {
                continue;
            }
            struct lws_pollfd pfd = { .fd = b->fds[i].fd,
                                      .events = b->fds[i].events,
                                      .revents = b->fds[i].revents };
            b->fds[i].revents = 0;
            --ready;
            /* Servicing can close a connection and shuffle the array, so
             * re-check the bound rather than trusting the loop variable. */
            lws_service_fd(b->ctx, &pfd);
            if (i >= b->fd_count) {
                break;
            }
        }
    }
}

/* ---------------------------------------------------------------- send -- */

static bool queue_out(mye_net_conn *conn, const void *data, size_t size,
                      uint32_t mask)
{
    if (size + LWS_PRE > conn->config.max_message_size) {
        mye_log_warn("net: message of %zu bytes exceeds max_message_size", size);
        return false;
    }
    if (mask == 0) {
        return false; /* nobody to send to */
    }

    /* Reserve the LWS_PRE headroom inside the slot by pushing an empty
     * message and copying the payload in after it. */
    if (!mye_net_queue_push(&conn->out, NULL, size + LWS_PRE, mask)) {
        return false;
    }
    unsigned char *slot = mye_net_queue_at(&conn->out, conn->out.count - 1,
                                           NULL, NULL);
    memcpy(slot + LWS_PRE, data, size);
    /* The stored size is the payload only; the headroom is not part of it. */
    conn->out.sizes[(conn->out.head + conn->out.count - 1) % conn->out.capacity] =
        size;

    lws_backend *b = (lws_backend *)conn->backend;
    request_writes(b);
    return true;
}

bool mye_net_send(mye_net_conn *conn, const void *data, size_t size)
{
    if (conn == NULL || conn->backend == NULL || data == NULL) {
        return false;
    }
    lws_backend *b = (lws_backend *)conn->backend;

    if (conn->is_server) {
        return queue_out(conn, data, size, connected_mask(b));
    }
    if (conn->status != MYE_NET_OPEN) {
        return false;
    }
    return queue_out(conn, data, size, 1u);
}

bool mye_net_send_to(mye_net_conn *conn, uint32_t peer, const void *data,
                     size_t size)
{
    if (conn == NULL || conn->backend == NULL || data == NULL) {
        return false;
    }
    if (!conn->is_server) {
        return mye_net_send(conn, data, size);
    }

    lws_backend *b = (lws_backend *)conn->backend;
    int slot = peer_slot_of_id(b, peer);
    if (slot < 0) {
        return false;
    }
    return queue_out(conn, data, size, (uint32_t)1u << slot);
}

int mye_net_peer_count(const mye_net_conn *conn)
{
    if (conn == NULL || conn->backend == NULL) {
        return 0;
    }
    return ((const lws_backend *)conn->backend)->peer_count;
}
