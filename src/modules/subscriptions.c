#include "src/internal/internal.h"
#include "maelys/mcp/subscriptions.h"
#include "maelys/mcp/uri.h"

#include <stdlib.h>
#include <string.h>

#define SUBSCRIPTION_META_KEY "io.modelcontextprotocol/subscriptionId"

typedef enum subscription_event_kind {
    SUBSCRIPTION_EVENT_RESOURCE_UPDATED,
    SUBSCRIPTION_EVENT_RESOURCES_LIST_CHANGED,
    SUBSCRIPTION_EVENT_TOOLS_LIST_CHANGED
} subscription_event_kind_t;

typedef struct subscription_target {
    json_t *id;
    char *key;
} subscription_target_t;

void maelys_mcp_subscription_clear(maelys_mcp_subscription_t *subscription) {
    if (!subscription) return;
    if (subscription->id) json_decref(subscription->id);
    for (size_t index = 0; index < subscription->resource_uri_count; ++index) {
        free(subscription->resource_uris[index]);
    }
    free(subscription->resource_uris);
    memset(subscription, 0, sizeof(*subscription));
}

static int id_equal(const json_t *left, const json_t *right) {
    return left && right && json_equal(left, right);
}

static ssize_t find_subscription_locked(
    const maelys_mcp_runtime_t *runtime,
    const json_t *id) {
    for (size_t index = 0; index < runtime->subscription_count; ++index) {
        if (id_equal(runtime->subscriptions[index].id, id)) return (ssize_t)index;
    }
    return -1;
}

static void remove_subscription_locked(
    maelys_mcp_runtime_t *runtime,
    size_t index) {
    maelys_mcp_subscription_clear(&runtime->subscriptions[index]);
    if (index + 1u < runtime->subscription_count) {
        runtime->subscriptions[index] =
            runtime->subscriptions[runtime->subscription_count - 1u];
        memset(&runtime->subscriptions[runtime->subscription_count - 1u], 0,
            sizeof(*runtime->subscriptions));
    }
    runtime->subscription_count--;
}

void maelys_mcp_cancel_subscription(
    maelys_mcp_runtime_t *runtime,
    const json_t *request_id) {
    if (!runtime || !request_id || !runtime->subscriptions_mutex_initialized) return;
    pthread_mutex_lock(&runtime->subscriptions_mutex);
    ssize_t found = find_subscription_locked(runtime, request_id);
    if (found >= 0) remove_subscription_locked(runtime, (size_t)found);
    pthread_mutex_unlock(&runtime->subscriptions_mutex);
}

static int append_unique_uri(
    maelys_mcp_subscription_t *subscription,
    const char *uri) {
    for (size_t index = 0; index < subscription->resource_uri_count; ++index) {
        if (strcmp(subscription->resource_uris[index], uri) == 0) return 0;
    }
    char **resized = realloc(subscription->resource_uris,
        (subscription->resource_uri_count + 1u) * sizeof(*resized));
    if (!resized) return -1;
    subscription->resource_uris = resized;
    resized[subscription->resource_uri_count] = maelys_mcp_strdup(uri);
    if (!resized[subscription->resource_uri_count]) return -1;
    subscription->resource_uri_count++;
    return 0;
}

static int parse_resource_uris(
    json_t *values,
    maelys_mcp_subscription_t *subscription,
    char **out_error) {
    if (!values) return 0;
    if (!json_is_array(values)) {
        if (out_error) *out_error = maelys_mcp_strdup(
            "resourceSubscriptions must be an array of URI strings");
        return -1;
    }
    size_t index;
    json_t *value;
    json_array_foreach(values, index, value) {
        if (!json_is_string(value) || maelys_mcp_json_string_has_nul(value)) {
            if (out_error) *out_error = maelys_mcp_strdup(
                "resourceSubscriptions must contain URI strings");
            return -1;
        }
        maelys_uri_options_t options = {.max_bytes = 8192u, .require_scheme = 1};
        maelys_uri_t *parsed = NULL;
        char *uri_error = NULL;
        maelys_mcp_result_t status = maelys_uri_parse(
            json_string_value(value), json_string_length(value),
            &options, &parsed, &uri_error);
        if (status != MAELYS_MCP_OK) {
            if (out_error) *out_error = uri_error;
            else free(uri_error);
            return -1;
        }
        int appended = append_unique_uri(subscription, maelys_uri_canonical(parsed));
        maelys_uri_destroy(parsed);
        if (appended != 0) {
            if (out_error) *out_error = maelys_mcp_strdup("Cannot store subscription URI");
            return -1;
        }
    }
    return 0;
}

static int optional_boolean(json_t *object, const char *key, int *out_value) {
    json_t *value = json_object_get(object, key);
    if (!value) return 0;
    if (!json_is_boolean(value)) return -1;
    *out_value = json_is_true(value);
    return 0;
}

static json_t *accepted_filter(const maelys_mcp_subscription_t *subscription) {
    json_t *accepted = json_object();
    if (!accepted) return NULL;
    if (subscription->tools_list_changed &&
        json_object_set_new(accepted, "toolsListChanged", json_true()) != 0) goto failed;
    if (subscription->resources_list_changed &&
        json_object_set_new(accepted, "resourcesListChanged", json_true()) != 0) goto failed;
    if (subscription->resource_uri_count) {
        json_t *uris = json_array();
        if (!uris) goto failed;
        for (size_t index = 0; index < subscription->resource_uri_count; ++index) {
            if (json_array_append_new(uris,
                json_string(subscription->resource_uris[index])) != 0) {
                json_decref(uris);
                goto failed;
            }
        }
        if (json_object_set(accepted, "resourceSubscriptions", uris) != 0) {
            json_decref(uris);
            goto failed;
        }
        json_decref(uris);
    }
    return accepted;
failed:
    json_decref(accepted);
    return NULL;
}

static json_t *subscription_meta(const json_t *id) {
    json_t *meta = json_object();
    json_t *id_copy = id ? json_deep_copy(id) : NULL;
    if (!meta || !id_copy ||
        json_object_set(meta, SUBSCRIPTION_META_KEY, id_copy) != 0) {
        if (meta) json_decref(meta);
        if (id_copy) json_decref(id_copy);
        return NULL;
    }
    json_decref(id_copy);
    return meta;
}

static json_t *notification(
    const char *method,
    json_t *params) {
    json_t *message = json_object();
    if (!message || !params ||
        json_object_set_new(message, "jsonrpc", json_string("2.0")) != 0 ||
        json_object_set_new(message, "method", json_string(method)) != 0 ||
        json_object_set(message, "params", params) != 0) {
        if (message) json_decref(message);
        if (params) json_decref(params);
        return NULL;
    }
    json_decref(params);
    return message;
}

static json_t *acknowledged(
    const maelys_mcp_subscription_t *subscription) {
    json_t *params = json_object();
    json_t *accepted = accepted_filter(subscription);
    json_t *meta = subscription_meta(subscription->id);
    if (!params || !accepted || !meta ||
        json_object_set(params, "notifications", accepted) != 0 ||
        json_object_set(params, "_meta", meta) != 0) {
        if (params) json_decref(params);
        if (accepted) json_decref(accepted);
        if (meta) json_decref(meta);
        return NULL;
    }
    json_decref(accepted);
    json_decref(meta);
    return notification("notifications/subscriptions/acknowledged", params);
}

static json_t *listen(
    maelys_mcp_runtime_t *runtime,
    const maelys_mcp_module_request_t *request) {
    if (!request->modern) {
        return maelys_mcp_error_response(request->id, JSONRPC_INVALID_PARAMS,
            "Subscriptions require MCP 2026-07-28", NULL);
    }
    json_t *filter = json_is_object(request->params) ?
        json_object_get(request->params, "notifications") : NULL;
    if (!json_is_object(filter)) {
        return maelys_mcp_error_response(request->id, JSONRPC_INVALID_PARAMS,
            "notifications must be an object", NULL);
    }
    maelys_mcp_subscription_t candidate = {0};
    int requested_tools = 0;
    int requested_resources = 0;
    int requested_prompts = 0;
    if (optional_boolean(filter, "toolsListChanged", &requested_tools) != 0 ||
        optional_boolean(filter, "resourcesListChanged", &requested_resources) != 0 ||
        optional_boolean(filter, "promptsListChanged", &requested_prompts) != 0) {
        return maelys_mcp_error_response(request->id, JSONRPC_INVALID_PARAMS,
            "Subscription list-change filters must be booleans", NULL);
    }
    (void)requested_prompts;
    candidate.tools_list_changed = requested_tools &&
        maelys_mcp_runtime_module_enabled(runtime, MAELYS_MCP_MODULE_TOOLS);
    candidate.resources_list_changed = requested_resources &&
        maelys_mcp_runtime_module_enabled(runtime, MAELYS_MCP_MODULE_RESOURCES);
    char *error = NULL;
    if (maelys_mcp_runtime_module_enabled(runtime, MAELYS_MCP_MODULE_RESOURCES) &&
        parse_resource_uris(json_object_get(filter, "resourceSubscriptions"),
            &candidate, &error) != 0) {
        json_t *response = maelys_mcp_error_response(request->id,
            JSONRPC_INVALID_PARAMS, error ? error : "Invalid resource subscription", NULL);
        free(error);
        maelys_mcp_subscription_clear(&candidate);
        return response;
    }
    candidate.id = json_deep_copy(request->id);
    if (!candidate.id) {
        maelys_mcp_subscription_clear(&candidate);
        return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
            "Cannot store subscription", NULL);
    }
    json_t *ack = acknowledged(&candidate);
    if (!ack) {
        maelys_mcp_subscription_clear(&candidate);
        return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
            "Cannot build subscription acknowledgement", NULL);
    }

    pthread_mutex_lock(&runtime->subscriptions_mutex);
    if (!runtime->outbox) {
        pthread_mutex_unlock(&runtime->subscriptions_mutex);
        json_decref(ack);
        maelys_mcp_subscription_clear(&candidate);
        return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
            "Subscriptions require an attached output bus", NULL);
    }
    if (find_subscription_locked(runtime, request->id) >= 0) {
        pthread_mutex_unlock(&runtime->subscriptions_mutex);
        json_decref(ack);
        maelys_mcp_subscription_clear(&candidate);
        return maelys_mcp_error_response(request->id, JSONRPC_INVALID_PARAMS,
            "Subscription id is already active", NULL);
    }
    if (runtime->subscription_count == runtime->max_subscriptions) {
        pthread_mutex_unlock(&runtime->subscriptions_mutex);
        json_decref(ack);
        maelys_mcp_subscription_clear(&candidate);
        return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
            "Subscription capacity reached", NULL);
    }
    size_t slot = runtime->subscription_count++;
    runtime->subscriptions[slot] = candidate;
    maelys_mcp_outbox_t *outbox = runtime->outbox;
    pthread_mutex_unlock(&runtime->subscriptions_mutex);

    /* Protocol-control acknowledgements share the response lane so no later
       response can overtake the mandatory first subscription message. */
    maelys_mcp_result_t status = maelys_mcp_outbox_enqueue_take(
        outbox, ack, MAELYS_MCP_OUTBOX_RESPONSE, NULL);
    if (status != MAELYS_MCP_OK) {
        json_decref(ack);
        maelys_mcp_cancel_subscription(runtime, request->id);
        return maelys_mcp_error_response(request->id, JSONRPC_INTERNAL_ERROR,
            "Cannot acknowledge subscription", NULL);
    }
    pthread_mutex_lock(&runtime->subscriptions_mutex);
    ssize_t registered = find_subscription_locked(runtime, request->id);
    if (registered >= 0) runtime->subscriptions[registered].active = 1;
    pthread_mutex_unlock(&runtime->subscriptions_mutex);
    return NULL;
}

static int uri_matches(const char *subscription_uri, const char *event_uri) {
    size_t length = strlen(subscription_uri);
    return strcmp(subscription_uri, event_uri) == 0 ||
        (strncmp(subscription_uri, event_uri, length) == 0 &&
         (subscription_uri[length - 1u] == '/' || event_uri[length] == '/'));
}

static int accepts(
    const maelys_mcp_subscription_t *subscription,
    subscription_event_kind_t kind,
    const char *uri) {
    if (!subscription->active) return 0;
    if (kind == SUBSCRIPTION_EVENT_TOOLS_LIST_CHANGED) {
        return subscription->tools_list_changed;
    }
    if (kind == SUBSCRIPTION_EVENT_RESOURCES_LIST_CHANGED) {
        return subscription->resources_list_changed;
    }
    for (size_t index = 0; index < subscription->resource_uri_count; ++index) {
        if (uri_matches(subscription->resource_uris[index], uri)) return 1;
    }
    return 0;
}

static char *coalescence_key(
    const json_t *id,
    const char *method,
    const char *uri) {
    char *encoded_id = json_dumps(id, JSON_COMPACT | JSON_ENCODE_ANY);
    if (!encoded_id) return NULL;
    size_t bytes = strlen(encoded_id) + strlen(method) + (uri ? strlen(uri) : 0u) + 4u;
    char *key = malloc(bytes);
    if (key) (void)snprintf(key, bytes, "%s|%s|%s", encoded_id, method, uri ? uri : "");
    free(encoded_id);
    return key;
}

static json_t *event_notification(
    subscription_event_kind_t kind,
    const json_t *id,
    const char *uri) {
    const char *method = kind == SUBSCRIPTION_EVENT_RESOURCE_UPDATED ?
        "notifications/resources/updated" :
        (kind == SUBSCRIPTION_EVENT_RESOURCES_LIST_CHANGED ?
            "notifications/resources/list_changed" : "notifications/tools/list_changed");
    json_t *params = json_object();
    json_t *meta = subscription_meta(id);
    if (!params || !meta ||
        (uri && json_object_set_new(params, "uri", json_string(uri)) != 0) ||
        json_object_set(params, "_meta", meta) != 0) {
        if (params) json_decref(params);
        if (meta) json_decref(meta);
        return NULL;
    }
    json_decref(meta);
    return notification(method, params);
}

static maelys_mcp_result_t emit_event(
    maelys_mcp_runtime_t *runtime,
    subscription_event_kind_t kind,
    const char *uri) {
    if (!runtime || !runtime->subscriptions_mutex_initialized ||
        !maelys_mcp_runtime_module_enabled(runtime, MAELYS_MCP_MODULE_SUBSCRIPTIONS)) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    subscription_target_t *targets = calloc(runtime->max_subscriptions, sizeof(*targets));
    if (!targets) return MAELYS_MCP_ERR_MEMORY;
    size_t count = 0u;
    maelys_mcp_result_t snapshot_status = MAELYS_MCP_OK;
    pthread_mutex_lock(&runtime->subscriptions_mutex);
    maelys_mcp_outbox_t *outbox = runtime->outbox;
    for (size_t index = 0; outbox && index < runtime->subscription_count; ++index) {
        if (!accepts(&runtime->subscriptions[index], kind, uri)) continue;
        targets[count].id = json_deep_copy(runtime->subscriptions[index].id);
        if (!targets[count].id) {
            snapshot_status = MAELYS_MCP_ERR_MEMORY;
            break;
        }
        count++;
    }
    pthread_mutex_unlock(&runtime->subscriptions_mutex);
    if (!outbox) {
        free(targets);
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    maelys_mcp_result_t outcome = snapshot_status;
    const char *method = kind == SUBSCRIPTION_EVENT_RESOURCE_UPDATED ?
        "resources/updated" : (kind == SUBSCRIPTION_EVENT_RESOURCES_LIST_CHANGED ?
            "resources/list_changed" : "tools/list_changed");
    for (size_t index = 0; outcome == MAELYS_MCP_OK && index < count; ++index) {
        json_t *message = event_notification(kind, targets[index].id, uri);
        targets[index].key = coalescence_key(targets[index].id, method, uri);
        if (!message || !targets[index].key) {
            if (message) json_decref(message);
            outcome = MAELYS_MCP_ERR_MEMORY;
        } else {
            maelys_mcp_result_t status = maelys_mcp_outbox_enqueue_take(
                outbox, message, MAELYS_MCP_OUTBOX_NOTIFICATION, targets[index].key);
            if (status != MAELYS_MCP_OK) {
                json_decref(message);
                outcome = status;
            }
        }
        json_decref(targets[index].id);
        targets[index].id = NULL;
        free(targets[index].key);
        targets[index].key = NULL;
        if (outcome != MAELYS_MCP_OK) break;
    }
    for (size_t index = 0; index < count; ++index) {
        if (targets[index].id) {
            json_decref(targets[index].id);
        }
        free(targets[index].key);
    }
    free(targets);
    return outcome;
}

maelys_mcp_result_t maelys_mcp_runtime_notify_resource_updated(
    maelys_mcp_runtime_t *runtime,
    const char *uri) {
    if (!uri) return MAELYS_MCP_ERR_ARGUMENT;
    maelys_uri_options_t options = {.max_bytes = 8192u, .require_scheme = 1};
    maelys_uri_t *parsed = NULL;
    maelys_mcp_result_t status = maelys_uri_parse(uri, strlen(uri),
        &options, &parsed, NULL);
    if (status != MAELYS_MCP_OK) return status;
    char *canonical = maelys_mcp_strdup(maelys_uri_canonical(parsed));
    maelys_uri_destroy(parsed);
    if (!canonical) return MAELYS_MCP_ERR_MEMORY;
    status = emit_event(runtime, SUBSCRIPTION_EVENT_RESOURCE_UPDATED, canonical);
    free(canonical);
    return status;
}

maelys_mcp_result_t maelys_mcp_runtime_notify_resources_list_changed(
    maelys_mcp_runtime_t *runtime) {
    return emit_event(runtime, SUBSCRIPTION_EVENT_RESOURCES_LIST_CHANGED, NULL);
}

maelys_mcp_result_t maelys_mcp_runtime_notify_tools_list_changed(
    maelys_mcp_runtime_t *runtime) {
    return emit_event(runtime, SUBSCRIPTION_EVENT_TOOLS_LIST_CHANGED, NULL);
}

maelys_mcp_result_t maelys_mcp_runtime_complete_subscriptions(
    maelys_mcp_runtime_t *runtime) {
    if (!runtime || !runtime->subscriptions_mutex_initialized) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    pthread_mutex_lock(&runtime->subscriptions_mutex);
    maelys_mcp_outbox_t *outbox = runtime->outbox;
    size_t count = runtime->subscription_count;
    json_t **ids = calloc(count ? count : 1u, sizeof(*ids));
    if (!ids) {
        pthread_mutex_unlock(&runtime->subscriptions_mutex);
        return MAELYS_MCP_ERR_MEMORY;
    }
    for (size_t index = 0; index < count; ++index) {
        ids[index] = json_deep_copy(runtime->subscriptions[index].id);
        maelys_mcp_subscription_clear(&runtime->subscriptions[index]);
    }
    runtime->subscription_count = 0u;
    pthread_mutex_unlock(&runtime->subscriptions_mutex);
    maelys_mcp_result_t outcome = MAELYS_MCP_OK;
    for (size_t index = 0; index < count; ++index) {
        if (!ids[index]) {
            outcome = MAELYS_MCP_ERR_MEMORY;
            continue;
        }
        json_t *result = json_object();
        json_t *meta = subscription_meta(ids[index]);
        if (!result || !meta ||
            json_object_set_new(result, "resultType", json_string("complete")) != 0 ||
            json_object_set(result, "_meta", meta) != 0) {
            if (result) json_decref(result);
            if (meta) json_decref(meta);
            outcome = MAELYS_MCP_ERR_MEMORY;
        } else {
            json_decref(meta);
            json_t *response_id = json_deep_copy(ids[index]);
            json_t *response = response_id ?
                maelys_mcp_success_response(response_id, result) : NULL;
            if (!response_id) json_decref(result);
            if (response_id) json_decref(response_id);
            if (!response || !outbox) {
                if (response) json_decref(response);
                outcome = outbox ? MAELYS_MCP_ERR_MEMORY : MAELYS_MCP_ERR_ARGUMENT;
            } else {
                maelys_mcp_result_t status = maelys_mcp_outbox_enqueue_take(
                    outbox, response, MAELYS_MCP_OUTBOX_RESPONSE, NULL);
                if (status != MAELYS_MCP_OK) {
                    json_decref(response);
                    outcome = status;
                }
            }
        }
        json_decref(ids[index]);
    }
    free(ids);
    return outcome;
}

static int handles(const char *method) {
    return strcmp(method, "subscriptions/listen") == 0;
}

static json_t *handle(
    maelys_mcp_runtime_t *runtime,
    const char *method,
    const maelys_mcp_module_request_t *request) {
    (void)method;
    return listen(runtime, request);
}

const maelys_mcp_module_descriptor_t maelys_mcp_subscriptions_module = {
    .kind = MAELYS_MCP_MODULE_SUBSCRIPTIONS,
    .name = "subscriptions",
    .capability_name = NULL,
    .capability = NULL,
    .handles = handles,
    .handle = handle
};
