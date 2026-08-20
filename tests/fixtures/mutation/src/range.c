#include "range.h"

/* Seven mutation sites, deliberately: two condition kills, two relational
 * swaps and three constant flips. The weak suite kills five of them and lets
 * both relational swaps through, because it never probes a boundary; the
 * strong suite kills all seven. That difference is what the runner's
 * self-test asserts. */
int in_range(int value, int low, int high)
{
    if (value < low) {
        return 0;
    }
    if (value > high) {
        return 0;
    }
    return 1;
}
