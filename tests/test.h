/* A minimal test framework: TEST() registers a function, CHECK macros fail
 * the test with a message. The runner (runner.c) runs every test in its own
 * process with its own temporary directory, so a crash or a failed check in
 * one test never affects another. */
#ifndef QUIP_TEST_H
#define QUIP_TEST_H

#include <string.h>

typedef void (*test_fn)(void);

void test_register(const char *name, test_fn fn, const char *file);
void test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));
/* Called (newest first) when a test fails, to print extra context such as
 * the screen of a running quip. */
void test_on_fail(void (*fn)(void *), void *ctx);
void test_on_fail_remove(void *ctx);

/* Delete a directory tree made by the test tools. Refuses any path that is
 * not inside a quip-tests-, quip-fuzz- or quip-diff- temporary directory,
 * so a bad path can never delete anything else. */
void test_remove_tree(const char *path);

/* The test's private temporary directory, removed after the test. */
extern const char *test_dir;

#define TEST(name)                                                   \
    static void test_##name(void);                                   \
    __attribute__((constructor)) static void test_reg_##name(void) { \
        test_register(#name, test_##name, __FILE__);                 \
    }                                                                \
    static void test_##name(void)

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) test_fail(__FILE__, __LINE__, "CHECK(%s)", #cond); \
    } while (0)

#define CHECK_INT(a, b)                                                                       \
    do {                                                                                      \
        long long a_ = (long long)(a), b_ = (long long)(b);                                   \
        if (a_ != b_) test_fail(__FILE__, __LINE__, "%s is %lld, expected %lld", #a, a_, b_); \
    } while (0)

void test_check_str(const char *file, int line, const char *expr, const char *got,
                    const char *want);
#define CHECK_STR(got, want) test_check_str(__FILE__, __LINE__, #got, (got), (want))

/* Byte strings that may hold NULs. */
void test_check_mem(const char *file, int line, const char *expr, const char *got, int gotlen,
                    const char *want, int wantlen);
#define CHECK_MEM(got, gotlen, want, wantlen) \
    test_check_mem(__FILE__, __LINE__, #got, (got), (gotlen), (want), (wantlen))

#endif
