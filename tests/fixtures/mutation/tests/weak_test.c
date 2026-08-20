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
    /* Deliberately weak: interior points only. Nothing here ever probes a
     * boundary, so widening either comparison is invisible to it. */
    expect(in_range(5, 0, 10), 1, "interior");
    expect(in_range(-5, 0, 10), 0, "below");
    expect(in_range(50, 0, 10), 0, "above");
    return failures == 0 ? 0 : 1;
}
