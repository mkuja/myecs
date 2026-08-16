/* Minimal pure-C test harness. See plan/09-testing.md.
 *
 *   TEST(arena_alignment) {
 *       ASSERT_TRUE(...);
 *       ASSERT_EQ_INT(4, x);
 *   }
 *   TEST_MAIN(TEST_CASE(arena_alignment))
 *
 * A failing assert prints file:line with expected/actual and aborts that test
 * (the rest still run). The process exits non-zero if any test failed, which
 * is what CTest reads.
 */
#ifndef MYE_TEST_H
#define MYE_TEST_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct mye_test_ctx {
    bool failed;
    bool skipped;
} mye_test_ctx;

typedef struct mye_test_entry {
    const char *name;
    void (*fn)(mye_test_ctx *);
} mye_test_entry;

#define TEST(name_) static void mye_test_##name_(mye_test_ctx *T)
#define TEST_CASE(name_) { #name_, mye_test_##name_ }

/* Marks the test skipped and stops it. */
#define SKIP(reason_)                                                          \
    do {                                                                       \
        T->skipped = true;                                                     \
        fprintf(stderr, "  SKIP %s:%d: %s\n", __FILE__, __LINE__, (reason_));   \
        return;                                                                \
    } while (0)

#define MYE_FAIL_(...)                                                         \
    do {                                                                       \
        T->failed = true;                                                      \
        fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);                 \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputc('\n', stderr);                                                   \
        return;                                                                \
    } while (0)

#define ASSERT_TRUE(expr_)                                                     \
    do {                                                                       \
        if (!(expr_)) MYE_FAIL_("expected true: %s", #expr_);                  \
    } while (0)

#define ASSERT_FALSE(expr_)                                                    \
    do {                                                                       \
        if ((expr_)) MYE_FAIL_("expected false: %s", #expr_);                  \
    } while (0)

#define ASSERT_EQ_INT(expected_, actual_)                                      \
    do {                                                                       \
        long long mye_e_ = (long long)(expected_);                             \
        long long mye_a_ = (long long)(actual_);                               \
        if (mye_e_ != mye_a_)                                                  \
            MYE_FAIL_("%s: expected %lld, got %lld", #actual_, mye_e_, mye_a_); \
    } while (0)

#define ASSERT_EQ_U64(expected_, actual_)                                      \
    do {                                                                       \
        uint64_t mye_e_ = (uint64_t)(expected_);                               \
        uint64_t mye_a_ = (uint64_t)(actual_);                                 \
        if (mye_e_ != mye_a_)                                                  \
            MYE_FAIL_("%s: expected %llu, got %llu", #actual_,                 \
                      (unsigned long long)mye_e_, (unsigned long long)mye_a_); \
    } while (0)

#define ASSERT_EQ_PTR(expected_, actual_)                                      \
    do {                                                                       \
        const void *mye_e_ = (const void *)(expected_);                        \
        const void *mye_a_ = (const void *)(actual_);                          \
        if (mye_e_ != mye_a_)                                                  \
            MYE_FAIL_("%s: expected %p, got %p", #actual_, mye_e_, mye_a_);    \
    } while (0)

#define ASSERT_NULL(ptr_)     ASSERT_EQ_PTR(NULL, (ptr_))
#define ASSERT_NOT_NULL(ptr_)                                                  \
    do {                                                                       \
        if ((const void *)(ptr_) == NULL) MYE_FAIL_("%s is NULL", #ptr_);      \
    } while (0)

#define ASSERT_NEAR(expected_, actual_, eps_)                                  \
    do {                                                                       \
        double mye_e_ = (double)(expected_);                                   \
        double mye_a_ = (double)(actual_);                                     \
        if (fabs(mye_e_ - mye_a_) > (double)(eps_))                            \
            MYE_FAIL_("%s: expected %g +/- %g, got %g", #actual_, mye_e_,      \
                      (double)(eps_), mye_a_);                                 \
    } while (0)

#define ASSERT_STR_EQ(expected_, actual_)                                      \
    do {                                                                       \
        const char *mye_e_ = (expected_);                                      \
        const char *mye_a_ = (actual_);                                        \
        if (mye_e_ == NULL || mye_a_ == NULL || strcmp(mye_e_, mye_a_) != 0)   \
            MYE_FAIL_("%s: expected \"%s\", got \"%s\"", #actual_,             \
                      mye_e_ ? mye_e_ : "(null)", mye_a_ ? mye_a_ : "(null)"); \
    } while (0)

static inline int mye_test_run_all(const mye_test_entry *tests, size_t count)
{
    size_t passed = 0, failed = 0, skipped = 0;

    for (size_t i = 0; i < count; ++i) {
        mye_test_ctx ctx = { .failed = false, .skipped = false };
        fprintf(stderr, "RUN  %s\n", tests[i].name);
        tests[i].fn(&ctx);
        if (ctx.failed) {
            ++failed;
            fprintf(stderr, "FAIL %s\n", tests[i].name);
        } else if (ctx.skipped) {
            ++skipped;
        } else {
            ++passed;
        }
    }

    fprintf(stderr, "---- %zu passed, %zu failed, %zu skipped\n", passed,
            failed, skipped);
    return failed == 0 ? 0 : 1;
}

#define TEST_MAIN(...)                                                         \
    int main(void)                                                             \
    {                                                                          \
        static const mye_test_entry mye_tests_[] = { __VA_ARGS__ };            \
        return mye_test_run_all(mye_tests_,                                    \
                                sizeof mye_tests_ / sizeof mye_tests_[0]);     \
    }

#endif /* MYE_TEST_H */
