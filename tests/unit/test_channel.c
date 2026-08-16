/* Unit tests for the lock-free channel. Single-threaded correctness first,
 * then a genuine multi-threaded stress test -- the only kind that can catch
 * a lost or duplicated message. See plan/09-testing.md. */
#include "core/channel.h"
#include "mye_test.h"

#include <stdatomic.h>
#include "core/thread.h"

typedef struct msg {
    int id;
    double payload;
} msg;

TEST(create_rejects_bad_arguments)
{
    mye_allocator a = mye_heap_allocator();

    ASSERT_NULL(mye_channel_create(a, 0, sizeof(msg)));   /* zero capacity */
    ASSERT_NULL(mye_channel_create(a, 1, sizeof(msg)));   /* below minimum */
    ASSERT_NULL(mye_channel_create(a, 6, sizeof(msg)));   /* not a power of 2 */
    ASSERT_NULL(mye_channel_create(a, 8, 0));             /* zero elements */
    ASSERT_NULL(mye_channel_create((mye_allocator){ 0 }, 8, sizeof(msg)));

    mye_channel_destroy(NULL); /* no-op, not a crash */
}

TEST(send_recv_preserves_order_and_contents)
{
    mye_channel *c = mye_channel_create(mye_heap_allocator(), 8, sizeof(msg));
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_U64(8, mye_channel_capacity(c));
    ASSERT_EQ_U64(0, mye_channel_count(c));

    msg out;
    ASSERT_FALSE(mye_channel_recv(c, &out)); /* empty */

    for (int i = 0; i < 5; ++i) {
        msg in = { .id = i, .payload = i * 1.5 };
        ASSERT_TRUE(mye_channel_send(c, &in));
    }
    ASSERT_EQ_U64(5, mye_channel_count(c));

    /* FIFO, byte-for-byte. */
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(mye_channel_recv(c, &out));
        ASSERT_EQ_INT(i, out.id);
        ASSERT_NEAR(i * 1.5, out.payload, 1e-9);
    }
    ASSERT_FALSE(mye_channel_recv(c, &out));
    ASSERT_EQ_U64(0, mye_channel_count(c));

    mye_channel_destroy(c);
}

TEST(full_channel_refuses_sends)
{
    mye_channel *c = mye_channel_create(mye_heap_allocator(), 4, sizeof(msg));
    ASSERT_NOT_NULL(c);

    for (int i = 0; i < 4; ++i) {
        msg in = { .id = i, .payload = 0.0 };
        ASSERT_TRUE(mye_channel_send(c, &in));
    }

    /* Full: refuses rather than blocking or overwriting. */
    msg overflow = { .id = 99, .payload = 0.0 };
    ASSERT_FALSE(mye_channel_send(c, &overflow));
    ASSERT_EQ_U64(4, mye_channel_count(c));

    /* Freeing one slot makes room for exactly one more. */
    msg out;
    ASSERT_TRUE(mye_channel_recv(c, &out));
    ASSERT_EQ_INT(0, out.id);
    ASSERT_TRUE(mye_channel_send(c, &overflow));
    ASSERT_FALSE(mye_channel_send(c, &overflow));

    mye_channel_destroy(c);
}

TEST(indices_wrap_around_many_times)
{
    /* Ring reuse is where an off-by-one in the sequence numbers would show:
     * push and pop far more messages than the capacity. */
    mye_channel *c = mye_channel_create(mye_heap_allocator(), 4, sizeof(msg));
    ASSERT_NOT_NULL(c);

    for (int i = 0; i < 1000; ++i) {
        msg in = { .id = i, .payload = 0.0 };
        ASSERT_TRUE(mye_channel_send(c, &in));

        msg out;
        ASSERT_TRUE(mye_channel_recv(c, &out));
        ASSERT_EQ_INT(i, out.id);
    }
    ASSERT_EQ_U64(0, mye_channel_count(c));

    mye_channel_destroy(c);
}

TEST(null_arguments_are_refused)
{
    mye_channel *c = mye_channel_create(mye_heap_allocator(), 4, sizeof(msg));
    ASSERT_NOT_NULL(c);

    msg m = { 0, 0.0 };
    ASSERT_FALSE(mye_channel_send(c, NULL));
    ASSERT_FALSE(mye_channel_send(NULL, &m));
    ASSERT_FALSE(mye_channel_recv(c, NULL));
    ASSERT_FALSE(mye_channel_recv(NULL, &m));
    ASSERT_EQ_U64(0, mye_channel_capacity(NULL));
    ASSERT_EQ_U64(0, mye_channel_count(NULL));

    mye_channel_destroy(c);
}

/* ------------------------------------------------------ threaded stress -- */

#define PRODUCERS 4
#define PER_PRODUCER 5000

typedef struct producer_ctx {
    mye_channel *channel;
    int producer_id;
    atomic_int *sent;
} producer_ctx;

static MYE_THREAD_RETURN producer_main(void *arg)
{
    producer_ctx *ctx = (producer_ctx *)arg;

    for (int i = 0; i < PER_PRODUCER; ++i) {
        msg m = { .id = ctx->producer_id, .payload = (double)i };
        /* Retry until it fits: the consumer drains concurrently. */
        while (!mye_channel_send(ctx->channel, &m)) {
            mye_thread_yield();
        }
        atomic_fetch_add(ctx->sent, 1);
    }
    return MYE_THREAD_RESULT;
}

TEST(many_producers_one_consumer_lose_nothing)
{
    /* The real question for a lock-free queue: with several threads pushing
     * at once, does every message arrive exactly once? A deliberately small
     * ring forces constant wraparound and contention. */
    mye_channel *c = mye_channel_create(mye_heap_allocator(), 16, sizeof(msg));
    ASSERT_NOT_NULL(c);

    atomic_int sent;
    atomic_init(&sent, 0);

    mye_thread threads[PRODUCERS];
    producer_ctx ctx[PRODUCERS];
    for (int i = 0; i < PRODUCERS; ++i) {
        ctx[i] = (producer_ctx){ .channel = c, .producer_id = i,
                                 .sent = &sent };
        ASSERT_TRUE(mye_thread_create(&threads[i], producer_main, &ctx[i]));
    }

    /* Consume on this thread until every message has been accounted for. */
    int received[PRODUCERS] = { 0 };
    int total = 0;
    const int expected = PRODUCERS * PER_PRODUCER;
    while (total < expected) {
        msg out;
        if (mye_channel_recv(c, &out)) {
            ASSERT_TRUE(out.id >= 0 && out.id < PRODUCERS);
            ++received[out.id];
            ++total;
        } else {
            mye_thread_yield();
        }
    }

    for (int i = 0; i < PRODUCERS; ++i) {
        mye_thread_join(threads[i]);
        /* Every producer's messages arrived, none duplicated. */
        ASSERT_EQ_INT(PER_PRODUCER, received[i]);
    }
    ASSERT_EQ_INT(expected, atomic_load(&sent));
    ASSERT_EQ_U64(0, mye_channel_count(c));

    mye_channel_destroy(c);
}

TEST_MAIN(TEST_CASE(create_rejects_bad_arguments),
          TEST_CASE(send_recv_preserves_order_and_contents),
          TEST_CASE(full_channel_refuses_sends),
          TEST_CASE(indices_wrap_around_many_times),
          TEST_CASE(null_arguments_are_refused),
          TEST_CASE(many_producers_one_consumer_lose_nothing))
