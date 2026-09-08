/* Minimal host test harness — no external dependency, one header.
 *
 *   #include "test.h"
 *   TEST(does_a_thing) { CHECK(1 + 1 == 2); CHECK_EQ(crc, 0x29B1u); }
 *   int main(void) { RUN(does_a_thing); return test_summary(); }
 *
 * A test is a void(void) function; CHECK / CHECK_EQ record a failure and
 * keep going (so one run reports every broken case, not just the first).
 * test_summary() prints the tally and returns non-zero if anything failed
 * — use it as main()'s return value so `make` / CI sees the exit code.
 */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

static int  test_checks  = 0;
static int  test_fails   = 0;
static const char *test_current = "";

#define TEST(name)  static void name(void)

#define RUN(name)   do {                                    \
        test_current = #name;                               \
        int before = test_fails;                            \
        name();                                             \
        printf("  %-40s %s\n", #name,                       \
               (test_fails == before) ? "ok" : "FAIL");     \
    } while (0)

#define CHECK(cond)  do {                                             \
        test_checks++;                                                \
        if (!(cond)) {                                                \
            test_fails++;                                             \
            printf("    %s:%d  CHECK(%s) failed  [%s]\n",              \
                   __FILE__, __LINE__, #cond, test_current);          \
        }                                                             \
    } while (0)

/* Integer equality with a value dump on mismatch. */
#define CHECK_EQ(got, want)  do {                                     \
        test_checks++;                                                \
        long long _g = (long long)(got), _w = (long long)(want);      \
        if (_g != _w) {                                               \
            test_fails++;                                             \
            printf("    %s:%d  %s: got %lld (0x%llx), want %lld (0x%llx)  [%s]\n", \
                   __FILE__, __LINE__, #got, _g,                      \
                   (unsigned long long)_g, _w,                        \
                   (unsigned long long)_w, test_current);             \
        }                                                             \
    } while (0)

static int test_summary(void)
{
    printf("\n%d checks, %d failed\n", test_checks, test_fails);
    return test_fails != 0;
}

#endif /* TEST_H */
