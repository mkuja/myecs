/* Smoke test for the test harness itself: every assert macro must accept a
 * passing case without tripping. Failure paths are exercised by hand (a
 * self-testing failure path would have to fail the suite to prove itself). */
#include "mye_test.h"

TEST(assert_bool)
{
    ASSERT_TRUE(1 == 1);
    ASSERT_FALSE(1 == 2);
}

TEST(assert_numbers)
{
    ASSERT_EQ_INT(-7, -7);
    ASSERT_EQ_U64(UINT64_C(1) << 40, UINT64_C(1) << 40);
    ASSERT_NEAR(1.0, 1.0 + 1e-9, 1e-6);
}

TEST(assert_pointers)
{
    int x = 0;
    int *p = &x;
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_PTR(&x, p);
    ASSERT_NULL(NULL);
}

TEST(assert_strings)
{
    ASSERT_STR_EQ("myecs", "myecs");
}

TEST_MAIN(TEST_CASE(assert_bool), TEST_CASE(assert_numbers),
          TEST_CASE(assert_pointers), TEST_CASE(assert_strings))
