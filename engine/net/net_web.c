/* Browser WebSocket backend. See plan/12-networking.md.
 *
 * The socket lives in JavaScript and its event handlers touch nothing but
 * JavaScript state: incoming messages go into a JS array, and status is a
 * number. Our pump then reaches *out* to collect them.
 *
 * That direction matters, and it is the whole reason this file does not use
 * emscripten/websocket.h. With ASYNCIFY the game loop spends most of its
 * time unwound, and a socket event that calls back into wasm re-enters it
 * mid-unwind. The symptom is not a crash: the loop simply stops advancing --
 * an open connection, a rendered first frame, and a frame counter frozen at
 * 1 forever. Keeping every wasm entry inside our own loop avoids it by
 * construction, which is what the plan asked for and what the first attempt
 * failed to do.
 */
#include "net/net_internal.h"

#include <emscripten/emscripten.h>

/* Status codes shared with the JS below; kept numeric because they cross a
 * language boundary. */
#define WEB_CONNECTING 1
#define WEB_OPEN 2
#define WEB_CLOSED 3
#define WEB_ERROR 4

EM_JS(int, mye_ws_open, (const char *url, const char *protocol), {
    var sockets = (Module.myeSockets = Module.myeSockets || []);
    var slot = { ws: null, status: 1, queue: [] };

    try {
        slot.ws = new WebSocket(UTF8ToString(url), UTF8ToString(protocol));
    } catch (e) {
        return -1;
    }
    slot.ws.binaryType = 'arraybuffer';

    /* Deliberately no calls into wasm from any of these. */
    slot.ws.onopen = function() { slot.status = 2; };
    slot.ws.onclose = function() { if (slot.status != 4) slot.status = 3; };
    slot.ws.onerror = function() { slot.status = 4; };
    slot.ws.onmessage = function(event) {
        if (typeof event.data === 'string') {
            slot.queue.push(new TextEncoder().encode(event.data));
        } else {
            slot.queue.push(new Uint8Array(event.data));
        }
    };

    sockets.push(slot);
    return sockets.length - 1;
})

EM_JS(int, mye_ws_status, (int handle), {
    var s = Module.myeSockets[handle];
    return s ? s.status : 4;
})

EM_JS(int, mye_ws_pending, (int handle), {
    var s = Module.myeSockets[handle];
    return (s && s.queue.length) ? s.queue.length : 0;
})

/* Copies the oldest message into wasm memory and drops it. Returns its size,
 * 0 when the queue is empty, or -1 when it would not fit -- the caller then
 * drops it, matching the native backend's "never truncate" rule. */
EM_JS(int, mye_ws_take, (int handle, unsigned char *destination, int capacity), {
    var s = Module.myeSockets[handle];
    if (!s || !s.queue.length) return 0;
    var message = s.queue[0];
    if (message.length > capacity) { s.queue.shift(); return -1; }
    HEAPU8.set(message, destination);
    s.queue.shift();
    return message.length;
})

EM_JS(int, mye_ws_send, (int handle, const unsigned char *data, int size), {
    var s = Module.myeSockets[handle];
    if (!s || !s.ws || s.ws.readyState !== 1) return 0;
    /* Copied, not a view: the socket may keep it past this call, and the
     * wasm heap moves when it grows. */
    s.ws.send(new Uint8Array(HEAPU8.subarray(data, data + size)));
    return 1;
})

EM_JS(void, mye_ws_close, (int handle), {
    var s = Module.myeSockets[handle];
    if (s && s.ws) { try { s.ws.close(); } catch (e) {} s.ws = null; }
})

EM_JS(int, mye_ws_supported, (void), {
    return (typeof WebSocket !== 'undefined') ? 1 : 0;
})

typedef struct web_backend {
    int handle;
} web_backend;

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
    if (!mye_ws_supported()) {
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

    b->handle = mye_ws_open(url, MYE_NET_SUBPROTOCOL);
    if (b->handle < 0) {
        mye_log_error("net: the browser refused to open %s", url);
        MYE_DELETE(allocator, b);
        mye_net_conn_free(conn);
        return NULL;
    }

    conn->backend = b;
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
        mye_ws_close(b->handle);
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

    switch (mye_ws_status(b->handle)) {
    case WEB_OPEN:
        if (conn->status != MYE_NET_OPEN) {
            conn->status = MYE_NET_OPEN;
            mye_log_info("net: connected");
        }
        break;
    case WEB_CLOSED:
        conn->status = MYE_NET_CLOSED;
        break;
    case WEB_ERROR:
        conn->status = MYE_NET_ERROR;
        break;
    default:
        conn->status = MYE_NET_CONNECTING;
        break;
    }

    /* Collect whatever the socket buffered since the last frame. Whole
     * messages only -- the browser reassembles fragments itself. */
    while (mye_ws_pending(b->handle) > 0) {
        if (conn->in.count >= conn->in.capacity) {
            mye_log_warn("net: receive queue full; leaving messages buffered");
            break;
        }

        unsigned char scratch[MYE_NET_DEFAULT_MAX_MESSAGE];
        size_t capacity = conn->config.max_message_size < sizeof scratch
                              ? conn->config.max_message_size
                              : sizeof scratch;

        int size = mye_ws_take(b->handle, scratch, (int)capacity);
        if (size == 0) {
            break;
        }
        if (size < 0) {
            mye_log_warn("net: dropped a message over max_message_size (%zu)",
                         capacity);
            continue;
        }

        if (mye_net_queue_push(&conn->in, scratch, (size_t)size, 0)) {
            conn->bytes_in += (uint64_t)size;
        }
    }

    if (conn->status != MYE_NET_OPEN) {
        return;
    }

    while (conn->out.count > 0) {
        size_t size = 0;
        unsigned char *bytes = mye_net_queue_at(&conn->out, 0, &size, NULL);
        if (bytes == NULL || !mye_ws_send(b->handle, bytes, (int)size)) {
            break; /* not writable yet: leave it queued for the next frame */
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
