/* Bounded lock-free queue for passing messages between threads.
 * See plan/05-concurrency.md.
 *
 * This is the "channel" in the Tokio/Rayon sense: threads exchange *messages*
 * (plain structs, copied in and out), never shared mutable state. An
 * ecs_entity_t is a 64-bit id and is safe to put in a message; the receiving
 * side touches the world only from the main thread.
 *
 *   mye_channel *c = mye_channel_create(alloc, 64, sizeof(my_msg));
 *   mye_channel_send(c, &msg);          // worker thread, never blocks
 *   while (mye_channel_recv(c, &msg)) { ... }   // main thread, drains
 *
 * Multi-producer, multi-consumer. Both ends are non-blocking: send fails when
 * full, recv fails when empty, and the caller decides what that means. No
 * operation ever waits for the other side.
 *
 * Implemented as a mutex-protected ring buffer, per the primitive policy in
 * plan/05-concurrency.md. The lock is held only across a memcpy. If profiling
 * ever shows this to be a hot spot, the implementation can change without
 * touching a single call site -- that is the point of the interface.
 */
#ifndef MYE_CORE_CHANNEL_H
#define MYE_CORE_CHANNEL_H

#include "core/alloc.h"

typedef struct mye_channel mye_channel;

/* `capacity` must be a power of two and at least 2. Returns NULL on bad
 * arguments or allocation failure. */
mye_channel *mye_channel_create(mye_allocator allocator, size_t capacity,
                                size_t element_size);
void mye_channel_destroy(mye_channel *channel);

/* Copies `element_size` bytes in. false means the queue is full. */
bool mye_channel_send(mye_channel *channel, const void *element);
/* Copies `element_size` bytes out. false means the queue is empty. */
bool mye_channel_recv(mye_channel *channel, void *out_element);

size_t mye_channel_capacity(const mye_channel *channel);
/* Exact at the moment it is taken, but another thread may change it before
 * you act on it. Useful for stats and tests, never for control flow. */
size_t mye_channel_count(const mye_channel *channel);

#endif /* MYE_CORE_CHANNEL_H */
