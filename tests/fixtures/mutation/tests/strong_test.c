#include <stdio.h>

#include "range.h"

static int failures;

static void expect(int actual, int expected, const char *label)
{
    if (actual != expected) {
        printf("FAIL %s: got %d want %d\n", label, actual, expected);
        failures = failures + 1;
    }
}

int main(void)
{
    expect(in_range(5, 0, 10), 1, "interior");
    expect(in_range(-5, 0, 10), 0, "below");
    expect(in_range(50, 0, 10), 0, "above");
    /* The two cases the weak suite is missing: both boundaries are inside the
     * range, so widening either comparison changes an answer. */
    expect(in_range(0, 0, 10), 1, "low boundary");
    expect(in_range(10, 0, 10), 1, "high boundary");
    return failures == 0 ? 0 : 1;
}
