#include "test.h"

int g_test_failures = 0;

int test_run_all(const test_case *cases, int count)
{
    int i;
    int failed_cases = 0;

    for (i = 0; i < count; i++) {
        g_test_failures = 0;
        cases[i].fn();
        if (g_test_failures == 0) {
            printf("PASS %s\n", cases[i].name);
        } else {
            printf("FAIL %s (%d assertion failure(s))\n", cases[i].name, g_test_failures);
            failed_cases++;
        }
    }

    printf("---\n%d/%d test cases passed\n", count - failed_cases, count);
    return (failed_cases == 0) ? 0 : 1;
}
