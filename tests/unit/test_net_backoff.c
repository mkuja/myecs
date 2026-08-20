/* The reconnect ladder: a state machine with no socket in it, so it can be
 * tested exhaustively in a loop that never sleeps and never binds a port.
 * See engine/net/net.h and plan/12-networking.md (N2). */
#include "mye_test.h"

#include "net/net.h"

/* Ticks until the retry is due, or `limit` ticks, whichever comes first.
 * Returns the number of ticks it took, or -1 if it never fired -- an assert
 * on a count is a much better failure message than a hang. */
static int ticks_until_ready(mye_net_backoff *backoff, double dt, int limit)
{
    for (int i = 1; i <= limit; ++i) {
        if (mye_net_backoff_ready(backoff, dt)) {
            return i;
        }
    }
    return -1;
}

TEST(defaults_fill_in_every_zero_field)
{
    mye_net_backoff backoff;
    mye_net_backoff_init(&backoff, NULL);

    ASSERT_NEAR(0.5, backoff.config.first_delay, 1e-9);
    ASSERT_NEAR(8.0, backoff.config.max_delay, 1e-9);
    ASSERT_NEAR(2.0, backoff.config.factor, 1e-9);
    ASSERT_EQ_INT(0, backoff.config.max_attempts); /* forever */

    /* Nothing has failed yet, so nothing is pending. */
    ASSERT_FALSE(mye_net_backoff_ready(&backoff, 100.0));
    ASSERT_NEAR(0.0, mye_net_backoff_remaining(&backoff), 1e-9);
}

/* The whole point of a backoff: a retry that happens immediately is not one.
 * A client that redials the instant it notices the socket died turns a server
 * restart into a denial of service by its own players. */
TEST(a_retry_waits_before_the_first_attempt)
{
    mye_net_backoff backoff;
    mye_net_backoff_init(&backoff, &(mye_net_backoff_config){
        .first_delay = 1.0, .factor = 2.0, .max_delay = 60.0 });

    mye_net_backoff_failed(&backoff);
    ASSERT_NEAR(1.0, mye_net_backoff_remaining(&backoff), 1e-9);

    /* Not yet: 0.9 s of a 1 s wait. */
    ASSERT_FALSE(mye_net_backoff_ready(&backoff, 0.5));
    ASSERT_FALSE(mye_net_backoff_ready(&backoff, 0.4));
    ASSERT_NEAR(0.1, mye_net_backoff_remaining(&backoff), 1e-9);

    /* Now. */
    ASSERT_TRUE(mye_net_backoff_ready(&backoff, 0.1));
    /* And exactly once: the caller dialled, so there is nothing pending. */
    ASSERT_FALSE(mye_net_backoff_ready(&backoff, 10.0));
    ASSERT_EQ_INT(1, backoff.attempts);
}

TEST(each_failure_waits_longer_up_to_the_ceiling)
{
    mye_net_backoff backoff;
    mye_net_backoff_init(&backoff, &(mye_net_backoff_config){
        .first_delay = 1.0, .factor = 2.0, .max_delay = 4.0 });

    const double expected[] = { 1.0, 2.0, 4.0, 4.0, 4.0 };
    for (int i = 0; i < 5; ++i) {
        mye_net_backoff_failed(&backoff);
        ASSERT_NEAR(expected[i], mye_net_backoff_remaining(&backoff), 1e-9);
        /* 0.5 s ticks, so the count is twice the delay. */
        ASSERT_EQ_INT((int)(expected[i] * 2.0),
                      ticks_until_ready(&backoff, 0.5, 100));
    }
}

/* Failure is noticed by polling a status every frame, so `failed` is called
 * sixty times a second while the connection is down. A timer that restarted
 * on each call would never expire. */
TEST(repeated_failures_do_not_restart_the_current_wait)
{
    mye_net_backoff backoff;
    mye_net_backoff_init(&backoff, &(mye_net_backoff_config){
        .first_delay = 1.0 });

    mye_net_backoff_failed(&backoff);
    for (int i = 0; i < 30; ++i) {
        mye_net_backoff_failed(&backoff);
        ASSERT_FALSE(mye_net_backoff_ready(&backoff, 1.0 / 60.0));
    }
    ASSERT_NEAR(0.5, mye_net_backoff_remaining(&backoff), 1e-9);

    mye_net_backoff_failed(&backoff);
    ASSERT_TRUE(mye_net_backoff_ready(&backoff, 0.5));
}

TEST(connecting_puts_the_ladder_back_on_its_bottom_rung)
{
    mye_net_backoff backoff;
    mye_net_backoff_init(&backoff, &(mye_net_backoff_config){
        .first_delay = 1.0, .factor = 2.0, .max_delay = 60.0 });

    mye_net_backoff_failed(&backoff);
    ASSERT_EQ_INT(2, ticks_until_ready(&backoff, 0.5, 100));
    mye_net_backoff_failed(&backoff);
    ASSERT_EQ_INT(4, ticks_until_ready(&backoff, 0.5, 100)); /* 2 s */

    mye_net_backoff_connected(&backoff);
    ASSERT_EQ_INT(0, backoff.attempts);
    ASSERT_NEAR(0.0, mye_net_backoff_remaining(&backoff), 1e-9);

    /* The next outage starts from one second again, not from four. */
    mye_net_backoff_failed(&backoff);
    ASSERT_EQ_INT(2, ticks_until_ready(&backoff, 0.5, 100));
}

TEST(max_attempts_gives_up_instead_of_retrying_forever)
{
    mye_net_backoff backoff;
    mye_net_backoff_init(&backoff, &(mye_net_backoff_config){
        .first_delay = 1.0, .max_attempts = 2 });

    mye_net_backoff_failed(&backoff);
    ASSERT_EQ_INT(1, ticks_until_ready(&backoff, 1.0, 10));
    ASSERT_FALSE(mye_net_backoff_exhausted(&backoff)); /* one left */

    mye_net_backoff_failed(&backoff);
    ASSERT_EQ_INT(1, ticks_until_ready(&backoff, 2.0, 10));
    ASSERT_TRUE(mye_net_backoff_exhausted(&backoff));
    mye_net_backoff_failed(&backoff);
    ASSERT_NEAR(0.0, mye_net_backoff_remaining(&backoff), 1e-9);
    ASSERT_EQ_INT(-1, ticks_until_ready(&backoff, 10.0, 10));
    ASSERT_EQ_INT(2, backoff.attempts);
}

/* Jitter exists so a hundred clients do not redial in the same millisecond,
 * and it is seeded so a test can still say exactly what happened. */
TEST(jitter_shortens_the_wait_reproducibly)
{
    mye_net_backoff_config config = { .first_delay = 1.0, .factor = 2.0,
                                      .max_delay = 8.0, .jitter = 0.5,
                                      .seed = 12345u };
    mye_net_backoff a, b;
    mye_net_backoff_init(&a, &config);
    mye_net_backoff_init(&b, &config);

    double rung = 1.0;
    bool any_jittered = false;
    for (int i = 0; i < 5; ++i) {
        mye_net_backoff_failed(&a);
        mye_net_backoff_failed(&b);

        double wait = mye_net_backoff_remaining(&a);
        /* Somewhere in [(1 - jitter) * rung, rung], never longer. */
        ASSERT_TRUE(wait <= rung + 1e-9);
        ASSERT_TRUE(wait >= rung * 0.5 - 1e-9);
        if (wait < rung - 1e-9) {
            any_jittered = true;
        }
        /* Same seed, same sequence: reproducible, not merely bounded. */
        ASSERT_NEAR(wait, mye_net_backoff_remaining(&b), 1e-12);

        ASSERT_TRUE(mye_net_backoff_ready(&a, wait));
        ASSERT_TRUE(mye_net_backoff_ready(&b, wait));
        rung = rung * 2.0 < 8.0 ? rung * 2.0 : 8.0;
    }
    ASSERT_TRUE(any_jittered);
}

/* A factor below one would shorten every rung instead of lengthening it --
 * a typo, not a policy, and the kind that only shows up under load. */
TEST(a_nonsense_configuration_is_corrected_not_obeyed)
{
    mye_net_backoff backoff;
    mye_net_backoff_init(&backoff, &(mye_net_backoff_config){
        .first_delay = -3.0, .factor = 0.25, .max_delay = 0.0,
        .jitter = 5.0, .max_attempts = -1 });

    ASSERT_NEAR(0.5, backoff.config.first_delay, 1e-9);
    ASSERT_NEAR(2.0, backoff.config.factor, 1e-9);
    ASSERT_NEAR(8.0, backoff.config.max_delay, 1e-9);
    ASSERT_NEAR(1.0, backoff.config.jitter, 1e-9);
    ASSERT_EQ_INT(0, backoff.config.max_attempts);

    /* And a ceiling below the first delay does not produce a negative wait. */
    mye_net_backoff other;
    mye_net_backoff_init(&other, &(mye_net_backoff_config){
        .first_delay = 10.0, .max_delay = 1.0 });
    mye_net_backoff_failed(&other);
    ASSERT_NEAR(10.0, mye_net_backoff_remaining(&other), 1e-9);
}

TEST(a_null_backoff_is_ignored_rather_than_crashing)
{
    mye_net_backoff_init(NULL, NULL);
    mye_net_backoff_failed(NULL);
    mye_net_backoff_connected(NULL);
    ASSERT_FALSE(mye_net_backoff_ready(NULL, 1.0));
    ASSERT_NEAR(0.0, mye_net_backoff_remaining(NULL), 1e-9);
    ASSERT_TRUE(mye_net_backoff_exhausted(NULL));
}

TEST_MAIN(TEST_CASE(defaults_fill_in_every_zero_field),
          TEST_CASE(a_retry_waits_before_the_first_attempt),
          TEST_CASE(each_failure_waits_longer_up_to_the_ceiling),
          TEST_CASE(repeated_failures_do_not_restart_the_current_wait),
          TEST_CASE(connecting_puts_the_ladder_back_on_its_bottom_rung),
          TEST_CASE(max_attempts_gives_up_instead_of_retrying_forever),
          TEST_CASE(jitter_shortens_the_wait_reproducibly),
          TEST_CASE(a_nonsense_configuration_is_corrected_not_obeyed),
          TEST_CASE(a_null_backoff_is_ignored_rather_than_crashing))
