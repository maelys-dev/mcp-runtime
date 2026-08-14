#pragma once

#include <stddef.h>
#include <stdio.h>

typedef int (*maelys_test_fn)(void);

typedef struct maelys_test_case {
    const char *name;
    maelys_test_fn run;
} maelys_test_case_t;

#define ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static inline int maelys_run_tests(
    const maelys_test_case_t *cases,
    size_t count) {
    int failures = 0;
    for (size_t index = 0; index < count; ++index) {
        int status = cases[index].run();
        fprintf(stderr, "%s %s\n", status == 0 ? "ok" : "not ok", cases[index].name);
        if (status != 0) ++failures;
    }
    return failures == 0 ? 0 : 1;
}
