/*
 * Channel lifecycle cost and concurrency.
 *
 * Two jobs, and the second is the one that earns this file a place in both
 * `make check` and `make tsan`:
 *
 *  1. Report what one channel create+destroy costs, sequentially and under
 *     contention. Every create and destroy takes lifecycle_mutex and
 *     channels_mutex, so this is a global serialisation point; the numbers
 *     say how much headroom a per-request-channel transport (HTTP) has.
 *     The figures are printed on every run so a regression is visible in CI
 *     logs as a trend, not just as a pass/fail bit.
 *
 *  2. Exercise concurrent channel creation and destruction against one
 *     runtime - registry list insert/unlink, the create gate, and the
 *     lifecycle locks - which is otherwise only lightly covered.
 *
 * The asserted ceiling is deliberately loose. A tight timing gate in CI is a
 * flakiness source (shared runners, TSan's 5-15x slowdown), and this project
 * has already been bitten once by a timing-sensitive test failing a release
 * build. The ceiling therefore only catches a catastrophic regression - a
 * thread spawn or a syscall introduced on the per-channel path, which would
 * cost tens of microseconds - while the printed numbers carry the real
 * signal.
 */
#include "maelys/mcp.h"
#include "tests/test_support.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define OPERATIONS_PER_ROW 8000
#define MAX_WORKERS 16
/* See the header comment: loose on purpose, and far above any plausible
 * loaded-CI or TSan-instrumented figure. */
#define CEILING_US 500.0

typedef struct worker_args {
    maelys_mcp_runtime_t *runtime;
    int iterations;
    int failed;
} worker_args_t;

static void *worker_main(void *argument) {
    worker_args_t *args = argument;
    for (int index = 0; index < args->iterations; ++index) {
        maelys_mcp_channel_t *channel = NULL;
        if (maelys_mcp_channel_create(args->runtime, NULL, &channel) != MAELYS_MCP_OK) {
            args->failed = 1;
            return NULL;
        }
        if (maelys_mcp_channel_destroy(channel) != MAELYS_MCP_OK) {
            args->failed = 1;
            return NULL;
        }
    }
    return NULL;
}

/* Returns microseconds per operation, or a negative value on failure. */
static double measure(int workers, int total_operations) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "channel-perf", .server_version = "1"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&config, &runtime) != MAELYS_MCP_OK) return -1.0;

    pthread_t handles[MAX_WORKERS];
    worker_args_t args[MAX_WORKERS];
    int per_worker = total_operations / workers;
    int started = 0;

    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return -2.0;
    for (int index = 0; index < workers; ++index) {
        args[index] = (worker_args_t){
            .runtime = runtime, .iterations = per_worker, .failed = 0
        };
        if (pthread_create(&handles[index], NULL, worker_main, &args[index]) != 0) break;
        ++started;
    }
    for (int index = 0; index < started; ++index) pthread_join(handles[index], NULL);
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) return -3.0;
    if (started != workers) return -4.0;

    for (int index = 0; index < workers; ++index) {
        if (args[index].failed) return -5.0;
    }
    if (maelys_mcp_runtime_destroy(runtime) != MAELYS_MCP_OK) return -6.0;

    double microseconds = ((double)(end.tv_sec - start.tv_sec) * 1e9 +
        (double)(end.tv_nsec - start.tv_nsec)) / 1000.0;
    return microseconds / (double)(per_worker * workers);
}

static int test_channel_lifecycle_cost_and_concurrency(void) {
    static const int worker_counts[] = {1, 2, 4, 8, 16};
    fprintf(stderr, "   channel create+destroy (%d ops per row)\n", OPERATIONS_PER_ROW);
    for (size_t index = 0;
         index < sizeof(worker_counts) / sizeof(worker_counts[0]); ++index) {
        double per_operation = measure(worker_counts[index], OPERATIONS_PER_ROW);
        ASSERT_TRUE(per_operation >= 0.0);
        fprintf(stderr, "     %2d thread(s): %8.2f us/op  %10.0f ops/s\n",
            worker_counts[index], per_operation, 1e6 / per_operation);
        ASSERT_TRUE(per_operation < CEILING_US);
    }
    return 0;
}

int main(void) {
    static const maelys_test_case_t tests[] = {
        {"channel lifecycle cost and concurrency",
            test_channel_lifecycle_cost_and_concurrency}
    };
    return maelys_run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
