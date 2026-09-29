/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_TEST_H
#define WP_TEST_H

#include <stdio.h>
#include <string.h>

/* Minimal, hand-rolled C89 test harness -- no external dependencies, no
   constructor-attribute registration magic. Every test function is listed
   explicitly in main.c's static case array; adding a suite means adding a
   .c file and adding its function names to that array. */

typedef void (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn     fn;
} test_case;

/* Reset to 0 by test_run_all before each case; incremented by the
   ASSERT_* macros on failure. Test code should never read this directly. */
extern int g_test_failures;

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_test_failures++; \
            return; \
        } \
    } while (0)

#define ASSERT_EQ_U32(a, b) \
    do { \
        unsigned long _wp_a = (unsigned long)(a); \
        unsigned long _wp_b = (unsigned long)(b); \
        if (_wp_a != _wp_b) { \
            printf("  FAIL %s:%d: %s (%lu) != %s (%lu)\n", __FILE__, __LINE__, \
                   #a, _wp_a, #b, _wp_b); \
            g_test_failures++; \
            return; \
        } \
    } while (0)

#define ASSERT_EQ_STR(a, b) \
    do { \
        const char *_wp_a = (a); \
        const char *_wp_b = (b); \
        if (strcmp(_wp_a, _wp_b) != 0) { \
            printf("  FAIL %s:%d: %s (\"%s\") != %s (\"%s\")\n", __FILE__, __LINE__, \
                   #a, _wp_a, #b, _wp_b); \
            g_test_failures++; \
            return; \
        } \
    } while (0)

#define ASSERT_MEM_EQ(a, b, n) \
    do { \
        if (memcmp((a), (b), (size_t)(n)) != 0) { \
            printf("  FAIL %s:%d: %s != %s (%lu bytes)\n", __FILE__, __LINE__, \
                   #a, #b, (unsigned long)(n)); \
            g_test_failures++; \
            return; \
        } \
    } while (0)

/* Runs every case in order, printing PASS/FAIL per case and a summary
   line. Returns 0 if every case passed, 1 otherwise -- suitable to use
   directly as the process exit code. */
int test_run_all(const test_case *cases, int count);

#endif /* WP_TEST_H */
