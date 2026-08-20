#include "range.h"

/* NOMUTATE-COMMENT: if (a == 0) && 1 || 0 - none of this is code. */
/* NOMUTATE-COMMENT: a second line of the same block comment,
 * with if (b != 1) on it too. */
// NOMUTATE-COMMENT: if (c <= 0) || 1

#define NOMUTATE_PREPROC_ONE 1
#if 0
int nomutate_dead_region = 1;
#endif
#define NOMUTATE_PREPROC_CMP(a, b) ((a) == (b))

const char *skip_strings(void)
{
    return "NOMUTATE-STRING if (a == 0) && 1 || 0";
}

char skip_char(void)
{
    return '0';
}

int skip_real_code(int n)
{
    if (n == 0 && n >= 0) {
        return 1;
    }
    return 0;
}

/* Operator shapes that look like relational operators but are not: the two
 * shifts, the compound shift-assign and the arrow. A scanner that rejects one
 * of these without advancing past it spins forever, so the pinned site
 * inventory covering this function is also the regression test for that. */
int skip_operator_shapes(struct pair *p, int a, int b)
{
    int v = a << 1;
    v >>= 1;
    return v + (p->left <= b) + (p->right >> 1);
}
