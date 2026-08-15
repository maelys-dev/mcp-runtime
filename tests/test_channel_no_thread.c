#include "maelys/mcp.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef __APPLE__
#include <mach/mach.h>

static int thread_count(mach_msg_type_number_t *out_count) {
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0u;
    kern_return_t status = task_threads(mach_task_self(), &threads, &count);
    if (status != KERN_SUCCESS) return 0;
    for (mach_msg_type_number_t index = 0u; index < count; ++index) {
        (void)mach_port_deallocate(mach_task_self(), threads[index]);
    }
    vm_size_t bytes = (vm_size_t)count * sizeof(*threads);
    if (bytes != 0u) {
        (void)vm_deallocate(mach_task_self(), (vm_address_t)threads, bytes);
    }
    *out_count = count;
    return 1;
}
#else
#include <pthread.h>
#include <stdatomic.h>

static atomic_uint wrapped_thread_creations;

int __real_pthread_create(
    pthread_t *thread,
    const pthread_attr_t *attributes,
    void *(*start)(void *),
    void *argument);

int __wrap_pthread_create(
    pthread_t *thread,
    const pthread_attr_t *attributes,
    void *(*start)(void *),
    void *argument) {
    atomic_fetch_add(&wrapped_thread_creations, 1u);
    return __real_pthread_create(thread, attributes, start, argument);
}
#endif

int main(void) {
    maelys_mcp_runtime_config_t runtime_config = {
        .server_name = "no-thread-test",
        .server_version = "0.10.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    if (maelys_mcp_runtime_create(&runtime_config, &runtime) != MAELYS_MCP_OK) {
        return 1;
    }
#ifdef __APPLE__
    mach_msg_type_number_t before = 0u;
    mach_msg_type_number_t after = 0u;
    if (!thread_count(&before)) return 2;
#else
    atomic_init(&wrapped_thread_creations, 0u);
#endif
    enum { CHANNEL_COUNT = 256 };
    maelys_mcp_channel_t *channels[CHANNEL_COUNT] = {0};
    for (size_t index = 0; index < CHANNEL_COUNT; ++index) {
        if (maelys_mcp_channel_create(runtime, NULL, &channels[index]) !=
            MAELYS_MCP_OK) {
            return 3;
        }
    }
#ifdef __APPLE__
    if (!thread_count(&after)) return 4;
    if (after != before) {
        fprintf(stderr, "thread count changed: before=%u after=%u\n",
            before, after);
        return 5;
    }
#else
    unsigned int created = atomic_load(&wrapped_thread_creations);
    if (created != 0u) {
        fprintf(stderr, "pthread_create calls while creating channels: %u\n", created);
        return 5;
    }
#endif
    for (size_t index = 0; index < CHANNEL_COUNT; ++index) {
        if (maelys_mcp_channel_destroy(channels[index]) != MAELYS_MCP_OK) return 6;
    }
    if (maelys_mcp_runtime_destroy(runtime) != MAELYS_MCP_OK) return 7;
    puts("test_channel_no_thread: OK");
    return 0;
}
