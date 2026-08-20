/* The flecs face of the transport. See plan/12-networking.md ("The flecs
 * module").
 *
 * net.[ch] knows nothing about the ECS; this is the thin layer that gives a
 * game the two things it would otherwise write by hand every time: something
 * that pumps the connection once per frame, and the current state of the
 * network as plain data a debug overlay can read.
 *
 *   mye_net_conn *conn = mye_net_connect(mye_allocator_of(world), url, NULL);
 *   mye_net_register(world, conn);      // now it is pumped every frame
 *   ...
 *   mye_net_unregister(world, conn);    // BEFORE mye_net_destroy
 *   mye_net_destroy(conn);
 *
 * Registration is explicit, and that is the whole design: the engine pumps
 * connections the game handed it and nothing else. A connection you never
 * registered is untouched -- no sockets are serviced behind your back, and a
 * game that wants to pump one itself (in a tool, in a test, at its own rate)
 * simply does not register it.
 *
 * The pump runs in mye_progress, in the same slot as input polling and for
 * the same reason: the fixed simulation steps have to see the messages that
 * arrived for THIS frame, and they run outside the pipeline where an ordinary
 * system could not reach them.
 *
 * Receiving is still the game's job. The engine moves bytes; what a message
 * means is gameplay, so nothing here turns a message into an ECS event --
 * drain mye_net_recv in your own system and decide what it meant.
 */
#ifndef MYE_NET_NET_MODULE_H
#define MYE_NET_NET_MODULE_H

#include "core/engine.h"
#include "net/net.h"

/* A game with more than a handful of simultaneous connections is a server,
 * and a server registers one listener. Fixed so the singleton stays a plain
 * struct with no allocation of its own. */
#define MYE_NET_MAX_REGISTERED 8

/* Read-only summary of every registered connection, updated once per frame by
 * the pump. Aggregate rather than per-connection because that is what an
 * overlay line wants; the per-connection numbers are still one
 * mye_net_bytes_sent() away. */
typedef struct MyeNetStatus {
    /* Engine-owned: use mye_net_register / mye_net_unregister. Listed here
     * rather than hidden in module state so the registry is inspectable like
     * everything else. */
    mye_net_conn *conns[MYE_NET_MAX_REGISTERED];
    int count;

    /* The first registered connection's status -- the one a game with a
     * single connection means when it asks. MYE_NET_IDLE when none. */
    mye_net_status status;

    int peers;         /* summed across registered listeners */
    int recv_pending;  /* messages waiting to be drained */
    int send_pending;  /* messages waiting to go out; a rising number is
                        * backpressure, i.e. the socket cannot keep up */
    uint64_t bytes_in;
    uint64_t bytes_out;

    /* Frames pumped since the world started. If this is not climbing, the
     * pump is not running and every other number here is stale. */
    uint64_t pumps;

    /* Engine-owned: the system entity mye_net_poll runs. */
    ecs_entity_t pump_system;
} MyeNetStatus;

extern ECS_COMPONENT_DECLARE(MyeNetStatus);

void MyeNetModuleImport(ecs_world_t *world);

/* Adds `conn` to the set pumped every frame. Registering the same connection
 * twice is a no-op. False when the world has no net module, `conn` is NULL,
 * or the registry is full (logged).
 *
 * Ownership does not move: the connection is still yours to destroy, and it
 * must be unregistered first, or the pump will service freed memory. */
bool mye_net_register(ecs_world_t *world, mye_net_conn *conn);

/* Removes it again; returns false if it was not registered. Does not close or
 * destroy anything -- unregistering is about who pumps, not about the socket. */
bool mye_net_unregister(ecs_world_t *world, mye_net_conn *conn);

/* Pumps every registered connection and refreshes MyeNetStatus. Called by
 * mye_progress; a game does not call this, and a test that drives the world
 * by hand may. Safe with nothing registered. */
void mye_net_poll(ecs_world_t *world);

#endif /* MYE_NET_NET_MODULE_H */
