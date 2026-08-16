/* Browser WebSocket backend. See plan/12-networking.md.
 *
 * The browser owns the socket, so there is no protocol library here and
 * nothing of libwebsockets reaches the wasm. Callbacks fire on the main
 * thread and only ever push into the receive queue. Nothing calls game code
 * from inside a callback.
 *
 * STATUS: partially verified. In a headless browser this connects, completes
 * the handshake, and delivers a message that a native relay confirms
 * receiving. What has NOT been observed working is the browser receiving a
 * message back, or the game loop continuing to send after the first frame --
 * a run of any length produces exactly one outbound message. The cause is
 * not yet identified; the obvious suspect, ASYNCIFY re-entrancy from an
 * inbound callback, was ruled out by pointing the client at a relay that
 * never echoes and seeing the same single message. Treat the receive path
 * here as unproven until there is a test that demonstrates it.
 */
#include "net/net_internal.h"

#include <emscripten/websocket.h>

typedef struct web_backend {
    EMSCRIPTEN_WEBSOCKET_T socket;
    mye_net_conn *conn;
} web_backend;

static EM_BOOL on_open(int type, const EmscriptenWebSocketOpenEvent *event,
                       void *user)
{
    (void)type;
    (void)event;
    mye_net_conn *conn = (mye_net_conn *)user;
    conn->status = MYE_NET_OPEN;
    mye_log_info("net: connected");
    return EM_TRUE;
}

static EM_BOOL on_message(int type, const EmscriptenWebSocketMessageEvent *event,
                          void *user)
{
    (void)type;
    mye_net_conn *conn = (mye_net_conn *)user;

    /* The browser hands over whole messages -- it reassembles fragments
     * itself -- so unlike the native backend there is nothing to stitch. */
    if (!mye_net_queue_push(&conn->in, event->data, event->numBytes, 0)) {
        mye_log_warn("net: receive queue full or message too large; dropped "
                     "%u bytes",
                     (unsigned)event->numBytes);
        return EM_TRUE;
    }
    conn->bytes_in += event->numBytes;
    return EM_TRUE;
}

static EM_BOOL on_error(int type, const EmscriptenWebSocketErrorEvent *event,
                        void *user)
{
    (void)type;
    (void)event;
    mye_net_conn *conn = (mye_net_conn *)user;
    conn->status = MYE_NET_ERROR;
    /* The browser deliberately withholds the reason from script, to avoid
     * leaking cross-origin information. Check the devtools console. */
    mye_log_warn("net: connection failed (the browser does not say why; see "
                 "the devtools console)");
    return EM_TRUE;
}

static EM_BOOL on_close(int type, const EmscriptenWebSocketCloseEvent *event,
                        void *user)
{
    (void)type;
    mye_net_conn *conn = (mye_net_conn *)user;
    if (conn->status != MYE_NET_ERROR) {
        conn->status = MYE_NET_CLOSED;
    }
    mye_log_info("net: closed (code %d)", event->code);
    return EM_TRUE;
}

bool mye_net_can_listen(void)
{
    return false;
}

mye_net_conn *mye_net_connect(mye_allocator allocator, const char *url,
                              const mye_net_config *config)
{
    if (url == NULL) {
        mye_log_error("net: no url");
        return NULL;
    }
    if (!emscripten_websocket_is_supported()) {
        mye_log_error("net: this browser has no WebSocket support");
        return NULL;
    }

    mye_net_conn *conn = mye_net_conn_alloc(allocator, config, false);
    if (conn == NULL) {
        return NULL;
    }

    web_backend *b = MYE_NEW(allocator, web_backend);
    if (b == NULL) {
        mye_net_conn_free(conn);
        return NULL;
    }
    b->conn = conn;
    conn->backend = b;

    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = url;
    attr.protocols = MYE_NET_SUBPROTOCOL;
    attr.createOnMainThread = EM_TRUE;

    b->socket = emscripten_websocket_new(&attr);
    if (b->socket <= 0) {
        mye_log_error("net: could not create a websocket for %s", url);
        MYE_DELETE(allocator, b);
        conn->backend = NULL;
        mye_net_conn_free(conn);
        return NULL;
    }

    emscripten_websocket_set_onopen_callback(b->socket, conn, on_open);
    emscripten_websocket_set_onmessage_callback(b->socket, conn, on_message);
    emscripten_websocket_set_onerror_callback(b->socket, conn, on_error);
    emscripten_websocket_set_onclose_callback(b->socket, conn, on_close);

    mye_log_info("net: connecting to %s", url);
    return conn;
}

mye_net_conn *mye_net_listen(mye_allocator allocator, uint16_t port,
                             const mye_net_config *config)
{
    (void)allocator;
    (void)config;
    mye_log_error("net: a web build cannot listen (port %u). A page is always "
                  "the client; run the server as a native build.",
                  (unsigned)port);
    return NULL;
}

void mye_net_destroy(mye_net_conn *conn)
{
    if (conn == NULL) {
        return;
    }
    web_backend *b = (web_backend *)conn->backend;
    if (b != NULL) {
        if (b->socket > 0) {
            emscripten_websocket_close(b->socket, 1000, "bye");
            emscripten_websocket_delete(b->socket);
        }
        MYE_DELETE(conn->allocator, b);
        conn->backend = NULL;
    }
    mye_net_conn_free(conn);
}

void mye_net_pump(mye_net_conn *conn)
{
    if (conn == NULL || conn->backend == NULL) {
        return;
    }
    web_backend *b = (web_backend *)conn->backend;

    /* Receiving needs no pumping -- the browser calls us. Sending is queued
     * rather than immediate so that the API behaves the same on both
     * targets: a send before the socket opens waits here instead of being
     * lost. */
    if (conn->status != MYE_NET_OPEN) {
        return;
    }

    while (conn->out.count > 0) {
        size_t size = 0;
        unsigned char *bytes = mye_net_queue_at(&conn->out, 0, &size, NULL);
        if (bytes == NULL) {
            break;
        }
        if (emscripten_websocket_send_binary(b->socket, bytes,
                                             (uint32_t)size) != EMSCRIPTEN_RESULT_SUCCESS) {
            mye_log_warn("net: send failed; leaving the message queued");
            break;
        }
        conn->bytes_out += size;
        mye_net_queue_pop(&conn->out);
    }
}

bool mye_net_send(mye_net_conn *conn, const void *data, size_t size)
{
    if (conn == NULL || conn->backend == NULL || data == NULL) {
        return false;
    }
    if (size > conn->config.max_message_size) {
        mye_log_warn("net: message of %zu bytes exceeds max_message_size", size);
        return false;
    }
    return mye_net_queue_push(&conn->out, data, size, 1u);
}

bool mye_net_send_to(mye_net_conn *conn, uint32_t peer, const void *data,
                     size_t size)
{
    (void)peer; /* a client has exactly one correspondent */
    return mye_net_send(conn, data, size);
}

int mye_net_peer_count(const mye_net_conn *conn)
{
    return (conn != NULL && conn->status == MYE_NET_OPEN) ? 1 : 0;
}
