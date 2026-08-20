#ifndef MUTATION_FIXTURE_RANGE_H
#define MUTATION_FIXTURE_RANGE_H

struct pair {
    int left;
    int right;
};

int in_range(int value, int low, int high);
int skip_operator_shapes(struct pair *p, int a, int b);
int guard_value(void);
const char *skip_strings(void);
char skip_char(void);
int skip_real_code(int n);

#endif
