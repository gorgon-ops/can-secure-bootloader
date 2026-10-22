#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long _a = (long)(a), _b = (long)(b);                                 \
        g_checks++;                                                          \
        if (_a != _b) {                                                      \
            g_failures++;                                                    \
            printf("FAIL %s:%d  %s == %s  (%ld != %ld)\n",                   \
                   __FILE__, __LINE__, #a, #b, _a, _b);                      \
        }                                                                    \
    } while (0)

static int test_report(void)
{
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

#endif
