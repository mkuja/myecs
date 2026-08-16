# 12 — Networking (WebSockets)

Goal: a game running in the browser can talk to the outside world, and the
same game code does the same thing on desktop. Planned here, built as
milestone N0–N2 ([07-roadmap.md](07-roadmap.md) stays the authority on
ordering; networking slots in after the current milestones, on demand).

## What the browser permits

A web build cannot open sockets. The only transports a browser offers are:

| Transport | Verdict |
|---|---|
| **WebSocket** | **Chosen.** TCP-based, message-framed, binary-capable, supported everywhere, and emscripten ships a C API for it. |
| WebRTC DataChannel | UDP-like (unordered/unreliable option) — attractive for action games, but the usable libraries are C++ and the signalling/NAT machinery is a project in itself. Deferred. |
| WebTransport | QUIC-based successor; emscripten support still experimental. Deferred — the transport interface below is deliberately small so this can slot in later. |
| plain `fetch` | Request/response only. Fine for leaderboards; not a game channel. Out of scope here. |

Consequences to accept up front:

- **TCP semantics.** Messages arrive in order or the connection dies. A
  lost packet stalls everything behind it (head-of-line blocking). Fine for
  turn-based, chat, lobbies, co-op with modest tick rates; wrong for
  twitch-action state sync at scale. We accept this; WebRTC/WebTransport is
  the escape hatch later, behind the same interface.
- **Browsers cannot listen.** A web build is always the client. Servers are
  native builds (or third-party infrastructure).
- **Pages served over `https://` may only open `wss://`** (TLS). Local dev
  over `http://localhost` may use plain `ws://`.

## The library (native side)

The browser gives the web build its WebSocket for free. The native build
needs a C implementation, and per policy we do not write protocol code
ourselves ([05-concurrency.md](05-concurrency.md) precedent: libraries for
infrastructure).

**Candidate: [libwebsockets](https://libwebsockets.org/)** — pure C, CMake,
FetchContent-able, client *and* server in one library, non-blocking service
loop that fits a game's frame pump. Believed MIT-licensed — **verify the
licence text at pin time and record it in THIRD-PARTY-NOTICES.md** (the Fox
asset taught us not to assert licences from memory).

Rejected alternatives:

- *emscripten's POSIX sockets emulation* (`-sPROXY_POSIX_SOCKETS`): tunnels
  TCP through a websockify bridge, so it adds a server-side proxy and hides
  what is actually happening. The direct WebSocket API is smaller and honest.
- *mongoose*: GPL/commercial dual licence — wrong fit for this repo.
- *hand-rolled RFC 6455*: it is deceptively simple until fragmentation,
  masking, and close handshakes arrive. Same reasoning as "do not write
  TSan".

TLS on native is the one heavy decision: libwebsockets needs mbedTLS or
OpenSSL for `wss://`. **Decision deferred to N2** — N0/N1 use plain `ws://`
(dev is localhost; the browser side gets `wss://` from the browser itself
regardless). The interface carries the URL scheme, so turning TLS on later
changes no game code.

## Architecture

Same shape as jobs/channels: a plain-C core that knows nothing about the
ECS, and a thin flecs module on top ([01-architecture.md](01-architecture.md)
layering rule).

```text
game systems          read/write components, call mye_net_send/recv
  │
MyeNetModule (flecs)  a system pumps the connection once per frame,
  │                   updates a MyeNetStatus singleton
core: net.[ch]        queues + backend, allocator-aware
  │
backend               native: libwebsockets service loop
                      web:    emscripten/websocket.h callbacks
```

### Core API (sketch — names final at implementation)

```c
typedef struct mye_net_conn mye_net_conn;

typedef enum {
    MYE_NET_CONNECTING, MYE_NET_OPEN, MYE_NET_CLOSED, MYE_NET_ERROR
} mye_net_status;

/* Client (both targets). Non-blocking: returns immediately, status starts
 * CONNECTING. url is ws://host:port/path or wss://... */
mye_net_conn *mye_net_connect(mye_allocator alloc, const char *url);

/* Server (native only; the web backend returns NULL and logs why). */
mye_net_conn *mye_net_listen(mye_allocator alloc, uint16_t port);

/* Drives I/O. Called once per frame by the flecs module; callable directly
 * in tests. Never blocks. */
void mye_net_pump(mye_net_conn *conn);

/* Messages are byte blobs; WebSocket's own framing delimits them, so there
 * is no length prefix to invent. Send queues and returns false when the
 * outbound queue is full (bounded, like the audio queue -- a runaway sender
 * gets backpressure, not unbounded memory). */
bool mye_net_send(mye_net_conn *conn, const void *data, size_t size);

/* Drains one received message into a caller/allocator-owned buffer.
 * Returns false when empty. Server connections tag the peer. */
bool mye_net_recv(mye_net_conn *conn, mye_net_msg *out);

mye_net_status mye_net_status_of(const mye_net_conn *conn);
void mye_net_close(mye_net_conn *conn); /* graceful; frees after close */
```

Everything allocates through `mye_allocator` and frees with sizes
([04-memory.md](04-memory.md)); queue capacities are set at create time.

### Concurrency

**None, by design.** Both backends are pumped from the main thread once per
frame — libwebsockets in non-blocking `lws_service(ctx, 0)` mode, emscripten
via callbacks that already run on the main thread. Single-threaded pumping
means the queues need no locks at all, which beats even "mutex by default":
the cheapest synchronisation is the kind that provably isn't needed. If a
background service thread ever becomes necessary (very large payloads), the
queues gain a mutex then — per policy, atomics/lock-free only if that mutex
measurably shows up in a profile.

Web threading note: the web build is `MYE_THREADS_NONE`
([10-web.md](10-web.md)); nothing here changes that.

### The flecs module

`MyeNetModule` owns pumping and exposes state as data:

- a system early in the frame (same slot as input polling — before the fixed
  steps, driven from `mye_progress`, since simulation should see this
  frame's messages);
- a `MyeNetStatus` singleton (status, queue depths, bytes in/out) so the
  debug overlay can show it;
- messages are *not* turned into ECS events by the engine — the game drains
  `mye_net_recv` in its own system and decides what a message means. The
  engine moves bytes; meaning is gameplay.

### Protocol: deliberately not the engine's business

The engine ships **no serialization opinion**. A message is bytes. For
getting started, flecs JSON reflection already exists
([serialize.h](../engine/scene/serialize.h)) — a state snapshot is one call —
and it is debuggable in a browser console. It is also verbose; when a game
outgrows it, that game defines a binary format. The engine stays payload-
agnostic either way. (One helper worth shipping: a 1-byte message-kind
prefix convention used by the example, as a pattern to copy, not an API.)

## Dev and test story

- **The relay is a native build of the same library.** `mye_net_listen` means
  the test server is not a separate stack: a headless native process using
  the same engine code relays messages between clients. No Python
  dependencies, no websockify.
- **Unit/integration tests (native, headless):** create a listener and a
  client in one process on `ws://127.0.0.1`, pump both, assert echo,
  backpressure (full queue refuses), and clean close. Same pattern as the
  channel tests.
- **Web verification:** headless chromium against a native relay — the
  engine's console logging already round-trips through the page console, so
  a scripted client can be asserted on, exactly like the hot-reload probe.
- **web_dev.py:** unchanged. WebSockets to localhost work from `http://`
  pages; COOP/COEP headers already sent do not block them.

## Milestones

**N0 — transport.** `net.[ch]`, both backends, native client+server;
`ws://` only. Echo integration test; TSan config builds it (trivially — no
threads). *Done when:* a native test round-trips 10k messages through a
loopback listener with zero leaks, and a web build connects to the native
relay and echoes in headless chromium.

**N1 — presence example.** `examples/07_net`: run with `--serve` for a
headless relay; run normally to join. Each client owns one entity; positions
are relayed; remote entities get `MyeInterpolate` (they arrive at network
rate — exactly what render interpolation is for). Chat line via the 1-byte
kind prefix. *Done when:* two headless clients + one relay see each other's
movement in an integration test, and the same example works browser-to-native
on one machine.

**N2 — hardening.** TLS decision for native `wss://` (mbedTLS vs system
OpenSSL); reconnect-with-backoff helper; message size cap + fuzz the
receive path; THIRD-PARTY-NOTICES entry.

**Explicitly deferred:** state-sync/rollback frameworks, interest
management, NAT traversal, matchmaking, WebRTC/WebTransport backends. The
transport interface is the seam where those arrive without rewriting games.

## Risks, named

- **libwebsockets under emscripten is unnecessary** — the web backend is the
  browser's own WebSocket; the library is fetched for native configs only.
  Keeps the wasm small.
- **ASYNCIFY interplay:** callbacks arriving while the main loop is unwound
  by ASYNCIFY are queued by the runtime; the pump-drains-queue design
  means we never call user code from inside a network callback, which
  sidesteps re-entrancy entirely.
- **Head-of-line blocking is real.** The N1 example should display its own
  round-trip time so the cost is visible from day one rather than
  discovered in a shipped game.
