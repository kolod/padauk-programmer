/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal unit-test framework: each test_*.c is one executable registered in CTest. */
#ifndef PDK_TEST_H
#define PDK_TEST_H

#include <stdio.h>
#include <string.h>

typedef void (*pdk_test_fn)(void);

typedef struct {
    const char *name;
    pdk_test_fn fn;
} pdk_test_case;

#define PDK_MAX_TESTS 256

extern pdk_test_case pdk_tests[PDK_MAX_TESTS];
extern int pdk_test_count;
extern int pdk_test_failed;

#define TEST(fn_)                                                     \
    static void fn_(void);                                            \
    __attribute__((constructor)) static void pdk_register_##fn_(void) \
    {                                                                 \
        pdk_tests[pdk_test_count].name = #fn_;                        \
        pdk_tests[pdk_test_count].fn = fn_;                           \
        pdk_test_count++;                                             \
    }                                                                 \
    static void fn_(void)

#define PDK_FAIL(...)                                                      \
    do {                                                                   \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                      \
        printf(__VA_ARGS__);                                               \
        printf("\n");                                                      \
        pdk_test_failed = 1;                                               \
        return;                                                            \
    } while (0)

#define ASSERT_TRUE(cond)                                                  \
    do { if (!(cond)) PDK_FAIL("%s", #cond); } while (0)

#define ASSERT_EQ(a, b)                                                    \
    do {                                                                   \
        long long pdk_a_ = (long long)(a), pdk_b_ = (long long)(b);        \
        if (pdk_a_ != pdk_b_)                                              \
            PDK_FAIL("%s == %s (0x%llx != 0x%llx)", #a, #b,                \
                     (unsigned long long)pdk_a_, (unsigned long long)pdk_b_); \
    } while (0)

#define ASSERT_MEM_EQ(a, b, n)                                             \
    do { if (memcmp((a), (b), (n)) != 0) PDK_FAIL("memory %s != %s", #a, #b); } while (0)

/* Define PDK_TEST_MAIN in exactly one translation unit per test executable. */
#ifdef PDK_TEST_MAIN
pdk_test_case pdk_tests[PDK_MAX_TESTS];
int pdk_test_count;
int pdk_test_failed;

/* Every test file defines pdk_test_setup(): the reset run before each test. */
void pdk_test_setup(void);

int main(void)
{
    int failures = 0;
    for (int i = 0; i < pdk_test_count; i++) {
        pdk_test_failed = 0;
        pdk_test_setup();
        pdk_tests[i].fn();
        printf("%s %s\n", pdk_test_failed ? "FAIL" : "ok  ", pdk_tests[i].name);
        failures += pdk_test_failed;
    }
    printf("%d tests, %d failed\n", pdk_test_count, failures);
    return failures ? 1 : 0;
}
#endif

#endif
