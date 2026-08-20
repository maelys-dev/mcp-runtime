#include "maelys/mcp.h"
#include "tests/test_support.h"

#include <stdlib.h>
#include <string.h>

/*
 * The middleware chain core: registration and ordering, hook 2 at all five
 * policy decision points, hook 5 with both views, and the compatibility
 * middleware that stands in for the removed authorize/audit config fields.
 */

#define PROBE_CAPACITY 32u

typedef struct probe_event {
    maelys_mcp_operation_t operation;
    maelys_mcp_tool_effect_t effect;
    char identity[128];
    char requested[128];
    /* Continuation traffic, the blind spot hook 2 exists to close. */
    int saw_input_responses;
    int saw_request_state;
    int saw_params;
    void *channel_context;
    maelys_mcp_result_t outcome;
} probe_event_t;

typedef struct destroy_log {
    int order[8];
    size_t count;
} destroy_log_t;

typedef struct probe {
    int id;
    /* Deny (or fail on) the one identity whose name matches, allow the rest. */
    const char *deny_identity;
    const char *error_identity;
    /* When set, allow only the channel carrying exactly this context. */
    void *required_context;
    int required_context_set;
    probe_event_t authorized[PROBE_CAPACITY];
    size_t authorized_count;
    probe_event_t audited[PROBE_CAPACITY];
    size_t audited_count;
    destroy_log_t *destroy_log;
} probe_t;

static void record_identity(char *slot, size_t size, const char *value) {
    (void)snprintf(slot, size, "%s", value ? value : "");
}

static maelys_mcp_authorize_decision_t probe_authorize(
    void *context,
    const maelys_mcp_authorize_context_t *request) {
    probe_t *probe = context;
    const char *identity = request->tool_name ?
        request->tool_name : request->resource_uri;
    if (probe->authorized_count < PROBE_CAPACITY) {
        probe_event_t *event = &probe->authorized[probe->authorized_count++];
        memset(event, 0, sizeof(*event));
        event->operation = request->operation;
        event->effect = request->effect;
        record_identity(event->identity, sizeof(event->identity), identity);
        event->saw_params = request->params != NULL;
        event->saw_input_responses = json_is_object(request->params) &&
            json_object_get(request->params, "inputResponses") != NULL;
        event->saw_request_state = json_is_object(request->params) &&
            json_object_get(request->params, "requestState") != NULL;
        event->channel_context = maelys_mcp_channel_context(request->channel);
    }
    if (probe->error_identity && identity &&
        strcmp(identity, probe->error_identity) == 0) {
        return MAELYS_MCP_AUTHORIZE_ERROR;
    }
    if (probe->deny_identity && identity &&
        strcmp(identity, probe->deny_identity) == 0) {
        return MAELYS_MCP_AUTHORIZE_DENY;
    }
    if (probe->required_context_set &&
        maelys_mcp_channel_context(request->channel) != probe->required_context) {
        return MAELYS_MCP_AUTHORIZE_DENY;
    }
    return MAELYS_MCP_AUTHORIZE_ALLOW;
}

static void probe_audit(void *context, const maelys_mcp_audit_context_t *record) {
    probe_t *probe = context;
    if (probe->audited_count >= PROBE_CAPACITY) return;
    probe_event_t *event = &probe->audited[probe->audited_count++];
    memset(event, 0, sizeof(*event));
    event->operation = record->operation;
    event->effect = record->effect;
    record_identity(event->identity, sizeof(event->identity),
        record->tool_name ? record->tool_name : record->resource_uri);
    record_identity(event->requested, sizeof(event->requested),
        record->requested_tool_name ? record->requested_tool_name :
        record->requested_resource_uri);
    event->saw_params = record->params != NULL;
    event->channel_context = maelys_mcp_channel_context(record->channel);
    event->outcome = record->outcome;
}

static void probe_destroy(void *context) {
    probe_t *probe = context;
    if (probe->destroy_log && probe->destroy_log->count <
        sizeof(probe->destroy_log->order) / sizeof(probe->destroy_log->order[0])) {
        probe->destroy_log->order[probe->destroy_log->count++] = probe->id;
    }
}

static maelys_mcp_middleware_t probe_middleware(const char *name, probe_t *probe) {
    maelys_mcp_middleware_t middleware = {
        .name = name,
        .context = probe,
        .on_authorize = probe_authorize,
        .on_audit = probe_audit,
        .destroy = probe_destroy
    };
    return middleware;
}

/* ---- fixture provider: two tools, one resource, one template ---- */

typedef struct fixture_state {
    int calls;
    int reads;
} fixture_state_t;

static maelys_mcp_result_t fixture_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)out_error;
    fixture_state_t *state = context;
    state->calls++;
    out_result->structured_content = json_pack("{s:s}", "tool", request->tool_name);
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_result_t fixture_read(
    void *context,
    const maelys_mcp_resource_request_t *request,
    maelys_mcp_resource_result_t *out_result,
    char **out_error) {
    fixture_state_t *state = context;
    state->reads++;
    if (strncmp(request->uri, "fx://repo/", 10u) != 0) {
        if (out_error) *out_error = strdup("resource not found");
        return MAELYS_MCP_ERR_NOT_FOUND;
    }
    out_result->contents = json_pack("[{s:s,s:s,s:s}]",
        "uri", request->uri, "mimeType", "text/markdown", "text", "# Doc");
    return out_result->contents ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_provider_t *fixture_provider(fixture_state_t *state) {
    json_t *read_schema = json_pack("{s:s,s:{s:{s:s}},s:[s],s:b}",
        "type", "object", "properties", "message", "type", "string",
        "required", "message", "additionalProperties", 0);
    json_t *empty_schema = json_pack("{s:s,s:b}",
        "type", "object", "additionalProperties", 0);
    if (!read_schema || !empty_schema) {
        if (read_schema) json_decref(read_schema);
        if (empty_schema) json_decref(empty_schema);
        return NULL;
    }
    maelys_mcp_tool_t tools[] = {
        {
            .name = "fx.read",
            .title = "Read",
            .description = "A read-effect tool.",
            .input_schema = read_schema,
            .effect = MAELYS_MCP_EFFECT_READ
        },
        {
            .name = "fx.mutate",
            .title = "Mutate",
            .description = "An apply-effect tool.",
            .input_schema = empty_schema,
            .effect = MAELYS_MCP_EFFECT_APPLY
        }
    };
    static const maelys_mcp_resource_t resources[] = {{
        /* Registered uppercase on purpose: the canonical form is what a
         * decision and a journal must both see. */
        .uri = "FX://repo/doc.mdx",
        .name = "Doc",
        .mime_type = "text/markdown"
    }};
    static const maelys_mcp_resource_template_t templates[] = {{
        .uri_template = "fx://repo/assets/{path}",
        .name = "Assets",
        .mime_type = "application/octet-stream"
    }};
    maelys_mcp_provider_config_t config = {
        .name = "fixture",
        .version = "1",
        .tools = tools,
        .tool_count = 2,
        .resources = resources,
        .resource_count = 1,
        .resource_templates = templates,
        .resource_template_count = 1,
        .call = fixture_call,
        .read_resource = fixture_read,
        .context = state
    };
    maelys_mcp_provider_t *provider = NULL;
    maelys_mcp_result_t status = maelys_mcp_provider_create(&config, &provider);
    json_decref(read_schema);
    json_decref(empty_schema);
    return status == MAELYS_MCP_OK ? provider : NULL;
}

static maelys_mcp_result_t build_runtime(
    fixture_state_t *state,
    maelys_mcp_runtime_t **out_runtime) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "middleware", .server_version = "1.0"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    maelys_mcp_result_t status = maelys_mcp_runtime_create(&config, &runtime);
    if (status != MAELYS_MCP_OK) return status;
    if (maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_MRTR) != MAELYS_MCP_OK ||
        maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_RESOURCES) != MAELYS_MCP_OK) {
        (void)maelys_mcp_runtime_destroy(runtime);
        return MAELYS_MCP_ERR_STATE;
    }
    maelys_mcp_provider_t *provider = fixture_provider(state);
    if (!provider) {
        (void)maelys_mcp_runtime_destroy(runtime);
        return MAELYS_MCP_ERR_MEMORY;
    }
    status = maelys_mcp_runtime_add_provider(runtime, provider, NULL);
    if (status != MAELYS_MCP_OK) {
        maelys_mcp_provider_destroy(provider);
        (void)maelys_mcp_runtime_destroy(runtime);
        return status;
    }
    *out_runtime = runtime;
    return MAELYS_MCP_OK;
}

static json_t *modern_params(void) {
    return json_pack("{s:{s:s,s:{s:s,s:s},s:{s:{},s:{}}}}",
        "_meta",
        "io.modelcontextprotocol/protocolVersion", MAELYS_MCP_PROTOCOL_MODERN,
        "io.modelcontextprotocol/clientInfo", "name", "probe", "version", "1",
        "io.modelcontextprotocol/clientCapabilities", "elicitation", "roots");
}

static json_t *dispatch(
    maelys_mcp_channel_t *channel,
    const char *method,
    json_t *params) {
    json_t *request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 1, "method", method, "params", params);
    if (!request) return NULL;
    maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, request);
    json_decref(request);
    if (status != MAELYS_MCP_OK) return NULL;
    json_t *response = NULL;
    if (maelys_mcp_channel_next(channel, 1000u, &response) != MAELYS_MCP_OK) return NULL;
    return response;
}

static json_t *call_params(const char *name, json_t *arguments) {
    json_t *params = modern_params();
    if (!params) return NULL;
    (void)json_object_set_new(params, "name", json_string(name));
    (void)json_object_set_new(params, "arguments",
        arguments ? arguments : json_object());
    return params;
}

static int error_code(json_t *response) {
    return (int)json_integer_value(
        json_object_get(json_object_get(response, "error"), "code"));
}

static const char *error_message(json_t *response) {
    const char *message = json_string_value(
        json_object_get(json_object_get(response, "error"), "message"));
    return message ? message : "";
}

static size_t authorize_calls_for(
    const probe_t *probe,
    maelys_mcp_operation_t operation) {
    size_t seen = 0;
    for (size_t index = 0; index < probe->authorized_count; ++index) {
        if (probe->authorized[index].operation == operation) ++seen;
    }
    return seen;
}

/*
 * An empty chain is the pre-chain code path: every operation succeeds and
 * nothing is consulted. This is the baseline the cost argument rests on.
 */
static int test_empty_chain_is_transparent(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *response = dispatch(channel, "tools/list", modern_params());
    ASSERT_TRUE(json_array_size(json_object_get(
        json_object_get(response, "result"), "tools")) == 2u);
    json_decref(response);

    response = dispatch(channel, "tools/call",
        call_params("fx.mutate", NULL));
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);

    json_t *params = modern_params();
    (void)json_object_set_new(params, "uri", json_string("fx://repo/doc.mdx"));
    response = dispatch(channel, "resources/read", params);
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    ASSERT_TRUE(state.calls == 1 && state.reads == 1);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

static int test_registration_rules(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    probe_t probe = {0};
    maelys_mcp_middleware_t middleware = probe_middleware("probe", &probe);

    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(NULL, &middleware, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, NULL, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    /* A middleware with no hook would occupy a slot and observe nothing. */
    maelys_mcp_middleware_t hookless = {.name = "hookless"};
    char *error = NULL;
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &hookless, &error) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(error && strcmp(error, "middleware implements no hook") == 0);
    free(error);
    ASSERT_TRUE(maelys_mcp_runtime_add_compat_policy(runtime, NULL, NULL, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);

    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);
    /* The chain is immutable once a channel can dispatch through it. */
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_ERR_STATE);
    ASSERT_TRUE(maelys_mcp_runtime_add_compat_policy(runtime, NULL, NULL, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * Ordering: hook 2 runs front to back and stops at the first non-allow, so a
 * later middleware can never overturn an earlier deny. Hook 5 runs every
 * registered middleware whatever the outcome.
 */
static int test_authorize_order_and_short_circuit(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    probe_t first = {.id = 1, .deny_identity = "fx.mutate"};
    probe_t second = {.id = 2};
    maelys_mcp_middleware_t first_middleware = probe_middleware("first", &first);
    maelys_mcp_middleware_t second_middleware = probe_middleware("second", &second);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &first_middleware, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &second_middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *response = dispatch(channel, "tools/call", call_params("fx.mutate", NULL));
    ASSERT_TRUE(error_code(response) == -32003);
    json_decref(response);
    ASSERT_TRUE(state.calls == 0);
    /* The denier was asked; the middleware behind it never was. */
    ASSERT_TRUE(authorize_calls_for(&first, MAELYS_MCP_OPERATION_CALL) == 1u);
    ASSERT_TRUE(authorize_calls_for(&second, MAELYS_MCP_OPERATION_CALL) == 0u);
    /* Both journalled it. */
    ASSERT_TRUE(first.audited_count == 1u && second.audited_count == 1u);
    ASSERT_TRUE(first.audited[0].outcome == MAELYS_MCP_ERR_DENIED);
    ASSERT_TRUE(second.audited[0].outcome == MAELYS_MCP_ERR_DENIED);

    response = dispatch(channel, "tools/call",
        call_params("fx.read", json_pack("{s:s}", "message", "hello")));
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    ASSERT_TRUE(authorize_calls_for(&second, MAELYS_MCP_OPERATION_CALL) == 1u);
    ASSERT_TRUE(second.audited_count == 2u &&
        second.audited[1].outcome == MAELYS_MCP_OK);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * All five decision points reach the chain - the two on the tools path and
 * the three on the resources path, which is the constraint that made a
 * tools-only conversion unshippable.
 */
static int test_all_five_policy_sites(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    probe_t probe = {.id = 1};
    maelys_mcp_middleware_t middleware = probe_middleware("probe", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *response = dispatch(channel, "tools/list", modern_params());
    ASSERT_TRUE(json_array_size(json_object_get(
        json_object_get(response, "result"), "tools")) == 2u);
    json_decref(response);
    ASSERT_TRUE(authorize_calls_for(&probe, MAELYS_MCP_OPERATION_LIST) == 2u);

    response = dispatch(channel, "tools/call", call_params("fx.mutate", NULL));
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    ASSERT_TRUE(authorize_calls_for(&probe, MAELYS_MCP_OPERATION_CALL) == 1u);

    response = dispatch(channel, "resources/list", modern_params());
    ASSERT_TRUE(json_array_size(json_object_get(
        json_object_get(response, "result"), "resources")) == 1u);
    json_decref(response);
    ASSERT_TRUE(authorize_calls_for(&probe,
        MAELYS_MCP_OPERATION_RESOURCE_LIST) == 1u);

    response = dispatch(channel, "resources/templates/list", modern_params());
    ASSERT_TRUE(json_array_size(json_object_get(
        json_object_get(response, "result"), "resourceTemplates")) == 1u);
    json_decref(response);
    ASSERT_TRUE(authorize_calls_for(&probe,
        MAELYS_MCP_OPERATION_RESOURCE_TEMPLATE_LIST) == 1u);

    json_t *params = modern_params();
    (void)json_object_set_new(params, "uri", json_string("FX://repo/doc.mdx"));
    response = dispatch(channel, "resources/read", params);
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    ASSERT_TRUE(authorize_calls_for(&probe,
        MAELYS_MCP_OPERATION_RESOURCE_READ) == 1u);

    /* Every decision was taken on a resolved, canonical identity. */
    for (size_t index = 0; index < probe.authorized_count; ++index) {
        const probe_event_t *event = &probe.authorized[index];
        if (event->operation == MAELYS_MCP_OPERATION_RESOURCE_LIST ||
            event->operation == MAELYS_MCP_OPERATION_RESOURCE_READ) {
            ASSERT_TRUE(strcmp(event->identity, "fx://repo/doc.mdx") == 0);
        }
        if (event->operation == MAELYS_MCP_OPERATION_RESOURCE_TEMPLATE_LIST) {
            ASSERT_TRUE(strcmp(event->identity, "fx://repo/assets/{path}") == 0);
        }
        if (event->operation != MAELYS_MCP_OPERATION_LIST &&
            event->operation != MAELYS_MCP_OPERATION_CALL) {
            ASSERT_TRUE(event->effect == MAELYS_MCP_EFFECT_READ);
        }
    }

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/* A deny hides a catalog entry and refuses a call or a read with -32003. */
static int test_denial_at_every_site(void) {
    static const char *const denied[] = {
        "fx.mutate", "fx://repo/doc.mdx", "fx://repo/assets/{path}"
    };
    for (size_t index = 0; index < sizeof(denied) / sizeof(denied[0]); ++index) {
        fixture_state_t state = {0};
        maelys_mcp_runtime_t *runtime = NULL;
        ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
        probe_t probe = {.id = 1, .deny_identity = denied[index]};
        maelys_mcp_middleware_t middleware = probe_middleware("deny", &probe);
        ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
            MAELYS_MCP_OK);
        maelys_mcp_channel_t *channel = NULL;
        ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

        json_t *response = dispatch(channel, "tools/list", modern_params());
        ASSERT_TRUE(json_array_size(json_object_get(
            json_object_get(response, "result"), "tools")) ==
            (index == 0u ? 1u : 2u));
        json_decref(response);

        response = dispatch(channel, "resources/list", modern_params());
        ASSERT_TRUE(json_array_size(json_object_get(
            json_object_get(response, "result"), "resources")) ==
            (index == 1u ? 0u : 1u));
        json_decref(response);

        response = dispatch(channel, "resources/templates/list", modern_params());
        ASSERT_TRUE(json_array_size(json_object_get(
            json_object_get(response, "result"), "resourceTemplates")) ==
            (index == 2u ? 0u : 1u));
        json_decref(response);

        response = dispatch(channel, "tools/call", call_params("fx.mutate", NULL));
        if (index == 0u) {
            ASSERT_TRUE(error_code(response) == -32003);
            ASSERT_TRUE(state.calls == 0);
        } else {
            ASSERT_TRUE(json_object_get(response, "result"));
        }
        json_decref(response);

        json_t *params = modern_params();
        (void)json_object_set_new(params, "uri", json_string("fx://repo/doc.mdx"));
        response = dispatch(channel, "resources/read", params);
        if (index == 1u) {
            ASSERT_TRUE(error_code(response) == -32003);
            ASSERT_TRUE(state.reads == 0);
            /* A refused read is journalled, which it was not before the chain. */
            ASSERT_TRUE(probe.audited_count != 0u);
            const probe_event_t *last = &probe.audited[probe.audited_count - 1u];
            ASSERT_TRUE(last->outcome == MAELYS_MCP_ERR_DENIED &&
                last->operation == MAELYS_MCP_OPERATION_RESOURCE_READ &&
                strcmp(last->identity, "fx://repo/doc.mdx") == 0);
        } else {
            ASSERT_TRUE(json_object_get(response, "result"));
        }
        json_decref(response);

        ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    }
    return 0;
}

/*
 * The named reorder: policy decides before schema validation, so a caller who
 * may not use a tool is refused rather than handed its argument schema
 * through a validation error detail.
 */
static int test_policy_precedes_schema_validation(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    probe_t probe = {.id = 1, .deny_identity = "fx.read"};
    maelys_mcp_middleware_t middleware = probe_middleware("deny-read", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    /* fx.read requires a "message" string; this call has none. */
    json_t *response = dispatch(channel, "tools/call", call_params("fx.read", NULL));
    ASSERT_TRUE(error_code(response) == -32003);
    ASSERT_TRUE(!json_object_get(json_object_get(response, "error"), "data"));
    json_decref(response);

    /* An allowed tool still gets its arguments validated. */
    response = dispatch(channel, "tools/call", call_params("fx.mutate",
        json_pack("{s:s}", "surprise", "yes")));
    ASSERT_TRUE(error_code(response) == -32602);
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * The MRTR continuation blind spot: inputResponses and requestState are
 * siblings of arguments, so a hook that only saw a name and its arguments
 * could not see the most sensitive inbound payloads in the protocol.
 */
static int test_authorize_sees_continuation_traffic(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    probe_t probe = {.id = 1};
    maelys_mcp_middleware_t middleware = probe_middleware("probe", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *params = call_params("fx.mutate", NULL);
    (void)json_object_set_new(params, "inputResponses",
        json_pack("{s:{s:s}}", "fx_elicit", "secret", "hunter2"));
    (void)json_object_set_new(params, "requestState", json_string("state-1"));
    json_t *response = dispatch(channel, "tools/call", params);
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    ASSERT_TRUE(probe.authorized_count == 1u);
    ASSERT_TRUE(probe.authorized[0].saw_params);
    ASSERT_TRUE(probe.authorized[0].saw_input_responses);
    ASSERT_TRUE(probe.authorized[0].saw_request_state);

    params = modern_params();
    (void)json_object_set_new(params, "uri", json_string("fx://repo/doc.mdx"));
    (void)json_object_set_new(params, "inputResponses",
        json_pack("{s:{s:s}}", "fx_elicit", "secret", "hunter2"));
    (void)json_object_set_new(params, "requestState", json_string("state-2"));
    response = dispatch(channel, "resources/read", params);
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    ASSERT_TRUE(probe.authorized_count == 2u);
    ASSERT_TRUE(probe.authorized[1].saw_input_responses);
    ASSERT_TRUE(probe.authorized[1].saw_request_state);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * A middleware that cannot decide is not a middleware that denies: the two
 * map to different JSON-RPC outcomes, and an unevaluated catalog entry is
 * never silently omitted.
 */
static int test_authorize_error_is_not_a_denial(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    probe_t probe = {.id = 1, .error_identity = "fx.mutate"};
    maelys_mcp_middleware_t middleware = probe_middleware("broken", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *response = dispatch(channel, "tools/call", call_params("fx.mutate", NULL));
    ASSERT_TRUE(error_code(response) == -32603);
    ASSERT_TRUE(strcmp(error_message(response), "Policy evaluation failed") == 0);
    json_decref(response);
    ASSERT_TRUE(state.calls == 0);
    ASSERT_TRUE(probe.audited_count == 1u &&
        probe.audited[0].outcome == MAELYS_MCP_ERR_STATE);

    /* The whole listing fails rather than quietly losing the entry. */
    response = dispatch(channel, "tools/list", modern_params());
    ASSERT_TRUE(error_code(response) == -32603);
    ASSERT_TRUE(strcmp(error_message(response), "Policy evaluation failed") == 0);
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * Hook 5 carries both views. On the resource path they genuinely differ
 * today, because normalization rewrites the URI the client sent.
 */
static int test_audit_carries_both_views(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    probe_t probe = {.id = 1};
    maelys_mcp_middleware_t middleware = probe_middleware("journal", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *params = modern_params();
    (void)json_object_set_new(params, "uri", json_string("FX://repo/a/../doc.mdx"));
    json_t *response = dispatch(channel, "resources/read", params);
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    ASSERT_TRUE(probe.audited_count == 1u);
    ASSERT_TRUE(strcmp(probe.audited[0].requested, "FX://repo/a/../doc.mdx") == 0);
    ASSERT_TRUE(strcmp(probe.audited[0].identity, "fx://repo/doc.mdx") == 0);
    ASSERT_TRUE(probe.audited[0].outcome == MAELYS_MCP_OK);
    ASSERT_TRUE(probe.audited[0].saw_params);

    response = dispatch(channel, "tools/call", call_params("fx.mutate", NULL));
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    ASSERT_TRUE(probe.audited_count == 2u);
    ASSERT_TRUE(strcmp(probe.audited[1].requested, "fx.mutate") == 0);
    ASSERT_TRUE(strcmp(probe.audited[1].identity, "fx.mutate") == 0);
    ASSERT_TRUE(probe.audited[1].effect == MAELYS_MCP_EFFECT_APPLY);
    /* Listing is not journalled, exactly as before the chain. */
    response = dispatch(channel, "tools/list", modern_params());
    json_decref(response);
    ASSERT_TRUE(probe.audited_count == 2u);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * The per-channel opaque context is the only anchor a decision may rest on:
 * one runtime, two channels, two principals, two answers.
 */
static int test_decision_uses_channel_context(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    int privileged = 1;
    int anonymous = 0;
    probe_t probe = {.id = 1, .required_context = &privileged, .required_context_set = 1};
    maelys_mcp_middleware_t middleware = probe_middleware("per-principal", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_config_t privileged_config = {.context = &privileged};
    maelys_mcp_channel_config_t anonymous_config = {.context = &anonymous};
    maelys_mcp_channel_t *allowed = NULL;
    maelys_mcp_channel_t *refused = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &privileged_config, &allowed) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &anonymous_config, &refused) ==
        MAELYS_MCP_OK);

    json_t *response = dispatch(allowed, "tools/call", call_params("fx.mutate", NULL));
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    response = dispatch(refused, "tools/call", call_params("fx.mutate", NULL));
    ASSERT_TRUE(error_code(response) == -32003);
    json_decref(response);
    ASSERT_TRUE(state.calls == 1);
    ASSERT_TRUE(probe.authorized_count == 2u);
    ASSERT_TRUE(probe.authorized[0].channel_context == &privileged);
    ASSERT_TRUE(probe.authorized[1].channel_context == &anonymous);

    /* The catalog differs per channel for the same reason. */
    response = dispatch(refused, "tools/list", modern_params());
    ASSERT_TRUE(json_array_size(json_object_get(
        json_object_get(response, "result"), "tools")) == 0u);
    json_decref(response);
    response = dispatch(allowed, "tools/list", modern_params());
    ASSERT_TRUE(json_array_size(json_object_get(
        json_object_get(response, "result"), "tools")) == 2u);
    json_decref(response);

    ASSERT_TRUE(maelys_mcp_channel_destroy(allowed) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_channel_destroy(refused) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/* ---- the compatibility middleware ---- */

typedef struct compat_state {
    int authorized;
    int audited;
    char last_audited_identity[128];
    maelys_mcp_operation_t last_audited_operation;
    maelys_mcp_result_t last_outcome;
} compat_state_t;

static int compat_authorize(
    void *context,
    const maelys_mcp_request_context_t *request) {
    compat_state_t *state = context;
    state->authorized++;
    /* The pre-chain host policy, verbatim: an effect allowlist. */
    return request->effect == MAELYS_MCP_EFFECT_READ ||
        request->effect == MAELYS_MCP_EFFECT_PREVIEW;
}

static void compat_audit(
    void *context,
    const maelys_mcp_request_context_t *request,
    maelys_mcp_result_t outcome) {
    compat_state_t *state = context;
    state->audited++;
    record_identity(state->last_audited_identity,
        sizeof(state->last_audited_identity),
        request->tool_name ? request->tool_name : request->resource_uri);
    state->last_audited_operation = request->operation;
    state->last_outcome = outcome;
}

static int test_compat_policy_reproduces_the_old_surface(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    compat_state_t policy = {0};
    ASSERT_TRUE(maelys_mcp_runtime_add_compat_policy(runtime, compat_authorize,
        compat_audit, &policy) == MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    /* The apply-effect tool is refused and its refusal journalled. */
    json_t *response = dispatch(channel, "tools/call", call_params("fx.mutate", NULL));
    ASSERT_TRUE(error_code(response) == -32003);
    json_decref(response);
    ASSERT_TRUE(policy.audited == 1 &&
        policy.last_outcome == MAELYS_MCP_ERR_DENIED &&
        strcmp(policy.last_audited_identity, "fx.mutate") == 0);

    /* The read-effect tool runs and is journalled with its outcome. */
    response = dispatch(channel, "tools/call",
        call_params("fx.read", json_pack("{s:s}", "message", "hi")));
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    ASSERT_TRUE(policy.audited == 2 && policy.last_outcome == MAELYS_MCP_OK);

    /* The catalog is filtered by the same callback, as it always was. */
    response = dispatch(channel, "tools/list", modern_params());
    ASSERT_TRUE(json_array_size(json_object_get(
        json_object_get(response, "result"), "tools")) == 1u);
    json_decref(response);

    /* Resources reach the same callback: the three sites are not lost. */
    json_t *params = modern_params();
    (void)json_object_set_new(params, "uri", json_string("fx://repo/doc.mdx"));
    response = dispatch(channel, "resources/read", params);
    ASSERT_TRUE(json_object_get(response, "result"));
    json_decref(response);
    ASSERT_TRUE(policy.audited == 3 &&
        policy.last_audited_operation == MAELYS_MCP_OPERATION_RESOURCE_READ &&
        strcmp(policy.last_audited_identity, "fx://repo/doc.mdx") == 0);
    ASSERT_TRUE(policy.authorized >= 5);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/* Teardown runs once per middleware, in reverse registration order. */
static int test_destroy_runs_in_reverse_order(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    destroy_log_t log = {0};
    probe_t first = {.id = 1, .destroy_log = &log};
    probe_t second = {.id = 2, .destroy_log = &log};
    maelys_mcp_middleware_t first_middleware = probe_middleware("first", &first);
    maelys_mcp_middleware_t second_middleware = probe_middleware("second", &second);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &first_middleware, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &second_middleware, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(log.count == 0u);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(log.count == 2u && log.order[0] == 2 && log.order[1] == 1);
    return 0;
}

int main(void) {
    static const maelys_test_case_t cases[] = {
        {"empty chain is transparent", test_empty_chain_is_transparent},
        {"registration rules", test_registration_rules},
        {"authorize order and short circuit", test_authorize_order_and_short_circuit},
        {"all five policy sites", test_all_five_policy_sites},
        {"denial at every site", test_denial_at_every_site},
        {"policy precedes schema validation", test_policy_precedes_schema_validation},
        {"authorize sees continuation traffic", test_authorize_sees_continuation_traffic},
        {"authorize error is not a denial", test_authorize_error_is_not_a_denial},
        {"audit carries both views", test_audit_carries_both_views},
        {"decision uses channel context", test_decision_uses_channel_context},
        {"compat policy reproduces the old surface",
            test_compat_policy_reproduces_the_old_surface},
        {"destroy runs in reverse order", test_destroy_runs_in_reverse_order}
    };
    int failures = maelys_run_tests(cases, sizeof(cases) / sizeof(cases[0]));
    if (failures == 0) puts("test_middleware: OK");
    return failures;
}
