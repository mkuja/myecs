#include "core/channel.h"

#include "core/thread.h"

#include <string.h>

/* A plain mutex-protected ring buffer.
 *
 * Per the policy in plan/05-concurrency.md: mutex by default, atomics on hot
 * spots, lock-free only at a hot spot that additionally has a single writer
 * or tolerates stale reads. This channel is none of those -- it carries a
 * handful of asset-upload messages per frame, has many producers, and must
 * not lose a message. The mutex is held only for a memcpy, so the critical
 * section is far shorter than the work on either side of it. */
struct mye_channel {
    mye_allocator allocator;

    uint8_t *data;
    size_t capacity;     /* power of two */
    size_t mask;         /* capacity - 1 */
    size_t element_size;
    size_t stride;       /* element_size rounded up for alignment */

    size_t head;         /* next slot to write */
    size_t tail;         /* next slot to read */
    size_t count;        /* live elements */

    mye_mutex mutex;
};

static bool is_power_of_two(size_t v)
{
    return v >= 2 && (v & (v - 1)) == 0;
}

mye_channel *mye_channel_create(mye_allocator allocator, size_t capacity,
                                size_t element_size)
{
    if (!is_power_of_two(capacity) || element_size == 0 ||
        !mye_allocator_valid(allocator)) {
        return NULL;
    }

    size_t stride = mye_align_up(element_size, MYE_DEFAULT_ALIGN);
    if (stride == SIZE_MAX || stride > SIZE_MAX / capacity) {
        return NULL;
    }

    mye_channel *channel = MYE_NEW(allocator, mye_channel);
    if (channel == NULL) {
        return NULL;
    }

    channel->allocator = allocator;
    channel->capacity = capacity;
    channel->mask = capacity - 1;
    channel->element_size = element_size;
    channel->stride = stride;

    channel->data = (uint8_t *)mye_alloc(allocator, stride * capacity,
                                         MYE_DEFAULT_ALIGN);
    if (channel->data == NULL || !mye_mutex_init(&channel->mutex)) {
        if (channel->data != NULL) {
            mye_free(allocator, channel->data, stride * capacity);
        }
        MYE_DELETE(allocator, channel);
        return NULL;
    }

    return channel;
}

void mye_channel_destroy(mye_channel *channel)
{
    if (channel == NULL) {
        return;
    }
    mye_allocator a = channel->allocator;
    mye_mutex_destroy(&channel->mutex);
    mye_free(a, channel->data, channel->stride * channel->capacity);
    MYE_DELETE(a, channel);
}

bool mye_channel_send(mye_channel *channel, const void *element)
{
    if (channel == NULL || element == NULL) {
        return false;
    }

    mye_mutex_lock(&channel->mutex);

    bool sent = false;
    if (channel->count < channel->capacity) {
        memcpy(channel->data + (channel->head & channel->mask) *
                                   channel->stride,
               element, channel->element_size);
        channel->head = (channel->head + 1) & channel->mask;
        ++channel->count;
        sent = true;
    }

    mye_mutex_unlock(&channel->mutex);
    return sent; /* false = full; the caller decides what that means */
}

bool mye_channel_recv(mye_channel *channel, void *out_element)
{
    if (channel == NULL || out_element == NULL) {
        return false;
    }

    mye_mutex_lock(&channel->mutex);

    bool received = false;
    if (channel->count > 0) {
        memcpy(out_element,
               channel->data + (channel->tail & channel->mask) *
                                   channel->stride,
               channel->element_size);
        channel->tail = (channel->tail + 1) & channel->mask;
        --channel->count;
        received = true;
    }

    mye_mutex_unlock(&channel->mutex);
    return received; /* false = empty */
}

size_t mye_channel_capacity(const mye_channel *channel)
{
    return channel != NULL ? channel->capacity : 0;
}

size_t mye_channel_count(const mye_channel *channel)
{
    if (channel == NULL) {
        return 0;
    }
    /* Cast away const to take the lock: reading the count is logically a
     * const operation even though locking is not. */
    mye_channel *mutable_channel = (mye_channel *)(uintptr_t)channel;
    mye_mutex_lock(&mutable_channel->mutex);
    size_t count = mutable_channel->count;
    mye_mutex_unlock(&mutable_channel->mutex);
    return count;
}
