#include "range.h"

/* Exactly one mutation site, and it can never build: flipping the literal
 * turns the static assertion false, which every C11 compiler rejects. That
 * makes this fixture a deterministic *stillborn*, which the runner must count
 * separately from a kill - a mutant the compiler refused proves nothing about
 * the test suite. */
_Static_assert(1, "the runner must call a build failure stillborn");

int guard_value(void)
{
    return 42;
}
