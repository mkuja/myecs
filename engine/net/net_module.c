/* The flecs module over net.[ch]. See net_module.h and plan/12-networking.md.
 *
 * Kept in its own translation unit rather than folded into net.c: net.c is
 * the plain-C core and knows nothing about flecs, which is the layering rule
 * the whole engine is built on (plan/01-architecture.md). Folding this in
 * would drag the ECS into the layer whose job is to have no opinions.
 */
#include "net/net_module.h"

#include "core/log.h"

ECS_COMPONENT_DECLARE(MyeNetStatus);

/* Pumps what the game registered, then republishes the numbers as data.
 *
 * Deliberately created with no phase, so the builtin pipeline never picks it
 * up: mye_progress runs it explicitly, before the fixed steps, which is
 * earlier than any pipeline phase can reach. Same trick, and the same reason,
 * as the fixed-step pipeline in core/engine.c. */
static void MyeNetPump(ecs_iter_t *it)
{
    MyeNetStatus *net = ecs_field(it, MyeNetStatus, 0);

    int peers = 0;
    int recv_pending = 0;
    int send_pending = 0;
    uint64_t bytes_in = 0;
    uint64_t bytes_out = 0;

    for (int i = 0; i < net->count; ++i) {
        mye_net_conn *conn = net->conns[i];
        mye_net_pump(conn);

        peers += mye_net_peer_count(conn);
        recv_pending += mye_net_recv_pending(conn);
        send_pending += mye_net_send_pending(conn);
        bytes_in += mye_net_bytes_received(conn);
        bytes_out += mye_net_bytes_sent(conn);
    }

    net->status = net->count > 0 ? mye_net_status_of(net->conns[0])
                                 : MYE_NET_IDLE;
    net->peers = peers;
    net->recv_pending = recv_pending;
    net->send_pending = send_pending;
    net->bytes_in = bytes_in;
    net->bytes_out = bytes_out;
    ++net->pumps;
}

void MyeNetModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeNetModule);

    ECS_COMPONENT_DEFINE(world, MyeNetStatus);
    ecs_add_id(world, ecs_id(MyeNetStatus), EcsSingleton);
    ecs_singleton_set(world, MyeNetStatus, { 0 });

    ecs_entity_t pump = ecs_system(world, {
        .entity = ecs_entity(world, { .name = "MyeNetPump" }),
        .query.terms = {{ .id = ecs_id(MyeNetStatus) }},
        .callback = MyeNetPump,
    });

    MyeNetStatus *net = ecs_singleton_ensure(world, MyeNetStatus);
    net->pump_system = pump;
    ecs_singleton_modified(world, MyeNetStatus);
}

bool mye_net_register(ecs_world_t *world, mye_net_conn *conn)
{
    if (world == NULL || conn == NULL) {
        return false;
    }
    MyeNetStatus *net = ecs_singleton_ensure(world, MyeNetStatus);
    if (net == NULL) {
        mye_log_error("net: this world has no net module");
        return false;
    }

    for (int i = 0; i < net->count; ++i) {
        if (net->conns[i] == conn) {
            return true; /* already pumped; asking twice is not an error */
        }
    }
    if (net->count >= MYE_NET_MAX_REGISTERED) {
        mye_log_error("net: cannot pump more than %d connections",
                      MYE_NET_MAX_REGISTERED);
        return false;
    }

    net->conns[net->count++] = conn;
    ecs_singleton_modified(world, MyeNetStatus);
    return true;
}

bool mye_net_unregister(ecs_world_t *world, mye_net_conn *conn)
{
    if (world == NULL || conn == NULL) {
        return false;
    }
    MyeNetStatus *net = ecs_singleton_ensure(world, MyeNetStatus);
    if (net == NULL) {
        return false;
    }

    for (int i = 0; i < net->count; ++i) {
        if (net->conns[i] != conn) {
            continue;
        }
        /* Order does not matter, so close the hole with the last entry. */
        net->conns[i] = net->conns[net->count - 1];
        net->conns[net->count - 1] = NULL;
        --net->count;
        if (net->count == 0) {
            net->status = MYE_NET_IDLE;
        }
        ecs_singleton_modified(world, MyeNetStatus);
        return true;
    }
    return false;
}

void mye_net_poll(ecs_world_t *world)
{
    if (world == NULL) {
        return;
    }
    const MyeNetStatus *net = ecs_singleton_get(world, MyeNetStatus);
    if (net == NULL || net->pump_system == 0) {
        return; /* a world built without the module: nothing to pump */
    }
    ecs_run(world, net->pump_system, 0.0f, NULL);
}
