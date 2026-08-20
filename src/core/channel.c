#include "src/internal/internal.h"
#include "maelys/mcp/subscriptions.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static maelys_mcp_result_t initialize_channel(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_channel_config_t *config,
    maelys_mcp_channel_t **out_channel) {
    maelys_mcp_result_t status = MAELYS_MCP_OK;
    maelys_mcp_channel_t *channel = calloc(1u, sizeof(*channel));
    if (!channel) return MAELYS_MCP_ERR_MEMORY;
    channel->runtime = runtime;
    if (config) channel->config = *config;
    if (!channel->config.max_messages) channel->config.max_messages = 256u;
    if (!channel->config.max_bytes) {
        channel->config.max_bytes = runtime->max_message_bytes > SIZE_MAX / 4u ?
            SIZE_MAX : runtime->max_message_bytes * 4u;
    }
    if (!channel->config.response_burst) channel->config.response_burst = 8u;
    if (!channel->config.admission_timeout_ms) {
        channel->config.admission_timeout_ms = 5000u;
    }
    if (!channel->config.close_timeout_ms) channel->config.close_timeout_ms = 5000u;
    channel->subscriptions = calloc(runtime->max_subscriptions,
        sizeof(*channel->subscriptions));
    if (!channel->subscriptions) goto failed;
    if (pthread_mutex_init(&channel->mutex, NULL) != 0) goto failed;
    channel->mutex_initialized = 1;
    if (maelys_mcp_cond_init_monotonic(&channel->idle) != 0) goto failed;
    channel->idle_initialized = 1;
    maelys_mcp_outbox_config_t outbox_config = {
        .max_messages = channel->config.max_messages,
        .max_bytes = channel->config.max_bytes,
        .response_burst = channel->config.response_burst,
        .admission_timeout_ms = channel->config.admission_timeout_ms
    };
    status = maelys_mcp_outbox_create(&outbox_config, &channel->outbox);
    if (status != MAELYS_MCP_OK) goto failed;
    channel->state = MAELYS_MCP_CHANNEL_STARTING;
    *out_channel = channel;
    return MAELYS_MCP_OK;

failed:
    if (channel->outbox) {
        (void)maelys_mcp_outbox_close(channel->outbox, 1);
        (void)maelys_mcp_outbox_destroy(channel->outbox);
    }
    if (channel->idle_initialized) pthread_cond_destroy(&channel->idle);
    if (channel->mutex_initialized) pthread_mutex_destroy(&channel->mutex);
    free(channel->subscriptions);
    free(channel);
    return status == MAELYS_MCP_OK ? MAELYS_MCP_ERR_IO : status;
}

static void free_unpublished_channel(maelys_mcp_channel_t *channel) {
    if (!channel) return;
    (void)maelys_mcp_outbox_close(channel->outbox, 1);
    (void)maelys_mcp_outbox_destroy(channel->outbox);
    pthread_cond_destroy(&channel->idle);
    pthread_mutex_destroy(&channel->mutex);
    free(channel->subscriptions);
    free(channel);
}

static maelys_mcp_result_t activate_providers(maelys_mcp_runtime_t *runtime) {
    for (size_t index = 0; index < runtime->provider_count; ++index) {
        maelys_mcp_provider_t *provider = runtime->providers[index];
        if (!provider->activate || provider->activated) continue;
        char *error = NULL;
        maelys_mcp_result_t status = provider->activate(provider->context, &error);
        free(error);
        if (status != MAELYS_MCP_OK) return status;
        provider->activated = 1;
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t begin_channel_create(maelys_mcp_runtime_t *runtime) {
    /* The first runtime access admits this creator or observes the closed gate. */
    pthread_mutex_lock(&runtime->channel_create_gate_mutex);
    if (!runtime->channel_create_accepting) {
        pthread_mutex_unlock(&runtime->channel_create_gate_mutex);
        return MAELYS_MCP_ERR_STATE;
    }
    runtime->channel_creators_admitted++;
    pthread_mutex_unlock(&runtime->channel_create_gate_mutex);
    return MAELYS_MCP_OK;
}

static void end_channel_create(maelys_mcp_runtime_t *runtime) {
    pthread_mutex_lock(&runtime->channel_create_gate_mutex);
    assert(runtime->channel_creators_admitted != 0u);
    runtime->channel_creators_admitted--;
    if (runtime->channel_creators_admitted == 0u) {
        pthread_cond_broadcast(&runtime->channel_create_gate_drained);
    }
    pthread_mutex_unlock(&runtime->channel_create_gate_mutex);
}

static maelys_mcp_result_t publish_channel(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_channel_t *channel) {
    pthread_mutex_lock(&runtime->lifecycle_mutex);
    while (runtime->lifecycle == MAELYS_MCP_RUNTIME_ACTIVATING) {
        pthread_cond_wait(&runtime->lifecycle_changed, &runtime->lifecycle_mutex);
    }
    if (runtime->shutdown_requested ||
        runtime->lifecycle == MAELYS_MCP_RUNTIME_SHUTTING_DOWN) {
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        return MAELYS_MCP_ERR_STATE;
    }
    if (runtime->lifecycle == MAELYS_MCP_RUNTIME_FAULTED) {
        maelys_mcp_result_t status = runtime->activation_status;
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        return status == MAELYS_MCP_OK ? MAELYS_MCP_ERR_STATE : status;
    }
    if (runtime->lifecycle == MAELYS_MCP_RUNTIME_COLD) {
        runtime->lifecycle = MAELYS_MCP_RUNTIME_ACTIVATING;
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        maelys_mcp_result_t activation = activate_providers(runtime);
        pthread_mutex_lock(&runtime->lifecycle_mutex);
        runtime->activation_status = activation;
        if (runtime->shutdown_requested) {
            runtime->lifecycle = MAELYS_MCP_RUNTIME_SHUTTING_DOWN;
            pthread_cond_broadcast(&runtime->lifecycle_changed);
            pthread_mutex_unlock(&runtime->lifecycle_mutex);
            return MAELYS_MCP_ERR_STATE;
        }
        if (activation != MAELYS_MCP_OK) {
            runtime->lifecycle = MAELYS_MCP_RUNTIME_FAULTED;
            pthread_cond_broadcast(&runtime->lifecycle_changed);
            pthread_mutex_unlock(&runtime->lifecycle_mutex);
            return activation;
        }
        runtime->lifecycle = MAELYS_MCP_RUNTIME_ACTIVE;
        pthread_cond_broadcast(&runtime->lifecycle_changed);
    }
    if (runtime->shutdown_requested ||
        runtime->lifecycle != MAELYS_MCP_RUNTIME_ACTIVE) {
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        return MAELYS_MCP_ERR_STATE;
    }
    pthread_mutex_lock(&runtime->channels_mutex);
    pthread_mutex_lock(&channel->mutex);
    channel->state = MAELYS_MCP_CHANNEL_ACTIVE;
    channel->targetable = 1;
    pthread_mutex_unlock(&channel->mutex);
    channel->next = runtime->channels;
    runtime->channels = channel;
    runtime->live_channel_count++;
    pthread_mutex_unlock(&runtime->channels_mutex);
    pthread_mutex_unlock(&runtime->lifecycle_mutex);

    pthread_mutex_lock(&runtime->provider_events_mutex);
    runtime->provider_events_accepting = 1;
    pthread_mutex_unlock(&runtime->provider_events_mutex);
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_channel_create(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_channel_config_t *config,
    maelys_mcp_channel_t **out_channel) {
    if (!runtime || !out_channel) return MAELYS_MCP_ERR_ARGUMENT;
    *out_channel = NULL;
    maelys_mcp_result_t status = begin_channel_create(runtime);
    if (status != MAELYS_MCP_OK) return status;
    maelys_mcp_channel_t *channel = NULL;
    status = initialize_channel(runtime, config, &channel);
    if (status == MAELYS_MCP_OK) {
        status = publish_channel(runtime, channel);
        if (status != MAELYS_MCP_OK) free_unpublished_channel(channel);
    }
    if (status == MAELYS_MCP_OK) *out_channel = channel;
    end_channel_create(runtime);
    return status;
}

static maelys_mcp_result_t begin_operation(maelys_mcp_channel_t *channel) {
    pthread_mutex_lock(&channel->mutex);
    if (channel->state != MAELYS_MCP_CHANNEL_ACTIVE) {
        pthread_mutex_unlock(&channel->mutex);
        return channel->state == MAELYS_MCP_CHANNEL_CLOSED ?
            MAELYS_MCP_ERR_CLOSED : MAELYS_MCP_ERR_STATE;
    }
    channel->operations_inflight++;
    pthread_mutex_unlock(&channel->mutex);
    return MAELYS_MCP_OK;
}

void maelys_mcp_channel_release_operation(maelys_mcp_channel_t *channel) {
    if (!channel) return;
    pthread_mutex_lock(&channel->mutex);
    if (channel->operations_inflight) channel->operations_inflight--;
    if (!channel->operations_inflight) pthread_cond_broadcast(&channel->idle);
    pthread_mutex_unlock(&channel->mutex);
}

static void fault_channel(maelys_mcp_channel_t *channel) {
    maelys_mcp_runtime_t *runtime = channel->runtime;
    pthread_mutex_lock(&runtime->channels_mutex);
    pthread_mutex_lock(&channel->mutex);
    if (channel->state != MAELYS_MCP_CHANNEL_CLOSED) {
        channel->state = MAELYS_MCP_CHANNEL_FAULTED;
        channel->targetable = 0;
    }
    pthread_mutex_unlock(&channel->mutex);
    pthread_mutex_unlock(&runtime->channels_mutex);
    (void)maelys_mcp_outbox_close(channel->outbox, 0);
}

void maelys_mcp_channel_abort(maelys_mcp_channel_t *channel) {
    if (!channel) return;
    fault_channel(channel);
    (void)maelys_mcp_outbox_close(channel->outbox, 1);
}

maelys_mcp_result_t maelys_mcp_channel_enqueue_take(
    maelys_mcp_channel_t *channel,
    json_t *message,
    maelys_mcp_outbox_class_t message_class,
    const char *coalesce_key,
    int fault_on_timeout) {
    if (!channel || !message) return MAELYS_MCP_ERR_ARGUMENT;
    maelys_mcp_result_t status = maelys_mcp_outbox_enqueue_take(
        channel->outbox, message, message_class, coalesce_key);
    if (fault_on_timeout &&
        (status == MAELYS_MCP_ERR_TIMEOUT || status == MAELYS_MCP_ERR_CLOSED)) {
        fault_channel(channel);
    }
    return status;
}

maelys_mcp_result_t maelys_mcp_channel_enqueue_take_until(
    maelys_mcp_channel_t *channel,
    json_t *message,
    maelys_mcp_outbox_class_t message_class,
    const char *coalesce_key,
    int fault_on_timeout,
    uint64_t deadline_ms) {
    if (!channel || !message) return MAELYS_MCP_ERR_ARGUMENT;
    maelys_mcp_result_t status = maelys_mcp_outbox_enqueue_take_until(
        channel->outbox, message, message_class, coalesce_key, deadline_ms);
    if (fault_on_timeout &&
        (status == MAELYS_MCP_ERR_TIMEOUT || status == MAELYS_MCP_ERR_CLOSED)) {
        fault_channel(channel);
    }
    return status;
}

static maelys_mcp_result_t outbox_sink_emit(void *context, json_t *message) {
    /*
     * RESPONSE, not NOTIFICATION, despite this being a JSON-RPC notification:
     * the outbox classes are scheduling classes, not message shapes.
     * NOTIFICATION is the coalescible lane for fanout that relates to no
     * particular request, and select_next deliberately prefers responses over
     * it - which would let a request's final response overtake the
     * request-scoped notifications that must precede it. Over SSE that is not
     * merely out of order: the final response terminates the stream, so those
     * notifications would be dropped outright. The response lane preserves
     * FIFO order with the response they belong to, and forbids coalescing,
     * which is exactly what request-scoped output needs.
     */
    return maelys_mcp_channel_enqueue_take((maelys_mcp_channel_t *)context,
        message, MAELYS_MCP_OUTBOX_RESPONSE, NULL, 1);
}

static maelys_mcp_result_t outbox_sink_complete(void *context, json_t *response) {
    return maelys_mcp_channel_enqueue_take((maelys_mcp_channel_t *)context,
        response, MAELYS_MCP_OUTBOX_RESPONSE, NULL, 1);
}

static int outbox_sink_cancelled(void *context) {
    maelys_mcp_channel_t *channel = context;
    pthread_mutex_lock(&channel->mutex);
    int cancelled = channel->state != MAELYS_MCP_CHANNEL_ACTIVE;
    pthread_mutex_unlock(&channel->mutex);
    return cancelled;
}

maelys_mcp_response_sink_t maelys_mcp_channel_outbox_sink(
    maelys_mcp_channel_t *channel) {
    maelys_mcp_response_sink_t sink = {
        .emit = outbox_sink_emit,
        .complete = outbox_sink_complete,
        .cancelled = outbox_sink_cancelled,
        .context = channel
    };
    return sink;
}

/*
 * The response a request gets when hook 6 fails, or when a wrapper accepted
 * the final response and never passed it on. Sent through the real sink, past
 * the wrappers: the chain has just demonstrated it cannot be trusted with this
 * request's output, and a client waiting forever is a worse failure than a
 * blunt internal error.
 */
static maelys_mcp_result_t answer_past_the_chain(
    const maelys_mcp_response_sink_t *base,
    json_t *id,
    const char *message) {
    json_t *response = maelys_mcp_error_response(id, JSONRPC_INTERNAL_ERROR,
        message, NULL);
    if (!response) return MAELYS_MCP_ERR_MEMORY;
    maelys_mcp_result_t status = base->complete(base->context, response);
    if (status != MAELYS_MCP_OK) json_decref(response);
    return status;
}

maelys_mcp_result_t maelys_mcp_channel_handle_with_sink(
    maelys_mcp_channel_t *channel,
    json_t *request,
    const maelys_mcp_response_sink_t *sink) {
    if (!channel || !request || !sink || !sink->complete) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_result_t status = begin_operation(channel);
    if (status != MAELYS_MCP_OK) return status;
    /*
     * Hook 6 is established before dispatch and torn down after it, so every
     * frame this request produces - the request-scoped notifications a module
     * emits while it runs, and the single response that follows them - travels
     * the same decorated path, in that order. The runtime never completes
     * before dispatch has returned, which is what makes "progress ahead of the
     * response" survive wrapping rather than depend on it.
     */
    maelys_mcp_sink_chain_t chain;
    const maelys_mcp_response_sink_t *effective = sink;
    int wrapped = maelys_mcp_chain_has_wrap_sink(channel->runtime);
    if (wrapped) {
        maelys_mcp_wrap_sink_context_t wrap_request = {
            .channel = channel,
            .request = request
        };
        maelys_mcp_result_t wrap_status = maelys_mcp_chain_wrap_sink(
            channel->runtime, &wrap_request, sink, &chain, &effective);
        if (wrap_status != MAELYS_MCP_OK) {
            json_t *id = json_is_object(request) ?
                json_object_get(request, "id") : NULL;
            status = id ? answer_past_the_chain(sink, id,
                "Response sink wrapping failed") : MAELYS_MCP_OK;
            maelys_mcp_channel_release_operation(channel);
            return status;
        }
    }
    json_t *activate_id = NULL;
    json_t *response = maelys_mcp_runtime_dispatch(channel, request, effective,
        &activate_id);
    if (response) {
        /*
         * Held across the completion on purpose: on success the response is no
         * longer ours, and a wrapper that swallowed it may already have
         * released it, so the id has to survive independently of both.
         */
        json_t *id = json_incref(json_object_get(response, "id"));
        status = effective->complete(effective->context, response);
        if (status != MAELYS_MCP_OK) {
            json_decref(response);
        } else if (wrapped && maelys_mcp_chain_sink_completions(&chain) == 0u) {
            /*
             * A wrapper reported success and delivered nothing. Left alone
             * that wedges this request id for the life of the connection, so
             * the runtime answers it itself rather than trusting the contract
             * it just watched being broken.
             */
            status = answer_past_the_chain(sink, id,
                "Response was not delivered");
        }
        json_decref(id);
    }
    if (wrapped) maelys_mcp_chain_release_sink(&chain);
    if (activate_id) {
        if (status == MAELYS_MCP_OK) {
            maelys_mcp_activate_subscription(channel, activate_id);
        } else {
            maelys_mcp_cancel_subscription(channel, activate_id);
        }
        json_decref(activate_id);
    }
    maelys_mcp_channel_release_operation(channel);
    return status;
}

maelys_mcp_result_t maelys_mcp_channel_handle(
    maelys_mcp_channel_t *channel,
    json_t *request) {
    if (!channel || !request) return MAELYS_MCP_ERR_ARGUMENT;
    maelys_mcp_response_sink_t sink = maelys_mcp_channel_outbox_sink(channel);
    return maelys_mcp_channel_handle_with_sink(channel, request, &sink);
}

void *maelys_mcp_channel_context(const maelys_mcp_channel_t *channel) {
    return channel ? channel->config.context : NULL;
}

maelys_mcp_result_t maelys_mcp_channel_next(
    maelys_mcp_channel_t *channel,
    unsigned int timeout_ms,
    json_t **out_message) {
    if (!channel) return MAELYS_MCP_ERR_ARGUMENT;
    return maelys_mcp_outbox_next(channel->outbox, timeout_ms, out_message);
}

maelys_mcp_result_t maelys_mcp_runtime_snapshot_channels(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_channel_t ***out_channels,
    size_t *out_count) {
    if (!runtime || !out_channels || !out_count) return MAELYS_MCP_ERR_ARGUMENT;
    *out_channels = NULL;
    *out_count = 0u;
    for (;;) {
        pthread_mutex_lock(&runtime->channels_mutex);
        size_t capacity = runtime->live_channel_count;
        pthread_mutex_unlock(&runtime->channels_mutex);
        maelys_mcp_channel_t **channels = capacity ?
            calloc(capacity, sizeof(*channels)) : NULL;
        if (capacity && !channels) return MAELYS_MCP_ERR_MEMORY;
        pthread_mutex_lock(&runtime->channels_mutex);
        if (runtime->live_channel_count > capacity) {
            pthread_mutex_unlock(&runtime->channels_mutex);
            free(channels);
            continue;
        }
        size_t count = 0u;
        int inconsistent_registry = 0;
        for (maelys_mcp_channel_t *channel = runtime->channels;
             channel; channel = channel->next) {
            pthread_mutex_lock(&channel->mutex);
            if (channel->targetable &&
                channel->state == MAELYS_MCP_CHANNEL_ACTIVE) {
                if (!channels || count >= capacity) {
                    inconsistent_registry = 1;
                } else {
                    channel->operations_inflight++;
                    channels[count++] = channel;
                }
            }
            pthread_mutex_unlock(&channel->mutex);
        }
        pthread_mutex_unlock(&runtime->channels_mutex);
        if (inconsistent_registry) {
            for (size_t index = 0; index < count; ++index) {
                maelys_mcp_channel_release_operation(channels[index]);
            }
            free(channels);
            return MAELYS_MCP_ERR_STATE;
        }
        if (!count) {
            free(channels);
            return MAELYS_MCP_ERR_NOT_FOUND;
        }
        *out_channels = channels;
        *out_count = count;
        return MAELYS_MCP_OK;
    }
}

maelys_mcp_result_t maelys_mcp_channel_close(
    maelys_mcp_channel_t *channel,
    unsigned int timeout_ms) {
    if (!channel) return MAELYS_MCP_ERR_ARGUMENT;
    if (!timeout_ms) timeout_ms = channel->config.close_timeout_ms;
    maelys_mcp_runtime_t *runtime = channel->runtime;
    int was_faulted = 0;
    pthread_mutex_lock(&runtime->channels_mutex);
    pthread_mutex_lock(&channel->mutex);
    if (channel->state == MAELYS_MCP_CHANNEL_CLOSED) {
        pthread_mutex_unlock(&channel->mutex);
        pthread_mutex_unlock(&runtime->channels_mutex);
        return MAELYS_MCP_OK;
    }
    was_faulted = channel->state == MAELYS_MCP_CHANNEL_FAULTED;
    channel->targetable = 0;
    if (!was_faulted) channel->state = MAELYS_MCP_CHANNEL_CLOSING;
    pthread_mutex_unlock(&runtime->channels_mutex);
    uint64_t deadline_ms = 0u;
    if (maelys_mcp_monotonic_deadline(timeout_ms, &deadline_ms) != 0) {
        pthread_mutex_unlock(&channel->mutex);
        return MAELYS_MCP_ERR_IO;
    }
    while (channel->operations_inflight) {
        int waited = maelys_mcp_cond_wait_until(
            &channel->idle, &channel->mutex, deadline_ms);
        if (waited == ETIMEDOUT) {
            pthread_mutex_unlock(&channel->mutex);
            return MAELYS_MCP_ERR_TIMEOUT;
        }
        if (waited != 0) {
            pthread_mutex_unlock(&channel->mutex);
            return MAELYS_MCP_ERR_IO;
        }
    }
    pthread_mutex_unlock(&channel->mutex);
    maelys_mcp_result_t status = MAELYS_MCP_OK;
    if (!was_faulted) {
        status = maelys_mcp_channel_complete_subscriptions_until(
            channel, deadline_ms);
    }
    maelys_mcp_result_t closed = maelys_mcp_outbox_close(channel->outbox, 0);
    maelys_mcp_result_t drained = closed == MAELYS_MCP_OK ?
        maelys_mcp_outbox_wait_drained_until(channel->outbox, deadline_ms) : closed;
    if (status != MAELYS_MCP_OK) return status;
    if (drained != MAELYS_MCP_OK) return drained;
    pthread_mutex_lock(&channel->mutex);
    channel->state = MAELYS_MCP_CHANNEL_CLOSED;
    pthread_cond_broadcast(&channel->idle);
    pthread_mutex_unlock(&channel->mutex);
    return MAELYS_MCP_OK;
}

maelys_mcp_result_t maelys_mcp_channel_destroy(maelys_mcp_channel_t *channel) {
    if (!channel) return MAELYS_MCP_ERR_ARGUMENT;
    maelys_mcp_result_t status = maelys_mcp_channel_close(
        channel, channel->config.close_timeout_ms);
    if (status != MAELYS_MCP_OK) {
        /*
         * Destruction consumes ownership even when the bounded drain failed.
         * Abort wakes blocked outbox users; the channel remains allocated until
         * all operations that already retained it have released their borrow.
         */
        maelys_mcp_channel_abort(channel);
        pthread_mutex_lock(&channel->mutex);
        while (channel->operations_inflight != 0u) {
            pthread_cond_wait(&channel->idle, &channel->mutex);
        }
        pthread_mutex_unlock(&channel->mutex);
    }
    maelys_mcp_runtime_t *runtime = channel->runtime;
    maelys_mcp_result_t outbox_status = maelys_mcp_outbox_destroy(channel->outbox);
    if (outbox_status != MAELYS_MCP_OK) return outbox_status;
    pthread_mutex_lock(&runtime->channels_mutex);
    maelys_mcp_channel_t **cursor = &runtime->channels;
    while (*cursor && *cursor != channel) cursor = &(*cursor)->next;
    if (*cursor != channel) {
        pthread_mutex_unlock(&runtime->channels_mutex);
        return MAELYS_MCP_ERR_STATE;
    }
    *cursor = channel->next;
    runtime->live_channel_count--;
    pthread_mutex_unlock(&runtime->channels_mutex);
    for (size_t index = 0; index < channel->subscription_count; ++index) {
        maelys_mcp_subscription_clear(&channel->subscriptions[index]);
    }
    free(channel->subscriptions);
    if (channel->legacy_capabilities) json_decref(channel->legacy_capabilities);
    pthread_cond_destroy(&channel->idle);
    pthread_mutex_destroy(&channel->mutex);
    free(channel);
    return status;
}
