#include "src/internal/internal.h"
#include "maelys/mcp/subscriptions.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char **out_error, const char *message) {
    if (!out_error) return;
    free(*out_error);
    *out_error = maelys_mcp_strdup(message);
}

static void stop_provider_events(maelys_mcp_runtime_t *runtime);

static json_t *server_info(maelys_mcp_runtime_t *runtime) {
    json_t *info = json_object();
    if (!info ||
        json_object_set_new(info, "name", json_string(runtime->server_name)) != 0 ||
        json_object_set_new(info, "version", json_string(runtime->server_version)) != 0) {
        if (info) json_decref(info);
        return NULL;
    }
    return info;
}

int maelys_mcp_add_server_meta(
    maelys_mcp_runtime_t *runtime,
    json_t *result,
    const char *result_type) {
    json_t *meta = json_object();
    json_t *info = server_info(runtime);
    if (!meta || !info || !result_type) {
        if (meta) json_decref(meta);
        if (info) json_decref(info);
        return -1;
    }
    if (json_object_set_new(result, "resultType", json_string(result_type)) != 0 ||
        json_object_set(meta, "io.modelcontextprotocol/serverInfo", info) != 0 ||
        json_object_set(result, "_meta", meta) != 0) {
        json_decref(meta);
        json_decref(info);
        return -1;
    }
    json_decref(meta);
    json_decref(info);
    return 0;
}

maelys_mcp_result_t maelys_mcp_runtime_create(
    const maelys_mcp_runtime_config_t *config,
    maelys_mcp_runtime_t **out_runtime) {
    if (!config || !out_runtime || !config->server_name || !*config->server_name ||
        !config->server_version || !*config->server_version) return MAELYS_MCP_ERR_ARGUMENT;
    *out_runtime = NULL;
    maelys_mcp_runtime_t *runtime = calloc(1u, sizeof(*runtime));
    if (!runtime) return MAELYS_MCP_ERR_MEMORY;
    runtime->server_name = maelys_mcp_strdup(config->server_name);
    runtime->server_version = maelys_mcp_strdup(config->server_version);
    runtime->instructions = maelys_mcp_strdup(config->instructions);
    runtime->max_providers = config->max_providers ? config->max_providers : 16u;
    runtime->max_message_bytes = config->max_message_bytes ?
        config->max_message_bytes : MAELYS_MCP_DEFAULT_MAX_MESSAGE_BYTES;
    runtime->max_subscriptions = config->max_subscriptions ?
        config->max_subscriptions : 64u;
    runtime->authorize = config->authorize;
    runtime->audit = config->audit;
    runtime->policy_context = config->policy_context;
    runtime->providers = calloc(runtime->max_providers, sizeof(*runtime->providers));
    if (!runtime->server_name || !runtime->server_version || !runtime->instructions ||
        !runtime->providers) goto failed;
    if (pthread_mutex_init(&runtime->lifecycle_mutex, NULL) != 0) goto failed;
    runtime->lifecycle_mutex_initialized = 1;
    if (maelys_mcp_cond_init_monotonic(&runtime->lifecycle_changed) != 0) goto failed;
    runtime->lifecycle_changed_initialized = 1;
    if (pthread_mutex_init(&runtime->channel_create_gate_mutex, NULL) != 0) {
        goto failed;
    }
    runtime->channel_create_gate_mutex_initialized = 1;
    if (maelys_mcp_cond_init_monotonic(&runtime->channel_create_gate_drained) != 0) {
        goto failed;
    }
    runtime->channel_create_gate_drained_initialized = 1;
    if (pthread_mutex_init(&runtime->channels_mutex, NULL) != 0) goto failed;
    runtime->channels_mutex_initialized = 1;
    if (pthread_mutex_init(&runtime->provider_events_mutex, NULL) != 0) {
        goto failed;
    }
    runtime->provider_events_mutex_initialized = 1;
    if (maelys_mcp_cond_init_monotonic(&runtime->provider_events_idle) != 0) goto failed;
    runtime->provider_events_idle_initialized = 1;
    runtime->lifecycle = MAELYS_MCP_RUNTIME_COLD;
    runtime->activation_status = MAELYS_MCP_OK;
    runtime->channel_create_accepting = 1;
    *out_runtime = runtime;
    return MAELYS_MCP_OK;

failed:
    if (runtime->provider_events_idle_initialized) {
        pthread_cond_destroy(&runtime->provider_events_idle);
    }
    if (runtime->provider_events_mutex_initialized) {
        pthread_mutex_destroy(&runtime->provider_events_mutex);
    }
    if (runtime->channels_mutex_initialized) {
        pthread_mutex_destroy(&runtime->channels_mutex);
    }
    if (runtime->channel_create_gate_drained_initialized) {
        pthread_cond_destroy(&runtime->channel_create_gate_drained);
    }
    if (runtime->channel_create_gate_mutex_initialized) {
        pthread_mutex_destroy(&runtime->channel_create_gate_mutex);
    }
    if (runtime->lifecycle_changed_initialized) {
        pthread_cond_destroy(&runtime->lifecycle_changed);
    }
    if (runtime->lifecycle_mutex_initialized) {
        pthread_mutex_destroy(&runtime->lifecycle_mutex);
    }
    free(runtime->providers);
    free(runtime->server_name);
    free(runtime->server_version);
    free(runtime->instructions);
    free(runtime);
    return MAELYS_MCP_ERR_MEMORY;
}

maelys_mcp_result_t maelys_mcp_runtime_destroy(maelys_mcp_runtime_t *runtime) {
    if (!runtime) return MAELYS_MCP_ERR_ARGUMENT;
    pthread_mutex_lock(&runtime->channel_create_gate_mutex);
    runtime->channel_create_accepting = 0;
    while (runtime->channel_creators_admitted != 0u) {
        pthread_cond_wait(&runtime->channel_create_gate_drained,
            &runtime->channel_create_gate_mutex);
    }
    pthread_mutex_unlock(&runtime->channel_create_gate_mutex);

    pthread_mutex_lock(&runtime->lifecycle_mutex);
    runtime->shutdown_requested = 1;
    runtime->lifecycle = MAELYS_MCP_RUNTIME_SHUTTING_DOWN;
    pthread_cond_broadcast(&runtime->lifecycle_changed);
    pthread_mutex_lock(&runtime->channels_mutex);
    if (runtime->live_channel_count != 0u) {
        pthread_mutex_unlock(&runtime->channels_mutex);
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        return MAELYS_MCP_ERR_STATE;
    }
    pthread_mutex_unlock(&runtime->channels_mutex);
    pthread_mutex_unlock(&runtime->lifecycle_mutex);
    stop_provider_events(runtime);
    for (size_t index = 0; index < runtime->provider_count; ++index) {
        maelys_mcp_provider_bind_event_sink(runtime->providers[index], NULL, NULL);
        maelys_mcp_provider_destroy(runtime->providers[index]);
    }
    pthread_cond_destroy(&runtime->provider_events_idle);
    pthread_mutex_destroy(&runtime->provider_events_mutex);
    pthread_mutex_destroy(&runtime->channels_mutex);
    pthread_cond_destroy(&runtime->channel_create_gate_drained);
    pthread_mutex_destroy(&runtime->channel_create_gate_mutex);
    pthread_cond_destroy(&runtime->lifecycle_changed);
    pthread_mutex_destroy(&runtime->lifecycle_mutex);
    free(runtime->providers);
    free(runtime->server_name);
    free(runtime->server_version);
    free(runtime->instructions);
    free(runtime);
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t handle_provider_event(
    void *context,
    const maelys_mcp_provider_event_t *event) {
    maelys_mcp_runtime_t *runtime = context;
    if (!runtime || !event) return MAELYS_MCP_ERR_ARGUMENT;
    maelys_mcp_result_t admitted = maelys_mcp_runtime_begin_provider_event(runtime);
    if (admitted != MAELYS_MCP_OK) return admitted;
    maelys_mcp_result_t result;
    switch (event->kind) {
        case MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED:
            result = maelys_mcp_runtime_notify_resource_updated(
                runtime, event->resource_uri);
            break;
        case MAELYS_MCP_PROVIDER_EVENT_RESOURCES_LIST_CHANGED:
            result = maelys_mcp_runtime_notify_resources_list_changed(runtime);
            break;
        case MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED:
            result = maelys_mcp_runtime_notify_tools_list_changed(runtime);
            break;
        default:
            result = MAELYS_MCP_ERR_ARGUMENT;
            break;
    }
    maelys_mcp_runtime_end_provider_event(runtime);
    return result;
}

maelys_mcp_result_t maelys_mcp_runtime_begin_provider_event(
    maelys_mcp_runtime_t *runtime) {
    if (!runtime) return MAELYS_MCP_ERR_ARGUMENT;
    pthread_mutex_lock(&runtime->provider_events_mutex);
    if (!runtime->provider_events_accepting) {
        pthread_mutex_unlock(&runtime->provider_events_mutex);
        return MAELYS_MCP_ERR_NOT_FOUND;
    }
    runtime->provider_events_inflight++;
    pthread_mutex_unlock(&runtime->provider_events_mutex);
    return MAELYS_MCP_OK;
}

void maelys_mcp_runtime_end_provider_event(maelys_mcp_runtime_t *runtime) {
    if (!runtime) return;
    pthread_mutex_lock(&runtime->provider_events_mutex);
    assert(runtime->provider_events_inflight != 0u);
    runtime->provider_events_inflight--;
    if (runtime->provider_events_inflight == 0u) {
        pthread_cond_broadcast(&runtime->provider_events_idle);
    }
    pthread_mutex_unlock(&runtime->provider_events_mutex);
}

static void stop_provider_events(maelys_mcp_runtime_t *runtime) {
    pthread_mutex_lock(&runtime->provider_events_mutex);
    runtime->provider_events_accepting = 0;
    while (runtime->provider_events_inflight != 0u) {
        pthread_cond_wait(&runtime->provider_events_idle,
            &runtime->provider_events_mutex);
    }
    pthread_mutex_unlock(&runtime->provider_events_mutex);
}

static int valid_request_id(const json_t *value) {
    return json_is_integer(value) ||
        (json_is_string(value) && !maelys_mcp_json_string_has_nul(value));
}

static void handle_cancelled_notification(
    maelys_mcp_channel_t *channel,
    json_t *params) {
    maelys_mcp_runtime_t *runtime = channel->runtime;
    if (!maelys_mcp_runtime_module_enabled(runtime,
        MAELYS_MCP_MODULE_SUBSCRIPTIONS) || !json_is_object(params)) return;
    json_t *request_id = json_object_get(params, "requestId");
    if (valid_request_id(request_id)) {
        maelys_mcp_cancel_subscription(channel, request_id);
    }
}

maelys_mcp_result_t maelys_mcp_runtime_add_provider(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_provider_t *provider,
    char **out_error) {
    if (!runtime || !provider) return MAELYS_MCP_ERR_ARGUMENT;
    pthread_mutex_lock(&runtime->lifecycle_mutex);
    if (runtime->lifecycle != MAELYS_MCP_RUNTIME_COLD ||
        runtime->shutdown_requested) {
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        return MAELYS_MCP_ERR_STATE;
    }
    if (provider->tool_count &&
        !maelys_mcp_runtime_module_enabled(runtime, MAELYS_MCP_MODULE_TOOLS)) {
        set_error(out_error, "tools module is not enabled");
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if ((provider->resource_count || provider->resource_template_count) &&
        !maelys_mcp_runtime_module_enabled(runtime, MAELYS_MCP_MODULE_RESOURCES)) {
        set_error(out_error, "resources module is not enabled");
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    if (runtime->provider_count == runtime->max_providers) {
        set_error(out_error, "provider capacity reached");
        pthread_mutex_unlock(&runtime->lifecycle_mutex);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    for (size_t candidate = 0; candidate < provider->tool_count; ++candidate) {
        for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
            maelys_mcp_provider_t *existing = runtime->providers[provider_index];
            for (size_t tool_index = 0; tool_index < existing->tool_count; ++tool_index) {
                if (strcmp(provider->tools[candidate].name, existing->tools[tool_index].name) == 0) {
                    set_error(out_error, "duplicate tool name");
                    pthread_mutex_unlock(&runtime->lifecycle_mutex);
                    return MAELYS_MCP_ERR_ARGUMENT;
                }
            }
        }
    }
    for (size_t candidate = 0; candidate < provider->resource_count; ++candidate) {
        for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
            maelys_mcp_provider_t *existing = runtime->providers[provider_index];
            for (size_t index = 0; index < existing->resource_count; ++index) {
                if (strcmp(provider->resources[candidate].uri,
                    existing->resources[index].uri) == 0) {
                    set_error(out_error, "duplicate resource URI");
                    pthread_mutex_unlock(&runtime->lifecycle_mutex);
                    return MAELYS_MCP_ERR_ARGUMENT;
                }
            }
        }
    }
    for (size_t candidate = 0; candidate < provider->resource_template_count; ++candidate) {
        for (size_t provider_index = 0; provider_index < runtime->provider_count; ++provider_index) {
            maelys_mcp_provider_t *existing = runtime->providers[provider_index];
            for (size_t index = 0; index < existing->resource_template_count; ++index) {
                if (strcmp(provider->resource_templates[candidate].uri_template,
                    existing->resource_templates[index].uri_template) == 0) {
                    set_error(out_error, "duplicate resource template");
                    pthread_mutex_unlock(&runtime->lifecycle_mutex);
                    return MAELYS_MCP_ERR_ARGUMENT;
                }
            }
        }
    }
    maelys_mcp_provider_bind_event_sink(provider, handle_provider_event, runtime);
    runtime->providers[runtime->provider_count++] = provider;
    pthread_mutex_unlock(&runtime->lifecycle_mutex);
    return MAELYS_MCP_OK;
}

static const char *client_name_from_params(json_t *params) {
    json_t *meta = json_is_object(params) ? json_object_get(params, "_meta") : NULL;
    json_t *client = json_is_object(meta) ?
        json_object_get(meta, "io.modelcontextprotocol/clientInfo") : NULL;
    json_t *name = json_is_object(client) ? json_object_get(client, "name") : NULL;
    return json_is_string(name) ? json_string_value(name) : "unknown";
}

/*
 * Legacy clients negotiate over the classic initialize handshake. Every dated
 * revision up to 2025-11-25 shares that handshake, so the runtime accepts any
 * of them and echoes the client's version back — MCP version negotiation says a
 * server that supports the requested version MUST respond with the same one.
 * 2026-07-28 dropped initialize entirely and is handled statelessly through
 * per-request _meta, so it never reaches this path.
 */
static const char *const legacy_protocol_versions[] = {
    "2024-11-05", "2025-03-26", "2025-06-18", MAELYS_MCP_PROTOCOL_LEGACY,
};

static int is_supported_legacy_version(const json_t *version) {
    if (!json_is_string(version) || maelys_mcp_json_string_has_nul(version)) return 0;
    const char *value = json_string_value(version);
    for (size_t index = 0;
         index < sizeof(legacy_protocol_versions) / sizeof(legacy_protocol_versions[0]);
         ++index) {
        if (strcmp(value, legacy_protocol_versions[index]) == 0) return 1;
    }
    return 0;
}

static json_t *initialize(
    maelys_mcp_channel_t *channel,
    json_t *id,
    json_t *params) {
    maelys_mcp_runtime_t *runtime = channel->runtime;
    pthread_mutex_lock(&channel->mutex);
    if (channel->legacy_initialize_received) {
        pthread_mutex_unlock(&channel->mutex);
        return maelys_mcp_error_response(id, JSONRPC_INVALID_REQUEST,
            "Initialize already received", NULL);
    }
    json_t *version = json_is_object(params) ? json_object_get(params, "protocolVersion") : NULL;
    if (!is_supported_legacy_version(version)) {
        pthread_mutex_unlock(&channel->mutex);
        return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS,
            "Unsupported legacy protocol version", NULL);
    }
    (void)snprintf(channel->legacy_protocol_version,
        sizeof(channel->legacy_protocol_version), "%s", json_string_value(version));
    /*
     * Legacy revisions declare client capabilities once, here, under the same
     * top-level keys (elicitation, sampling, roots) that modern per-request
     * _meta.clientCapabilities carries. Storing this lets a legacy channel use
     * input_required/MRTR under its own negotiated revision, per the spec's
     * compatibility matrix ("Legacy | Dual-era: the server ... serves the
     * client according to the negotiated legacy revision") instead of being
     * unconditionally denied for not being modern.
     */
    json_t *capabilities = json_object_get(params, "capabilities");
    if (json_is_object(capabilities)) {
        channel->legacy_capabilities = json_incref(capabilities);
    }
    json_t *client = json_object_get(params, "clientInfo");
    json_t *name = json_is_object(client) ? json_object_get(client, "name") : NULL;
    (void)snprintf(channel->legacy_client_name, sizeof(channel->legacy_client_name),
        "%s", json_is_string(name) && !maelys_mcp_json_string_has_nul(name) ?
        json_string_value(name) : "unknown");
    channel->legacy_initialize_received = 1;
    pthread_mutex_unlock(&channel->mutex);
    json_t *result = json_object();
    json_t *info = server_info(runtime);
    json_t *caps = maelys_mcp_runtime_capabilities(runtime, 0);
    if (!result || !info || !caps ||
        json_object_set_new(result, "protocolVersion",
            json_string(json_string_value(version))) != 0 ||
        json_object_set(result, "capabilities", caps) != 0 ||
        json_object_set(result, "serverInfo", info) != 0) {
        if (result) json_decref(result);
        if (info) json_decref(info);
        if (caps) json_decref(caps);
        return maelys_mcp_error_response(id, JSONRPC_INTERNAL_ERROR, "Internal error", NULL);
    }
    if (*runtime->instructions) {
        (void)json_object_set_new(result, "instructions", json_string(runtime->instructions));
    }
    json_decref(info);
    json_decref(caps);
    return maelys_mcp_success_response(id, result);
}

static json_t *discover(maelys_mcp_runtime_t *runtime, json_t *id) {
    json_t *result = json_object();
    json_t *versions = json_array();
    json_t *caps = maelys_mcp_runtime_capabilities(runtime, 1);
    if (!result || !versions || !caps ||
        json_array_append_new(versions, json_string(MAELYS_MCP_PROTOCOL_MODERN)) != 0 ||
        json_array_append_new(versions, json_string(MAELYS_MCP_PROTOCOL_LEGACY)) != 0 ||
        json_object_set(result, "supportedVersions", versions) != 0 ||
        json_object_set(result, "capabilities", caps) != 0 ||
        json_object_set_new(result, "ttlMs", json_integer(0)) != 0 ||
        json_object_set_new(result, "cacheScope", json_string("private")) != 0 ||
        maelys_mcp_add_server_meta(runtime, result, "complete") != 0) {
        if (result) json_decref(result);
        if (versions) json_decref(versions);
        if (caps) json_decref(caps);
        return maelys_mcp_error_response(id, JSONRPC_INTERNAL_ERROR, "Internal error", NULL);
    }
    if (*runtime->instructions) {
        (void)json_object_set_new(result, "instructions", json_string(runtime->instructions));
    }
    json_decref(versions);
    json_decref(caps);
    return maelys_mcp_success_response(id, result);
}

static json_t *unsupported_version(json_t *id, const char *requested) {
    json_t *data = json_pack("{s:[s,s],s:s}", "supported",
        MAELYS_MCP_PROTOCOL_MODERN, MAELYS_MCP_PROTOCOL_LEGACY,
        "requested", requested ? requested : "");
    json_t *response = maelys_mcp_error_response(id, MCP_UNSUPPORTED_VERSION,
        "Unsupported protocol version", data);
    if (data) json_decref(data);
    return response;
}

static json_t *validate_modern_metadata(json_t *id, json_t *params, const char **out_version) {
    json_t *meta = json_is_object(params) ? json_object_get(params, "_meta") : NULL;
    json_t *version = json_is_object(meta) ?
        json_object_get(meta, "io.modelcontextprotocol/protocolVersion") : NULL;
    json_t *capabilities = json_is_object(meta) ?
        json_object_get(meta, "io.modelcontextprotocol/clientCapabilities") : NULL;
    json_t *client = json_is_object(meta) ?
        json_object_get(meta, "io.modelcontextprotocol/clientInfo") : NULL;
    if (!json_is_string(version) || maelys_mcp_json_string_has_nul(version) ||
        !json_is_object(capabilities)) {
        return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS,
            "Modern requests require protocolVersion and clientCapabilities metadata", NULL);
    }
    if (client) {
        json_t *name = json_is_object(client) ? json_object_get(client, "name") : NULL;
        json_t *client_version = json_is_object(client) ? json_object_get(client, "version") : NULL;
        if (!json_is_string(name) || maelys_mcp_json_string_has_nul(name) ||
            !json_is_string(client_version) || maelys_mcp_json_string_has_nul(client_version)) {
            return maelys_mcp_error_response(id, JSONRPC_INVALID_PARAMS,
                "clientInfo must contain name and version", NULL);
        }
    }
    *out_version = json_string_value(version);
    return maelys_mcp_json_string_equals(version, MAELYS_MCP_PROTOCOL_MODERN) ?
        NULL : unsupported_version(id, *out_version);
}

json_t *maelys_mcp_runtime_dispatch(
    maelys_mcp_channel_t *channel,
    json_t *request,
    const maelys_mcp_response_sink_t *sink,
    json_t **out_post_enqueue_subscription_id) {
    if (out_post_enqueue_subscription_id) *out_post_enqueue_subscription_id = NULL;
    if (!channel || !json_is_object(request)) {
        return maelys_mcp_error_response(NULL, JSONRPC_INVALID_REQUEST,
            "Invalid Request", NULL);
    }
    maelys_mcp_runtime_t *runtime = channel->runtime;
    json_t *id = json_object_get(request, "id");
    json_t *jsonrpc = json_object_get(request, "jsonrpc");
    json_t *method = json_object_get(request, "method");
    json_t *params = json_object_get(request, "params");
    int valid_id = !id || json_is_integer(id) ||
        (json_is_string(id) && !maelys_mcp_json_string_has_nul(id));
    if (!maelys_mcp_json_string_equals(jsonrpc, "2.0") ||
        !json_is_string(method) || maelys_mcp_json_string_has_nul(method) || !valid_id) {
        return maelys_mcp_error_response(valid_id ? id : NULL,
            JSONRPC_INVALID_REQUEST, "Invalid Request", NULL);
    }
    const char *method_name = json_string_value(method);
    if (strcmp(method_name, "initialize") == 0) return initialize(channel, id, params);
    if (strcmp(method_name, "notifications/initialized") == 0) {
        pthread_mutex_lock(&channel->mutex);
        if (channel->legacy_initialize_received) channel->legacy_initialized = 1;
        pthread_mutex_unlock(&channel->mutex);
        return NULL;
    }
    if (strcmp(method_name, "notifications/cancelled") == 0 && !id) {
        handle_cancelled_notification(channel, params);
        return NULL;
    }
    if (!id) return NULL;

    char legacy_client_name[sizeof(channel->legacy_client_name)];
    char legacy_protocol_version[sizeof(channel->legacy_protocol_version)];
    pthread_mutex_lock(&channel->mutex);
    int legacy_initialized = channel->legacy_initialized;
    (void)snprintf(legacy_client_name, sizeof(legacy_client_name), "%s",
        channel->legacy_client_name);
    (void)snprintf(legacy_protocol_version, sizeof(legacy_protocol_version), "%s",
        channel->legacy_protocol_version);
    /*
     * Borrowed, not owned: this channel field is written once (initialize())
     * and freed only at channel_destroy(), which waits for every in-flight
     * operation on this channel to drain first. Reading the pointer value
     * under the channel mutex is enough to make the read itself race-free;
     * the object's lifetime for the remainder of this synchronous dispatch is
     * already guaranteed by the operations_inflight count channel_handle()
     * incremented before calling here.
     */
    json_t *legacy_capabilities = channel->legacy_capabilities;
    pthread_mutex_unlock(&channel->mutex);

    /*
     * `_meta` is a general MCP extension point (progress tokens, and other
     * application-defined keys), not by itself a sign of protocol negotiation.
     * Only treat a request as attempting 2026-07-28 per-request negotiation
     * when `_meta` actually carries the protocol-version key — including on an
     * already legacy-initialized channel, where that key can only mean an
     * explicit (and here unsupported) attempt to renegotiate mid-session.
     * Absent that key, `_meta` is opaque and passed through untouched.
     * server/discover is the stateless discovery entry point and always
     * requires full modern metadata, regardless of any prior legacy session.
     */
    json_t *meta = json_is_object(params) ? json_object_get(params, "_meta") : NULL;
    int meta_requests_negotiation = json_is_object(meta) &&
        json_object_get(meta, "io.modelcontextprotocol/protocolVersion") != NULL;
    const char *modern_version = NULL;
    if (meta_requests_negotiation || strcmp(method_name, "server/discover") == 0) {
        json_t *metadata_error = validate_modern_metadata(id, params, &modern_version);
        if (metadata_error) return metadata_error;
    }
    if (strcmp(method_name, "server/discover") == 0) return discover(runtime, id);

    int modern = modern_version != NULL;
    if (!modern && !legacy_initialized) {
        return maelys_mcp_error_response(id, MCP_SERVER_NOT_INITIALIZED,
            "Server not initialized", NULL);
    }
    maelys_mcp_module_request_t module_request = {
        .channel = channel,
        .sink = sink,
        .id = id,
        .params = params,
        .protocol_version = modern ? modern_version : legacy_protocol_version,
        .client_name = modern ? client_name_from_params(params) : legacy_client_name,
        .legacy_capabilities = legacy_capabilities,
        .modern = modern,
        .post_enqueue_subscription_id = out_post_enqueue_subscription_id
    };
    for (size_t index = 0; index < runtime->module_count; ++index) {
        const maelys_mcp_module_descriptor_t *module = runtime->modules[index];
        if (module->handles && module->handles(method_name)) {
            return module->handle(runtime, method_name, &module_request);
        }
    }
    return maelys_mcp_error_response(id, JSONRPC_METHOD_NOT_FOUND,
        "Method not found", NULL);
}
