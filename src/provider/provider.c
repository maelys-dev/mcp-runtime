#include "src/internal/internal.h"
#include "maelys/mcp/uri.h"

#include <stdlib.h>
#include <string.h>

const char *maelys_mcp_tool_effect_string(maelys_mcp_tool_effect_t effect) {
    switch (effect) {
        case MAELYS_MCP_EFFECT_UNSPECIFIED: return NULL;
        case MAELYS_MCP_EFFECT_READ: return "read";
        case MAELYS_MCP_EFFECT_PREVIEW: return "preview";
        case MAELYS_MCP_EFFECT_APPLY: return "apply";
        case MAELYS_MCP_EFFECT_COMMIT: return "commit";
        case MAELYS_MCP_EFFECT_EXECUTE: return "execute";
    }
    return NULL;
}

maelys_mcp_result_t maelys_mcp_tool_effect_parse(
    const char *value,
    maelys_mcp_tool_effect_t *out_effect) {
    if (!value || !out_effect) return MAELYS_MCP_ERR_ARGUMENT;
    static const maelys_mcp_tool_effect_t effects[] = {
        MAELYS_MCP_EFFECT_READ,
        MAELYS_MCP_EFFECT_PREVIEW,
        MAELYS_MCP_EFFECT_APPLY,
        MAELYS_MCP_EFFECT_COMMIT,
        MAELYS_MCP_EFFECT_EXECUTE
    };
    for (size_t index = 0; index < sizeof(effects) / sizeof(effects[0]); ++index) {
        if (strcmp(value, maelys_mcp_tool_effect_string(effects[index])) == 0) {
            *out_effect = effects[index];
            return MAELYS_MCP_OK;
        }
    }
    return MAELYS_MCP_ERR_ARGUMENT;
}

static void clear_tools(maelys_mcp_owned_tool_t *tools, size_t count) {
    if (!tools) return;
    for (size_t index = 0; index < count; ++index) {
        free(tools[index].name);
        free(tools[index].title);
        free(tools[index].description);
        if (tools[index].input_schema) json_decref(tools[index].input_schema);
        if (tools[index].output_schema) json_decref(tools[index].output_schema);
    }
    free(tools);
}

static void clear_resources(maelys_mcp_owned_resource_t *resources, size_t count) {
    if (!resources) return;
    for (size_t index = 0; index < count; ++index) {
        free(resources[index].uri);
        free(resources[index].name);
        free(resources[index].title);
        free(resources[index].description);
        free(resources[index].mime_type);
    }
    free(resources);
}

static void clear_resource_templates(
    maelys_mcp_owned_resource_template_t *templates,
    size_t count) {
    if (!templates) return;
    for (size_t index = 0; index < count; ++index) {
        free(templates[index].uri_template);
        free(templates[index].name);
        free(templates[index].title);
        free(templates[index].description);
        free(templates[index].mime_type);
    }
    free(templates);
}

void maelys_mcp_provider_result_init(maelys_mcp_provider_result_t *result) {
    if (result) memset(result, 0, sizeof(*result));
}

void maelys_mcp_provider_result_clear(maelys_mcp_provider_result_t *result) {
    if (!result) return;
    if (result->content) json_decref(result->content);
    if (result->structured_content) json_decref(result->structured_content);
    if (result->input_requests) json_decref(result->input_requests);
    if (result->request_state) json_decref(result->request_state);
    maelys_mcp_provider_result_init(result);
}

maelys_mcp_result_t maelys_mcp_provider_set_nested_handlers(
    maelys_mcp_provider_t *provider,
    const maelys_mcp_provider_nested_handlers_t *handlers) {
    if (!provider || !handlers) return MAELYS_MCP_ERR_ARGUMENT;
    if (!handlers->call && !handlers->read_resource) return MAELYS_MCP_ERR_ARGUMENT;
    provider->call_nested = handlers->call;
    provider->read_resource_nested = handlers->read_resource;
    return MAELYS_MCP_OK;
}

void maelys_mcp_provider_bind_event_sink(
    maelys_mcp_provider_t *provider,
    maelys_mcp_result_t (*sink)(
        void *context,
        const maelys_mcp_provider_event_t *event),
    void *context) {
    if (!provider || !provider->event_mutex_initialized) return;
    pthread_mutex_lock(&provider->event_mutex);
    provider->event_sink = sink;
    provider->event_sink_context = sink ? context : NULL;
    pthread_mutex_unlock(&provider->event_mutex);
}

maelys_mcp_result_t maelys_mcp_provider_report_progress(
    maelys_mcp_progress_reporter_t *reporter,
    double progress,
    double total,
    const char *message) {
    /* A NULL reporter means the client asked for no progress, so a provider
     * may report unconditionally. Not an error, and nothing to send. */
    if (!reporter) return MAELYS_MCP_OK;
    if (!reporter->sink || !reporter->sink->emit || !reporter->token) {
        return MAELYS_MCP_ERR_STATE;
    }
    json_t *params = json_object();
    json_t *notification = json_object();
    if (!params || !notification) goto failed;
    /* progressToken and progress are the only required params; total and
     * message are omitted rather than sent empty when not supplied. */
    if (json_object_set(params, "progressToken", reporter->token) != 0 ||
        json_object_set_new(params, "progress", json_real(progress)) != 0 ||
        (total >= 0.0 &&
            json_object_set_new(params, "total", json_real(total)) != 0) ||
        (message &&
            json_object_set_new(params, "message", json_string(message)) != 0)) {
        goto failed;
    }
    if (json_object_set_new(notification, "jsonrpc", json_string("2.0")) != 0 ||
        json_object_set_new(notification, "method",
            json_string("notifications/progress")) != 0) {
        goto failed;
    }
    /* set_new consumes the reference whether or not it succeeds, so `params`
     * must stop being ours the moment it is handed over - not one branch
     * earlier, which would have leaked it when either set_new above failed. */
    int attached = json_object_set_new(notification, "params", params) == 0;
    params = NULL;
    if (!attached) goto failed;
    maelys_mcp_result_t status = reporter->sink->emit(
        reporter->sink->context, notification);
    if (status != MAELYS_MCP_OK) json_decref(notification);
    return status;
failed:
    if (params) json_decref(params);
    if (notification) json_decref(notification);
    return MAELYS_MCP_ERR_MEMORY;
}

maelys_mcp_result_t maelys_mcp_provider_emit_event(
    maelys_mcp_provider_t *provider,
    const maelys_mcp_provider_event_t *event) {
    if (!provider || !event || !provider->event_mutex_initialized ||
        (event->kind != MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED &&
         event->kind != MAELYS_MCP_PROVIDER_EVENT_RESOURCES_LIST_CHANGED &&
         event->kind != MAELYS_MCP_PROVIDER_EVENT_TOOLS_LIST_CHANGED) ||
        (event->kind == MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED &&
         (!event->resource_uri || !*event->resource_uri)) ||
        (event->kind != MAELYS_MCP_PROVIDER_EVENT_RESOURCE_UPDATED &&
         event->resource_uri)) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    pthread_mutex_lock(&provider->event_mutex);
    maelys_mcp_result_t (*sink)(
        void *, const maelys_mcp_provider_event_t *) = provider->event_sink;
    void *context = provider->event_sink_context;
    maelys_mcp_result_t status = sink ?
        sink(context, event) : MAELYS_MCP_ERR_NOT_FOUND;
    pthread_mutex_unlock(&provider->event_mutex);
    return status;
}

maelys_mcp_result_t maelys_mcp_provider_create(
    const maelys_mcp_provider_config_t *config,
    maelys_mcp_provider_t **out_provider) {
    if (!config || !out_provider || !config->name || !*config->name ||
        !config->version || !*config->version ||
        (config->tool_count && (!config->tools || !config->call)) ||
        (config->resource_count && !config->resources) ||
        (config->resource_template_count && !config->resource_templates) ||
        ((config->resource_count || config->resource_template_count) && !config->read_resource)) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    *out_provider = NULL;
    maelys_mcp_provider_t *provider = calloc(1u, sizeof(*provider));
    if (!provider) return MAELYS_MCP_ERR_MEMORY;
    if (pthread_mutex_init(&provider->event_mutex, NULL) != 0) {
        free(provider);
        return MAELYS_MCP_ERR_IO;
    }
    provider->event_mutex_initialized = 1;
    provider->name = maelys_mcp_strdup(config->name);
    provider->version = maelys_mcp_strdup(config->version);
    provider->call = config->call;
    provider->read_resource = config->read_resource;
    provider->activated = 1;
    provider->destroy = config->destroy;
    provider->context = config->context;
    provider->tool_count = config->tool_count;
    provider->resource_count = config->resource_count;
    provider->resource_template_count = config->resource_template_count;
    if (!provider->name || !provider->version) goto memory_error;
    if (config->tool_count) {
        provider->tools = calloc(config->tool_count, sizeof(*provider->tools));
        if (!provider->tools) goto memory_error;
    }
    if (config->resource_count) {
        provider->resources = calloc(config->resource_count, sizeof(*provider->resources));
        if (!provider->resources) goto memory_error;
    }
    if (config->resource_template_count) {
        provider->resource_templates = calloc(config->resource_template_count,
            sizeof(*provider->resource_templates));
        if (!provider->resource_templates) goto memory_error;
    }
    for (size_t index = 0; index < config->tool_count; ++index) {
        const maelys_mcp_tool_t *source = &config->tools[index];
        maelys_mcp_owned_tool_t *target = &provider->tools[index];
        if (!source->name || !*source->name || !source->description ||
            !json_is_object(source->input_schema) ||
            !maelys_mcp_tool_effect_string(source->effect)) goto argument_error;
        if (maelys_mcp_validate_schema_definition(source->input_schema, 1, NULL) != MAELYS_MCP_OK ||
            (source->output_schema &&
             maelys_mcp_validate_schema_definition(source->output_schema, 0, NULL) != MAELYS_MCP_OK)) {
            goto argument_error;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (strcmp(source->name, config->tools[previous].name) == 0) goto argument_error;
        }
        target->name = maelys_mcp_strdup(source->name);
        target->title = maelys_mcp_strdup(source->title ? source->title : source->name);
        target->description = maelys_mcp_strdup(source->description);
        target->input_schema = json_incref(source->input_schema);
        target->output_schema = source->output_schema ? json_incref(source->output_schema) : NULL;
        target->effect = source->effect;
        target->provider = provider;
        if (!target->name || !target->title || !target->description) goto memory_error;
    }
    for (size_t index = 0; index < config->resource_count; ++index) {
        const maelys_mcp_resource_t *source = &config->resources[index];
        maelys_mcp_owned_resource_t *target = &provider->resources[index];
        if (!source->uri || !source->name || !*source->name ||
            (source->has_size && source->size < 0)) {
            goto argument_error;
        }
        maelys_uri_t *uri = NULL;
        maelys_uri_options_t uri_options = {.max_bytes = 8192u, .require_scheme = 1};
        if (maelys_uri_parse(source->uri, strlen(source->uri), &uri_options,
            &uri, NULL) != MAELYS_MCP_OK) goto argument_error;
        for (size_t previous = 0; previous < index; ++previous) {
            if (strcmp(maelys_uri_canonical(uri), provider->resources[previous].uri) == 0) {
                maelys_uri_destroy(uri);
                goto argument_error;
            }
        }
        target->uri = maelys_mcp_strdup(maelys_uri_canonical(uri));
        maelys_uri_destroy(uri);
        target->name = maelys_mcp_strdup(source->name);
        target->title = maelys_mcp_strdup(source->title);
        target->description = maelys_mcp_strdup(source->description);
        target->mime_type = maelys_mcp_strdup(source->mime_type);
        target->has_size = source->has_size;
        target->size = source->size;
        target->provider = provider;
        if (!target->uri || !target->name || !target->title || !target->description ||
            !target->mime_type) goto memory_error;
    }
    for (size_t index = 0; index < config->resource_template_count; ++index) {
        const maelys_mcp_resource_template_t *source = &config->resource_templates[index];
        maelys_mcp_owned_resource_template_t *target = &provider->resource_templates[index];
        if (!source->uri_template || !source->name || !*source->name ||
            maelys_uri_template_validate(source->uri_template,
                strlen(source->uri_template), 8192u, NULL) != MAELYS_MCP_OK) {
            goto argument_error;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (strcmp(source->uri_template,
                provider->resource_templates[previous].uri_template) == 0) goto argument_error;
        }
        target->uri_template = maelys_mcp_strdup(source->uri_template);
        target->name = maelys_mcp_strdup(source->name);
        target->title = maelys_mcp_strdup(source->title);
        target->description = maelys_mcp_strdup(source->description);
        target->mime_type = maelys_mcp_strdup(source->mime_type);
        target->provider = provider;
        if (!target->uri_template || !target->name || !target->title ||
            !target->description || !target->mime_type) goto memory_error;
    }
    *out_provider = provider;
    return MAELYS_MCP_OK;

argument_error:
    clear_tools(provider->tools, provider->tool_count);
    clear_resources(provider->resources, provider->resource_count);
    clear_resource_templates(provider->resource_templates, provider->resource_template_count);
    free(provider->name);
    free(provider->version);
    pthread_mutex_destroy(&provider->event_mutex);
    free(provider);
    return MAELYS_MCP_ERR_ARGUMENT;
memory_error:
    clear_tools(provider->tools, provider->tool_count);
    clear_resources(provider->resources, provider->resource_count);
    clear_resource_templates(provider->resource_templates, provider->resource_template_count);
    free(provider->name);
    free(provider->version);
    pthread_mutex_destroy(&provider->event_mutex);
    free(provider);
    return MAELYS_MCP_ERR_MEMORY;
}

void maelys_mcp_provider_destroy(maelys_mcp_provider_t *provider) {
    if (!provider) return;
    if (provider->destroy) provider->destroy(provider->context);
    maelys_mcp_provider_bind_event_sink(provider, NULL, NULL);
    clear_tools(provider->tools, provider->tool_count);
    clear_resources(provider->resources, provider->resource_count);
    clear_resource_templates(provider->resource_templates, provider->resource_template_count);
    free(provider->name);
    free(provider->version);
    if (provider->event_mutex_initialized) {
        pthread_mutex_destroy(&provider->event_mutex);
    }
    free(provider);
}
