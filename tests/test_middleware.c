#include "maelys/mcp.h"
#include "tests/test_support.h"

#include <pthread.h>
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
    /* Whether the params this hook received carry the argument hook 1
     * injects - they must not, which is the whole point of hook 5 keeping
     * the client's view. */
    int saw_injected_argument;
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
    event->saw_injected_argument = json_object_get(
        json_object_get(record->params, "arguments"), "message") != NULL;
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
    int progress_reports;
} fixture_state_t;

static maelys_mcp_result_t fixture_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)out_error;
    fixture_state_t *state = context;
    state->calls++;
    /*
     * Reported unconditionally: a NULL reporter is the documented way to say
     * "the client asked for none", so this is a no-op for every call that
     * carries no progressToken - which is every call in this file except the
     * hook 6 tests, where the frames are the point.
     */
    for (int step = 0; step <= 2; ++step) {
        if (maelys_mcp_provider_report_progress(request->progress,
            step * 50.0, 100.0, NULL) == MAELYS_MCP_OK && request->progress) {
            state->progress_reports++;
        }
    }
    /* The resolved arguments, echoed back so a test can see what hook 1 did
     * to them by the time the provider ran. */
    out_result->structured_content = json_pack("{s:s,s:O}",
        "tool", request->tool_name, "arguments", request->arguments);
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

/*
 * Tears a half-built runtime down and reports the failure that caused it.
 * A helper rather than a (void) cast because GCC's -Wunused-result does not
 * honour one on a warn_unused_result function, so the teardown status has to
 * be captured and folded in.
 */
static maelys_mcp_result_t abandon_runtime(
    maelys_mcp_runtime_t *runtime,
    maelys_mcp_result_t failure) {
    maelys_mcp_result_t destroyed = maelys_mcp_runtime_destroy(runtime);
    return failure != MAELYS_MCP_OK ? failure : destroyed;
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
        return abandon_runtime(runtime, MAELYS_MCP_ERR_STATE);
    }
    maelys_mcp_provider_t *provider = fixture_provider(state);
    if (!provider) {
        return abandon_runtime(runtime, MAELYS_MCP_ERR_MEMORY);
    }
    status = maelys_mcp_runtime_add_provider(runtime, provider, NULL);
    if (status != MAELYS_MCP_OK) {
        maelys_mcp_provider_destroy(provider);
        return abandon_runtime(runtime, status);
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

/* ---- the transformation hooks: 1, 3, 4, 6 and 7 ---- */

/*
 * One configurable transform standing in for all four request- and
 * result-side hooks, in the same spirit as probe_t above: each test sets the
 * two or three fields its scenario needs and reads back what the chain saw.
 */
typedef struct transform {
    int id;
    /* hook 1 */
    const char *rename_from;
    const char *rename_to;
    const char *inject_message;
    int coerce_message;
    int resolve_fails;
    int resolve_returns_array;
    /* hook 3 */
    const char *substitute_text;
    int substitute_empty;
    int call_fails;
    /* hook 4 */
    const char *result_text;
    int result_empty;
    int result_fails;
    /* hook 7 */
    const char *list_rename_from;
    const char *list_rename_to;
    const char *list_add;
    int list_fails;
    int list_returns_object;
    /* what the hooks observed */
    int resolves;
    int calls;
    int results;
    int lists;
    char last_resolved[128];
    char last_called[128];
    char last_requested[128];
    int call_saw_injected_message;
    size_t last_list_size;
    maelys_mcp_catalog_kind_t last_catalog;
} transform_t;

static maelys_mcp_result_t transform_resolve(
    void *context,
    const maelys_mcp_resolve_context_t *request,
    maelys_mcp_resolution_t *out_resolution) {
    transform_t *transform = context;
    transform->resolves++;
    record_identity(transform->last_resolved, sizeof(transform->last_resolved),
        request->tool_name);
    if (transform->resolve_fails) return MAELYS_MCP_ERR_STATE;
    if (transform->resolve_returns_array) {
        /* Not an object: refused by the chain rather than handed to the
         * validator, which could only blame the caller for it. */
        out_resolution->arguments = json_array();
        return MAELYS_MCP_OK;
    }
    if (transform->rename_from && request->tool_name &&
        strcmp(request->tool_name, transform->rename_from) == 0) {
        out_resolution->tool_name = strdup(transform->rename_to);
        if (!out_resolution->tool_name) return MAELYS_MCP_ERR_MEMORY;
    }
    /*
     * The hidden-argument shape, and the reason hook 5 keeps the client's
     * view: what is injected here is the canonical API key of every
     * transformation tutorial, and an audit built on the resolved arguments
     * would write it to a log file.
     */
    if (transform->inject_message) {
        json_t *arguments = json_deep_copy(request->arguments);
        if (!arguments || json_object_set_new(arguments, "message",
            json_string(transform->inject_message)) != 0) {
            if (arguments) json_decref(arguments);
            return MAELYS_MCP_ERR_MEMORY;
        }
        out_resolution->arguments = arguments;
    }
    /*
     * The type conversion the design names: the published schema may say
     * "integer", the real one says "string", and only a conversion here makes
     * the call validate - relabelling it in hook 7 alone cannot.
     */
    if (transform->coerce_message &&
        json_is_integer(json_object_get(request->arguments, "message"))) {
        char buffer[32];
        (void)snprintf(buffer, sizeof(buffer), "%lld",
            json_integer_value(json_object_get(request->arguments, "message")));
        json_t *arguments = json_deep_copy(request->arguments);
        if (!arguments || json_object_set_new(arguments, "message",
            json_string(buffer)) != 0) {
            if (arguments) json_decref(arguments);
            return MAELYS_MCP_ERR_MEMORY;
        }
        out_resolution->arguments = arguments;
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_call_disposition_t transform_call(
    void *context,
    const maelys_mcp_call_context_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    transform_t *transform = context;
    transform->calls++;
    record_identity(transform->last_called, sizeof(transform->last_called),
        request->tool_name);
    record_identity(transform->last_requested, sizeof(transform->last_requested),
        request->requested_tool_name);
    transform->call_saw_injected_message = json_is_string(
        json_object_get(request->arguments, "message"));
    if (transform->call_fails) {
        if (out_error) *out_error = strdup("cache is on fire");
        return MAELYS_MCP_CALL_ERROR;
    }
    if (transform->substitute_empty) {
        /* Structurally invalid: a substitution answers to the provider's
         * contract, so this must be refused rather than serialized. */
        return MAELYS_MCP_CALL_SUBSTITUTE;
    }
    if (!transform->substitute_text) return MAELYS_MCP_CALL_INVOKE;
    out_result->content = json_pack("[{s:s,s:s}]", "type", "text",
        "text", transform->substitute_text);
    return out_result->content ?
        MAELYS_MCP_CALL_SUBSTITUTE : MAELYS_MCP_CALL_ERROR;
}

static maelys_mcp_result_disposition_t transform_result(
    void *context,
    const maelys_mcp_result_context_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    transform_t *transform = context;
    transform->results++;
    record_identity(transform->last_called, sizeof(transform->last_called),
        request->tool_name);
    record_identity(transform->last_requested, sizeof(transform->last_requested),
        request->requested_tool_name);
    if (transform->result_fails) {
        if (out_error) *out_error = strdup("redactor unavailable");
        return MAELYS_MCP_RESULT_ERROR;
    }
    if (transform->result_empty) return MAELYS_MCP_RESULT_REPLACED;
    if (!transform->result_text) return MAELYS_MCP_RESULT_UNCHANGED;
    /* Chained: each hook 4 appends to what the previous one produced, so the
     * final text records the order they ran in. */
    const char *previous = json_string_value(json_object_get(
        json_array_get(request->result ? request->result->content : NULL, 0),
        "text"));
    char buffer[256];
    (void)snprintf(buffer, sizeof(buffer), "%s%s%s", previous ? previous : "",
        previous ? "|" : "", transform->result_text);
    out_result->content = json_pack("[{s:s,s:s}]", "type", "text",
        "text", buffer);
    return out_result->content ?
        MAELYS_MCP_RESULT_REPLACED : MAELYS_MCP_RESULT_ERROR;
}

static const char *entry_identity(json_t *entry) {
    json_t *value = json_object_get(entry, "name");
    if (!json_is_string(value)) value = json_object_get(entry, "uri");
    if (!json_is_string(value)) value = json_object_get(entry, "uriTemplate");
    return json_string_value(value);
}

static maelys_mcp_result_t transform_list(
    void *context,
    const maelys_mcp_list_context_t *request,
    json_t **out_entries) {
    transform_t *transform = context;
    transform->lists++;
    transform->last_catalog = request->catalog;
    transform->last_list_size = json_array_size(request->entries);
    if (transform->list_fails) return MAELYS_MCP_ERR_STATE;
    if (transform->list_returns_object) {
        *out_entries = json_object();
        return MAELYS_MCP_OK;
    }
    if (!transform->list_rename_from && !transform->list_add) {
        return MAELYS_MCP_OK;
    }
    json_t *entries = json_deep_copy(request->entries);
    if (!entries) return MAELYS_MCP_ERR_MEMORY;
    size_t index;
    json_t *entry;
    json_array_foreach(entries, index, entry) {
        const char *identity = entry_identity(entry);
        if (transform->list_rename_from && identity &&
            strcmp(identity, transform->list_rename_from) == 0) {
            const char *key = json_object_get(entry, "name") ? "name" :
                (json_object_get(entry, "uri") ? "uri" : "uriTemplate");
            if (json_object_set_new(entry, key,
                json_string(transform->list_rename_to)) != 0) {
                json_decref(entries);
                return MAELYS_MCP_ERR_MEMORY;
            }
        }
    }
    if (transform->list_add) {
        /* A synthetic entry: no registry backing, which is what a proxy or a
         * retrieval-first meta-tool needs. */
        json_t *synthetic = json_pack("{s:s,s:s}", "name", transform->list_add,
            "description", "Invented by hook 7.");
        if (!synthetic || json_array_append_new(entries, synthetic) != 0) {
            if (synthetic) json_decref(synthetic);
            json_decref(entries);
            return MAELYS_MCP_ERR_MEMORY;
        }
    }
    *out_entries = entries;
    return MAELYS_MCP_OK;
}

static maelys_mcp_middleware_t transform_middleware(
    const char *name,
    transform_t *transform) {
    maelys_mcp_middleware_t middleware = {
        .name = name,
        .context = transform,
        .on_resolve = transform_resolve,
        .on_call = transform_call,
        .on_result = transform_result,
        .on_list = transform_list
    };
    return middleware;
}

static const char *result_text(json_t *response) {
    json_t *content = json_object_get(json_object_get(response, "result"), "content");
    const char *text = json_string_value(
        json_object_get(json_array_get(content, 0), "text"));
    return text ? text : "";
}

static json_t *listed(json_t *response, const char *key) {
    return json_object_get(json_object_get(response, "result"), key);
}

static int lists_entry(json_t *entries, const char *identity) {
    size_t index;
    json_t *entry;
    json_array_foreach(entries, index, entry) {
        const char *value = entry_identity(entry);
        if (value && strcmp(value, identity) == 0) return 1;
    }
    return 0;
}

/*
 * Hook 1's headline: a client-facing name maps to a real tool and a hidden
 * argument is injected from the middleware's own state. What the client asked
 * for and what ran are both journalled - and the injected value is in neither
 * the journal nor the params hook 5 receives, which is the property that
 * keeps a credential out of a log file.
 */
static int test_resolve_renames_and_injects(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    transform_t transform = {
        .id = 1, .rename_from = "edit_doc", .rename_to = "fx.read",
        .inject_message = "hunter2"
    };
    probe_t probe = {.id = 2};
    maelys_mcp_middleware_t transformer = transform_middleware("transform", &transform);
    maelys_mcp_middleware_t journal = probe_middleware("journal", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &transformer, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &journal, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    /* The client calls a name no provider has ever registered, with no
     * arguments at all - and the real tool requires one. */
    json_t *response = dispatch(channel, "tools/call", call_params("edit_doc", NULL));
    ASSERT_TRUE(json_object_get(response, "result"));
    json_t *structured = json_object_get(
        json_object_get(response, "result"), "structuredContent");
    ASSERT_TRUE(strcmp(json_string_value(
        json_object_get(structured, "tool")), "fx.read") == 0);
    ASSERT_TRUE(strcmp(json_string_value(json_object_get(
        json_object_get(structured, "arguments"), "message")), "hunter2") == 0);
    json_decref(response);
    ASSERT_TRUE(state.calls == 1);
    ASSERT_TRUE(transform.resolves == 1);
    ASSERT_TRUE(strcmp(transform.last_resolved, "edit_doc") == 0);

    /* Hook 2 decided on the resolved identity and its real effect. */
    ASSERT_TRUE(probe.authorized_count == 1u);
    ASSERT_TRUE(strcmp(probe.authorized[0].identity, "fx.read") == 0);
    ASSERT_TRUE(probe.authorized[0].effect == MAELYS_MCP_EFFECT_READ);

    /* Hook 5's two views finally differ, which is what the pair was for. */
    ASSERT_TRUE(probe.audited_count == 1u);
    ASSERT_TRUE(strcmp(probe.audited[0].requested, "edit_doc") == 0);
    ASSERT_TRUE(strcmp(probe.audited[0].identity, "fx.read") == 0);
    ASSERT_TRUE(probe.audited[0].outcome == MAELYS_MCP_OK);
    /* And the injected secret reached the provider without reaching the
     * journal: hook 5 sees the client's params, which never carried it. */
    ASSERT_TRUE(!probe.audited[0].saw_injected_argument);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * The precision the design draws from FastMCP's own examples: validation runs
 * against the REAL tool's schema, so a transform that changes an argument's
 * type must convert it in hook 1. Hook 7 alone republishes a lie and every
 * call fails.
 */
static int test_type_change_must_convert_in_resolve(void) {
    for (int converts = 0; converts <= 1; ++converts) {
        fixture_state_t state = {0};
        maelys_mcp_runtime_t *runtime = NULL;
        ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
        /* Both runs publish the same relabelled catalog; only the second one
         * also converts. */
        transform_t transform = {.id = 1, .coerce_message = converts};
        maelys_mcp_middleware_t middleware =
            transform_middleware("retype", &transform);
        if (!converts) middleware.on_resolve = NULL;
        ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
            MAELYS_MCP_OK);
        maelys_mcp_channel_t *channel = NULL;
        ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

        json_t *response = dispatch(channel, "tools/call",
            call_params("fx.read", json_pack("{s:i}", "message", 42)));
        if (converts) {
            ASSERT_TRUE(json_object_get(response, "result"));
            ASSERT_TRUE(state.calls == 1);
        } else {
            ASSERT_TRUE(error_code(response) == -32602);
            ASSERT_TRUE(state.calls == 0);
        }
        json_decref(response);

        ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    }
    return 0;
}

/*
 * Transformation is presentation, never privilege: a name hook 7 published
 * and hook 1 maps onto an apply-effect tool is still an apply when hook 2 sees
 * it. This is the reason policy is ordered after resolution.
 */
static int test_rename_cannot_route_around_a_deny(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    transform_t transform = {
        .id = 1, .rename_from = "safe_read", .rename_to = "fx.mutate",
        .list_rename_from = "fx.mutate", .list_rename_to = "safe_read"
    };
    probe_t probe = {.id = 2, .deny_identity = "fx.mutate"};
    maelys_mcp_middleware_t transformer = transform_middleware("rename", &transform);
    maelys_mcp_middleware_t policy = probe_middleware("policy", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &transformer, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &policy, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    /* The deny already hides it from the catalog, so hook 7 renames nothing;
     * the client calls the gentler name anyway. */
    json_t *response = dispatch(channel, "tools/call", call_params("safe_read", NULL));
    ASSERT_TRUE(error_code(response) == -32003);
    json_decref(response);
    ASSERT_TRUE(state.calls == 0);
    ASSERT_TRUE(strcmp(probe.authorized[0].identity, "fx.mutate") == 0);
    ASSERT_TRUE(probe.authorized[0].effect == MAELYS_MCP_EFFECT_APPLY);
    /* The journal keeps the spelling the client used. */
    ASSERT_TRUE(probe.audited_count == 1u &&
        strcmp(probe.audited[0].requested, "safe_read") == 0 &&
        strcmp(probe.audited[0].identity, "fx.mutate") == 0);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/* A hook 1 that fails, and a hook 1 that resolves to nothing. */
static int test_resolve_failures(void) {
    static const int cases = 3;
    for (int index = 0; index < cases; ++index) {
        fixture_state_t state = {0};
        maelys_mcp_runtime_t *runtime = NULL;
        ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
        transform_t transform = {
            .id = 1,
            .resolve_fails = index == 0,
            .resolve_returns_array = index == 1,
            .rename_from = index == 2 ? "fx.mutate" : NULL,
            .rename_to = "fx.nowhere"
        };
        probe_t probe = {.id = 2};
        maelys_mcp_middleware_t transformer =
            transform_middleware("broken", &transform);
        maelys_mcp_middleware_t journal = probe_middleware("journal", &probe);
        ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &transformer, NULL) ==
            MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &journal, NULL) ==
            MAELYS_MCP_OK);
        maelys_mcp_channel_t *channel = NULL;
        ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

        json_t *response = dispatch(channel, "tools/call",
            call_params("fx.mutate", NULL));
        if (index == 2) {
            /* A rename onto nothing is the client's "Unknown tool", exactly
             * as an unregistered name has always been. */
            ASSERT_TRUE(error_code(response) == -32602);
            ASSERT_TRUE(strcmp(error_message(response), "Unknown tool") == 0);
            ASSERT_TRUE(probe.audited_count == 0u);
        } else {
            ASSERT_TRUE(error_code(response) == -32603);
            ASSERT_TRUE(strcmp(error_message(response),
                "Request transformation failed") == 0);
            /* A middleware fault is journalled, and against the client's
             * name: nothing resolved, so there is no other name to record. */
            ASSERT_TRUE(probe.audited_count == 1u);
            ASSERT_TRUE(probe.audited[0].outcome == MAELYS_MCP_ERR_STATE);
            ASSERT_TRUE(strcmp(probe.audited[0].requested, "fx.mutate") == 0);
        }
        json_decref(response);
        ASSERT_TRUE(state.calls == 0);
        /* Never a denial: the caller was not refused, the chain broke. */
        ASSERT_TRUE(probe.authorized_count == 0u);

        ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    }
    return 0;
}

/*
 * Hook 3 answers in the provider's place, and answers to the provider's
 * contract: the first substitution wins, the provider is never reached, and a
 * substituted result that is not a result is refused rather than serialized.
 */
static int test_call_substitutes_for_the_provider(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    transform_t cache = {.id = 1, .substitute_text = "from the cache"};
    transform_t behind = {.id = 2};
    probe_t probe = {.id = 3};
    maelys_mcp_middleware_t cache_middleware = transform_middleware("cache", &cache);
    maelys_mcp_middleware_t behind_middleware = transform_middleware("behind", &behind);
    maelys_mcp_middleware_t journal = probe_middleware("journal", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &cache_middleware, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &behind_middleware, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &journal, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *response = dispatch(channel, "tools/call",
        call_params("fx.read", json_pack("{s:s}", "message", "hi")));
    ASSERT_TRUE(strcmp(result_text(response), "from the cache") == 0);
    json_decref(response);
    ASSERT_TRUE(state.calls == 0);
    ASSERT_TRUE(cache.calls == 1);
    /* The middleware behind the substitution was never asked. */
    ASSERT_TRUE(behind.calls == 0);
    /* But its hook 4 still runs: the result exists and is transformable. */
    ASSERT_TRUE(behind.results == 1);
    ASSERT_TRUE(strcmp(cache.last_called, "fx.read") == 0);
    ASSERT_TRUE(cache.call_saw_injected_message);
    ASSERT_TRUE(probe.audited_count == 1u &&
        probe.audited[0].outcome == MAELYS_MCP_OK);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/* A hook 3 that fails, and one that substitutes something that is not a result. */
static int test_call_substitution_failures(void) {
    for (int index = 0; index <= 1; ++index) {
        fixture_state_t state = {0};
        maelys_mcp_runtime_t *runtime = NULL;
        ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
        transform_t transform = {
            .id = 1, .call_fails = index == 0, .substitute_empty = index == 1
        };
        probe_t probe = {.id = 2};
        maelys_mcp_middleware_t middleware =
            transform_middleware("broken", &transform);
        maelys_mcp_middleware_t journal = probe_middleware("journal", &probe);
        ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
            MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &journal, NULL) ==
            MAELYS_MCP_OK);
        maelys_mcp_channel_t *channel = NULL;
        ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

        json_t *response = dispatch(channel, "tools/call",
            call_params("fx.mutate", NULL));
        if (index == 0) {
            ASSERT_TRUE(error_code(response) == -32603);
            /* The hook's own diagnostic, not a generic one. */
            ASSERT_TRUE(strcmp(error_message(response), "cache is on fire") == 0);
        } else {
            /* An empty substitution fails result validation, so it surfaces
             * as a tool error rather than as a response with no content. */
            ASSERT_TRUE(json_is_true(json_object_get(
                json_object_get(response, "result"), "isError")));
        }
        json_decref(response);
        ASSERT_TRUE(state.calls == 0);
        ASSERT_TRUE(probe.audited_count == 1u &&
            probe.audited[0].outcome != MAELYS_MCP_OK);

        ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    }
    return 0;
}

/* Hook 4 rewrites the result, and two of them compose in registration order. */
static int test_result_rewrites_in_order(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    transform_t first = {.id = 1, .result_text = "first"};
    transform_t second = {.id = 2, .result_text = "second"};
    maelys_mcp_middleware_t first_middleware = transform_middleware("first", &first);
    maelys_mcp_middleware_t second_middleware = transform_middleware("second", &second);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &first_middleware, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &second_middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *response = dispatch(channel, "tools/call", call_params("fx.mutate", NULL));
    /* The provider produced structuredContent only; the first hook replaced
     * it with text, and the second appended to that text. */
    ASSERT_TRUE(strcmp(result_text(response), "first|second") == 0);
    json_decref(response);
    ASSERT_TRUE(state.calls == 1);
    ASSERT_TRUE(first.results == 1 && second.results == 1);
    ASSERT_TRUE(strcmp(second.last_called, "fx.mutate") == 0);
    ASSERT_TRUE(strcmp(second.last_requested, "fx.mutate") == 0);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/* A hook 4 that fails, and one whose replacement is not a serializable result. */
static int test_result_transformation_failures(void) {
    for (int index = 0; index <= 1; ++index) {
        fixture_state_t state = {0};
        maelys_mcp_runtime_t *runtime = NULL;
        ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
        transform_t transform = {
            .id = 1, .result_fails = index == 0, .result_empty = index == 1
        };
        probe_t probe = {.id = 2};
        maelys_mcp_middleware_t middleware =
            transform_middleware("broken", &transform);
        maelys_mcp_middleware_t journal = probe_middleware("journal", &probe);
        ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
            MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &journal, NULL) ==
            MAELYS_MCP_OK);
        maelys_mcp_channel_t *channel = NULL;
        ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

        json_t *response = dispatch(channel, "tools/call",
            call_params("fx.mutate", NULL));
        /* Either way the provider already ran; what the client gets is an
         * error result, never a response the middleware quietly emptied. */
        ASSERT_TRUE(state.calls == 1);
        ASSERT_TRUE(json_is_true(json_object_get(
            json_object_get(response, "result"), "isError")));
        if (index == 0) {
            ASSERT_TRUE(strstr(result_text(response), "redactor unavailable"));
        }
        json_decref(response);
        ASSERT_TRUE(probe.audited_count == 1u &&
            probe.audited[0].outcome != MAELYS_MCP_OK);

        ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    }
    return 0;
}

/* ---- hook 4 versus the tool's output schema ---- */

typedef struct redact_state {
    int replaced;
} redact_state_t;

static maelys_mcp_result_disposition_t redact_secret(
    void *context,
    const maelys_mcp_result_context_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)out_error;
    redact_state_t *state = context;
    /* The schema is passed in precisely so a hook can see what it would have
     * to satisfy - and this one deliberately does not. */
    if (!request->output_schema) return MAELYS_MCP_RESULT_UNCHANGED;
    json_t *structured = json_deep_copy(request->result->structured_content);
    if (!structured) return MAELYS_MCP_RESULT_ERROR;
    (void)json_object_del(structured, "secret");
    out_result->structured_content = structured;
    state->replaced++;
    return MAELYS_MCP_RESULT_REPLACED;
}

typedef struct schema_state {
    int honest;
} schema_state_t;

static maelys_mcp_result_t schema_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)request;
    (void)out_error;
    schema_state_t *state = context;
    out_result->structured_content = state->honest ?
        json_pack("{s:s,s:s}", "secret", "sk-live", "public", "ok") :
        json_pack("{s:s}", "public", "ok");
    return out_result->structured_content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

/*
 * The named hole in the design, made explicit rather than left to chance. The
 * runtime checks the provider's result against the real output schema; a hook
 * 4 replacement is re-checked structurally but not against that schema,
 * because redacting a field the schema requires is what redaction means.
 */
static int test_result_redaction_versus_output_schema(void) {
    for (int honest = 0; honest <= 1; ++honest) {
        json_t *input_schema = json_pack("{s:s,s:b}",
            "type", "object", "additionalProperties", 0);
        json_t *output_schema = json_pack("{s:s,s:{s:{s:s},s:{s:s}},s:[s,s]}",
            "type", "object", "properties",
            "secret", "type", "string", "public", "type", "string",
            "required", "secret", "public");
        ASSERT_TRUE(input_schema && output_schema);
        schema_state_t provider_state = {.honest = honest};
        maelys_mcp_tool_t tool = {
            .name = "sx.report",
            .title = "Report",
            .description = "Returns a secret and a public field.",
            .input_schema = input_schema,
            .output_schema = output_schema,
            .effect = MAELYS_MCP_EFFECT_READ
        };
        maelys_mcp_provider_config_t provider_config = {
            .name = "schema-provider", .version = "1",
            .tools = &tool, .tool_count = 1,
            .call = schema_call, .context = &provider_state
        };
        maelys_mcp_provider_t *provider = NULL;
        maelys_mcp_result_t created =
            maelys_mcp_provider_create(&provider_config, &provider);
        json_decref(input_schema);
        json_decref(output_schema);
        ASSERT_TRUE(created == MAELYS_MCP_OK);

        maelys_mcp_runtime_config_t config = {
            .server_name = "schema", .server_version = "1"
        };
        maelys_mcp_runtime_t *runtime = NULL;
        ASSERT_TRUE(maelys_mcp_runtime_create(&config, &runtime) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_enable_module(runtime,
            MAELYS_MCP_MODULE_TOOLS) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_add_provider(runtime, provider, NULL) ==
            MAELYS_MCP_OK);
        redact_state_t redactor = {0};
        maelys_mcp_middleware_t middleware = {
            .name = "redactor", .context = &redactor, .on_result = redact_secret
        };
        ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
            MAELYS_MCP_OK);
        maelys_mcp_channel_t *channel = NULL;
        ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

        json_t *response = dispatch(channel, "tools/call",
            call_params("sx.report", NULL));
        if (honest) {
            /* The replacement drops a required field and is delivered anyway:
             * the schema check the runtime enforces is on the provider. */
            json_t *structured = json_object_get(
                json_object_get(response, "result"), "structuredContent");
            ASSERT_TRUE(json_object_get(structured, "public"));
            ASSERT_TRUE(!json_object_get(structured, "secret"));
            ASSERT_TRUE(redactor.replaced == 1);
        } else {
            /* And it really is enforced there: a provider that breaks its own
             * declared schema is refused before hook 4 is ever consulted. */
            ASSERT_TRUE(json_is_true(json_object_get(
                json_object_get(response, "result"), "isError")));
            ASSERT_TRUE(redactor.replaced == 0);
        }
        json_decref(response);

        ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    }
    return 0;
}

/*
 * Hook 7 on all three catalogs: it renames, it invents, and it sees only what
 * hook 2 already let through.
 */
static int test_list_transforms_every_catalog(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    transform_t transform = {
        .id = 1,
        .list_rename_from = "fx.mutate", .list_rename_to = "edit_doc",
        .list_add = "meta.search"
    };
    probe_t probe = {.id = 2, .deny_identity = "fx.read"};
    maelys_mcp_middleware_t transformer = transform_middleware("catalog", &transform);
    maelys_mcp_middleware_t policy = probe_middleware("policy", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &policy, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &transformer, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *response = dispatch(channel, "tools/list", modern_params());
    json_t *tools = listed(response, "tools");
    /* One denied, one renamed, one invented. */
    ASSERT_TRUE(json_array_size(tools) == 2u);
    ASSERT_TRUE(lists_entry(tools, "edit_doc"));
    ASSERT_TRUE(lists_entry(tools, "meta.search"));
    ASSERT_TRUE(!lists_entry(tools, "fx.mutate"));
    ASSERT_TRUE(!lists_entry(tools, "fx.read"));
    json_decref(response);
    /* Hook 7 saw the post-policy catalog, not the registry. */
    ASSERT_TRUE(transform.lists == 1 && transform.last_list_size == 1u);
    ASSERT_TRUE(transform.last_catalog == MAELYS_MCP_CATALOG_TOOLS);

    response = dispatch(channel, "resources/list", modern_params());
    ASSERT_TRUE(json_array_size(listed(response, "resources")) == 2u);
    ASSERT_TRUE(lists_entry(listed(response, "resources"), "meta.search"));
    json_decref(response);
    ASSERT_TRUE(transform.last_catalog == MAELYS_MCP_CATALOG_RESOURCES);

    response = dispatch(channel, "resources/templates/list", modern_params());
    ASSERT_TRUE(json_array_size(listed(response, "resourceTemplates")) == 2u);
    json_decref(response);
    ASSERT_TRUE(transform.last_catalog == MAELYS_MCP_CATALOG_RESOURCE_TEMPLATES);
    ASSERT_TRUE(transform.lists == 3);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * A catalog that could not be transformed is not a shorter catalog: the
 * listing fails, for the same reason an undecidable hook 2 fails one.
 */
static int test_list_failure_fails_the_listing(void) {
    for (int index = 0; index <= 1; ++index) {
        fixture_state_t state = {0};
        maelys_mcp_runtime_t *runtime = NULL;
        ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
        transform_t transform = {
            .id = 1, .list_fails = index == 0, .list_returns_object = index == 1
        };
        maelys_mcp_middleware_t middleware =
            transform_middleware("broken", &transform);
        ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
            MAELYS_MCP_OK);
        maelys_mcp_channel_t *channel = NULL;
        ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

        json_t *response = dispatch(channel, "tools/list", modern_params());
        ASSERT_TRUE(error_code(response) == -32603);
        ASSERT_TRUE(strcmp(error_message(response),
            "Catalog transformation failed") == 0);
        json_decref(response);

        response = dispatch(channel, "resources/list", modern_params());
        ASSERT_TRUE(error_code(response) == -32603);
        json_decref(response);

        response = dispatch(channel, "resources/templates/list", modern_params());
        ASSERT_TRUE(error_code(response) == -32603);
        json_decref(response);

        ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
        ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    }
    return 0;
}

/* ---- hook 6: the response sink ---- */

#define FRAME_CAPACITY 16u

typedef struct sink_probe {
    int id;
    int wraps;
    int releases;
    int swallow_complete;
    int fail_wrap;
    int passive;
    /* The order the wrapper was asked to do things in, which is the ordering
     * guarantee this hook exists to keep. */
    char order[FRAME_CAPACITY][16];
    size_t order_count;
    void *last_channel_context;
    int saw_request;
} sink_probe_t;

typedef struct sink_state {
    sink_probe_t *probe;
} sink_state_t;

static void record_order(sink_probe_t *probe, const char *what) {
    if (probe->order_count >= FRAME_CAPACITY) return;
    record_identity(probe->order[probe->order_count],
        sizeof(probe->order[0]), what);
    probe->order_count++;
}

/* Every frame carries the ids of the wrappers that touched it, in order. */
static void tag_frame(json_t *message, int id) {
    json_t *seen = json_object_get(message, "_seen");
    if (!seen) {
        seen = json_array();
        if (!seen) return;
        if (json_object_set_new(message, "_seen", seen) != 0) return;
    }
    (void)json_array_append_new(seen, json_integer(id));
}

static maelys_mcp_result_t sink_emit(
    void *context,
    const maelys_mcp_response_sink_t *inner,
    json_t *message) {
    sink_state_t *state = context;
    record_order(state->probe, "emit");
    tag_frame(message, state->probe->id);
    return maelys_mcp_sink_emit(inner, message);
}

static maelys_mcp_result_t sink_complete(
    void *context,
    const maelys_mcp_response_sink_t *inner,
    json_t *response) {
    sink_state_t *state = context;
    record_order(state->probe, "complete");
    tag_frame(response, state->probe->id);
    if (state->probe->swallow_complete) {
        /* Reports success, forwards nothing, and releases what it was given -
         * the exact abuse that would otherwise wedge this request id. */
        json_decref(response);
        return MAELYS_MCP_OK;
    }
    return maelys_mcp_sink_complete(inner, response);
}

static void sink_release(void *context) {
    sink_state_t *state = context;
    state->probe->releases++;
    free(state);
}

static maelys_mcp_result_t sink_wrap(
    void *context,
    const maelys_mcp_wrap_sink_context_t *request,
    maelys_mcp_sink_wrapper_t *out_wrapper) {
    sink_probe_t *probe = context;
    probe->wraps++;
    probe->last_channel_context = maelys_mcp_channel_context(request->channel);
    probe->saw_request = json_is_object(request->request);
    if (probe->fail_wrap) return MAELYS_MCP_ERR_STATE;
    sink_state_t *state = calloc(1u, sizeof(*state));
    if (!state) return MAELYS_MCP_ERR_MEMORY;
    state->probe = probe;
    out_wrapper->context = state;
    out_wrapper->release = sink_release;
    /* A passive wrapper sets no function at all: everything passes through. */
    if (!probe->passive) {
        out_wrapper->emit = sink_emit;
        out_wrapper->complete = sink_complete;
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_middleware_t sink_middleware(
    const char *name,
    sink_probe_t *probe) {
    maelys_mcp_middleware_t middleware = {
        .name = name, .context = probe, .wrap_sink = sink_wrap
    };
    return middleware;
}

static json_t *token_call_params(const char *name, const char *token) {
    json_t *params = call_params(name, NULL);
    if (!params) return NULL;
    (void)json_object_set_new(json_object_get(params, "_meta"),
        "progressToken", json_string(token));
    return params;
}

/*
 * Handles one request and collects every frame it produced, the response
 * last. Unlike dispatch() above this does not stop at the first message,
 * because for hook 6 the frames before the response are the subject.
 */
static json_t *dispatch_frames(
    maelys_mcp_channel_t *channel,
    const char *method,
    json_t *params) {
    json_t *request = json_pack("{s:s,s:i,s:s,s:o}",
        "jsonrpc", "2.0", "id", 1, "method", method, "params", params);
    if (!request) return NULL;
    maelys_mcp_result_t status = maelys_mcp_channel_handle(channel, request);
    json_decref(request);
    if (status != MAELYS_MCP_OK) return NULL;
    json_t *frames = json_array();
    if (!frames) return NULL;
    for (size_t guard = 0; guard < FRAME_CAPACITY; ++guard) {
        json_t *message = NULL;
        if (maelys_mcp_channel_next(channel, 1000u, &message) != MAELYS_MCP_OK) break;
        int is_response = json_object_get(message, "result") != NULL ||
            json_object_get(message, "error") != NULL;
        if (json_array_append_new(frames, message) != 0) break;
        if (is_response) break;
    }
    return frames;
}

static int frame_is_progress(json_t *frame) {
    const char *method = json_string_value(json_object_get(frame, "method"));
    return method && strcmp(method, "notifications/progress") == 0;
}

/*
 * The ordering guarantee, and the reason hook 6 is the risky half of this
 * phase. Progress frames must reach the client strictly ahead of the response
 * they belong to - over SSE the response terminates the stream, so anything
 * behind it is not merely late, it is lost. Wrapping the sink must not change
 * that, and it does not, because the runtime forwards each frame as it is
 * produced and only completes once dispatch has returned.
 */
static int test_wrap_sink_keeps_progress_ahead_of_the_response(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    sink_probe_t probe = {.id = 1};
    maelys_mcp_middleware_t middleware = sink_middleware("stream", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    int principal = 7;
    maelys_mcp_channel_config_t config = {.context = &principal};
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, &config, &channel) == MAELYS_MCP_OK);

    json_t *frames = dispatch_frames(channel, "tools/call",
        token_call_params("fx.mutate", "tok-1"));
    ASSERT_TRUE(json_array_size(frames) == 4u);
    ASSERT_TRUE(state.progress_reports == 3);

    /* Every frame but the last is a progress notification, and the last is
     * the response: no progress arrived behind it. */
    for (size_t index = 0; index < 3u; ++index) {
        json_t *frame = json_array_get(frames, index);
        ASSERT_TRUE(frame_is_progress(frame));
        /* And it really passed through the wrapper rather than around it. */
        ASSERT_TRUE(json_integer_value(
            json_array_get(json_object_get(frame, "_seen"), 0)) == 1);
    }
    json_t *response = json_array_get(frames, 3u);
    ASSERT_TRUE(!frame_is_progress(response));
    ASSERT_TRUE(json_object_get(response, "result"));
    ASSERT_TRUE(json_integer_value(
        json_array_get(json_object_get(response, "_seen"), 0)) == 1);
    json_decref(frames);

    /* The same ordering seen from inside the wrapper: three emits, then the
     * completion, never the other way round. */
    ASSERT_TRUE(probe.order_count == 4u);
    ASSERT_TRUE(strcmp(probe.order[0], "emit") == 0);
    ASSERT_TRUE(strcmp(probe.order[1], "emit") == 0);
    ASSERT_TRUE(strcmp(probe.order[2], "emit") == 0);
    ASSERT_TRUE(strcmp(probe.order[3], "complete") == 0);

    /* Wrapped once per request, released once per request. */
    ASSERT_TRUE(probe.wraps == 1 && probe.releases == 1);
    ASSERT_TRUE(probe.last_channel_context == &principal);
    ASSERT_TRUE(probe.saw_request);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * Wrapping order is the reverse of hook 4's: the first-registered middleware
 * is the closest to the client, so it is the last to touch a frame before the
 * transport does.
 */
static int test_wrap_sink_order_is_reversed(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    sink_probe_t first = {.id = 1};
    sink_probe_t second = {.id = 2};
    maelys_mcp_middleware_t first_middleware = sink_middleware("first", &first);
    maelys_mcp_middleware_t second_middleware = sink_middleware("second", &second);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &first_middleware, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &second_middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *frames = dispatch_frames(channel, "tools/call",
        call_params("fx.mutate", NULL));
    ASSERT_TRUE(json_array_size(frames) == 1u);
    json_t *seen = json_object_get(json_array_get(frames, 0), "_seen");
    ASSERT_TRUE(json_array_size(seen) == 2u);
    /* Registered second, therefore outermost, therefore first to see it. */
    ASSERT_TRUE(json_integer_value(json_array_get(seen, 0)) == 2);
    ASSERT_TRUE(json_integer_value(json_array_get(seen, 1)) == 1);
    json_decref(frames);
    ASSERT_TRUE(first.releases == 1 && second.releases == 1);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * A wrapper that accepts the response and never passes it on would leave the
 * client waiting on that request id forever. The runtime notices and answers
 * past the chain instead.
 */
static int test_wrap_sink_that_swallows_the_response(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    sink_probe_t probe = {.id = 1, .swallow_complete = 1};
    maelys_mcp_middleware_t middleware = sink_middleware("blackhole", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *frames = dispatch_frames(channel, "tools/call",
        call_params("fx.mutate", NULL));
    ASSERT_TRUE(json_array_size(frames) == 1u);
    json_t *response = json_array_get(frames, 0);
    ASSERT_TRUE(error_code(response) == -32603);
    ASSERT_TRUE(strcmp(error_message(response), "Response was not delivered") == 0);
    /* Answered past the chain, so it carries no wrapper's mark - and it
     * carries the id the client sent, or it answers nothing. */
    ASSERT_TRUE(!json_object_get(response, "_seen"));
    ASSERT_TRUE(json_integer_value(json_object_get(response, "id")) == 1);
    json_decref(frames);
    ASSERT_TRUE(state.calls == 1);
    ASSERT_TRUE(probe.releases == 1);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/*
 * A wrap_sink that fails answers the request rather than dropping it, does
 * not dispatch, and does not leak the wrappers already built beside it.
 */
static int test_wrap_sink_failure_answers_the_request(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    sink_probe_t good = {.id = 1};
    sink_probe_t broken = {.id = 2, .fail_wrap = 1};
    maelys_mcp_middleware_t good_middleware = sink_middleware("good", &good);
    maelys_mcp_middleware_t broken_middleware = sink_middleware("broken", &broken);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &good_middleware, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &broken_middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *frames = dispatch_frames(channel, "tools/call",
        call_params("fx.mutate", NULL));
    ASSERT_TRUE(json_array_size(frames) == 1u);
    ASSERT_TRUE(error_code(json_array_get(frames, 0)) == -32603);
    ASSERT_TRUE(strcmp(error_message(json_array_get(frames, 0)),
        "Response sink wrapping failed") == 0);
    json_decref(frames);
    /* Nothing was dispatched, and the wrapper built before the failure was
     * released rather than abandoned. */
    ASSERT_TRUE(state.calls == 0);
    ASSERT_TRUE(good.wraps == 1 && good.releases == 1);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/* A wrapper that overrides nothing forwards everything, untouched. */
static int test_wrap_sink_defaults_to_pass_through(void) {
    fixture_state_t state = {0};
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_runtime(&state, &runtime) == MAELYS_MCP_OK);
    sink_probe_t probe = {.id = 1, .passive = 1};
    maelys_mcp_middleware_t middleware = sink_middleware("passive", &probe);
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    maelys_mcp_channel_t *channel = NULL;
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &channel) == MAELYS_MCP_OK);

    json_t *frames = dispatch_frames(channel, "tools/call",
        token_call_params("fx.mutate", "tok-1"));
    ASSERT_TRUE(json_array_size(frames) == 4u);
    for (size_t index = 0; index < 3u; ++index) {
        ASSERT_TRUE(frame_is_progress(json_array_get(frames, index)));
        ASSERT_TRUE(!json_object_get(json_array_get(frames, index), "_seen"));
    }
    ASSERT_TRUE(json_object_get(json_array_get(frames, 3u), "result"));
    json_decref(frames);
    ASSERT_TRUE(probe.wraps == 1 && probe.releases == 1 && probe.order_count == 0u);

    /* A notification produces no response at all, and the wrapper is still
     * established and released around it. */
    json_t *request = json_pack("{s:s,s:s}", "jsonrpc", "2.0",
        "method", "notifications/initialized");
    ASSERT_TRUE(request != NULL);
    ASSERT_TRUE(maelys_mcp_channel_handle(channel, request) == MAELYS_MCP_OK);
    json_decref(request);
    ASSERT_TRUE(probe.wraps == 2 && probe.releases == 2);

    ASSERT_TRUE(maelys_mcp_channel_destroy(channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    return 0;
}

/* ---- hook 6 across channels, concurrently ---- */

typedef struct concurrent_sink {
    pthread_mutex_t mutex;
    int wraps;
    int releases;
    int frames;
} concurrent_sink_t;

typedef struct concurrent_state {
    concurrent_sink_t *shared;
} concurrent_state_t;

static maelys_mcp_result_t concurrent_emit(
    void *context,
    const maelys_mcp_response_sink_t *inner,
    json_t *message) {
    concurrent_state_t *state = context;
    pthread_mutex_lock(&state->shared->mutex);
    state->shared->frames++;
    pthread_mutex_unlock(&state->shared->mutex);
    return maelys_mcp_sink_emit(inner, message);
}

static maelys_mcp_result_t concurrent_complete(
    void *context,
    const maelys_mcp_response_sink_t *inner,
    json_t *response) {
    concurrent_state_t *state = context;
    pthread_mutex_lock(&state->shared->mutex);
    state->shared->frames++;
    pthread_mutex_unlock(&state->shared->mutex);
    return maelys_mcp_sink_complete(inner, response);
}

static void concurrent_release(void *context) {
    concurrent_state_t *state = context;
    pthread_mutex_lock(&state->shared->mutex);
    state->shared->releases++;
    pthread_mutex_unlock(&state->shared->mutex);
    free(state);
}

static maelys_mcp_result_t concurrent_wrap(
    void *context,
    const maelys_mcp_wrap_sink_context_t *request,
    maelys_mcp_sink_wrapper_t *out_wrapper) {
    (void)request;
    concurrent_sink_t *shared = context;
    concurrent_state_t *state = calloc(1u, sizeof(*state));
    if (!state) return MAELYS_MCP_ERR_MEMORY;
    state->shared = shared;
    pthread_mutex_lock(&shared->mutex);
    shared->wraps++;
    pthread_mutex_unlock(&shared->mutex);
    out_wrapper->context = state;
    out_wrapper->emit = concurrent_emit;
    out_wrapper->complete = concurrent_complete;
    out_wrapper->release = concurrent_release;
    return MAELYS_MCP_OK;
}

/*
 * Its own provider rather than the shared fixture: that one counts its calls
 * in a plain int, which is exactly right for the single-threaded tests above
 * and would be a data race here. This one holds no mutable state at all, so
 * the only concurrency under test is the runtime's and the middleware's.
 */
static maelys_mcp_result_t concurrent_call(
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)context;
    (void)out_error;
    for (int step = 0; step <= 2; ++step) {
        (void)maelys_mcp_provider_report_progress(request->progress,
            step * 50.0, 100.0, NULL);
    }
    out_result->content = json_pack("[{s:s,s:s}]", "type", "text", "text", "done");
    return out_result->content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

static maelys_mcp_result_t build_concurrent_runtime(
    maelys_mcp_runtime_t **out_runtime) {
    maelys_mcp_runtime_config_t config = {
        .server_name = "concurrent", .server_version = "1"
    };
    maelys_mcp_runtime_t *runtime = NULL;
    maelys_mcp_result_t status = maelys_mcp_runtime_create(&config, &runtime);
    if (status != MAELYS_MCP_OK) return status;
    if (maelys_mcp_runtime_enable_module(runtime, MAELYS_MCP_MODULE_TOOLS) !=
        MAELYS_MCP_OK) {
        return abandon_runtime(runtime, MAELYS_MCP_ERR_STATE);
    }
    json_t *schema = json_pack("{s:s,s:b}", "type", "object",
        "additionalProperties", 0);
    if (!schema) return abandon_runtime(runtime, MAELYS_MCP_ERR_MEMORY);
    maelys_mcp_tool_t tool = {
        .name = "cc.work",
        .title = "Work",
        .description = "Reports progress, then completes.",
        .input_schema = schema,
        .effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_config_t provider_config = {
        .name = "concurrent-provider", .version = "1",
        .tools = &tool, .tool_count = 1, .call = concurrent_call
    };
    maelys_mcp_provider_t *provider = NULL;
    status = maelys_mcp_provider_create(&provider_config, &provider);
    json_decref(schema);
    if (status != MAELYS_MCP_OK) return abandon_runtime(runtime, status);
    status = maelys_mcp_runtime_add_provider(runtime, provider, NULL);
    if (status != MAELYS_MCP_OK) {
        maelys_mcp_provider_destroy(provider);
        return abandon_runtime(runtime, status);
    }
    *out_runtime = runtime;
    return MAELYS_MCP_OK;
}

#define CONCURRENT_ROUNDS 24

typedef struct caller {
    maelys_mcp_channel_t *channel;
    int failures;
} caller_t;

static void *call_repeatedly(void *argument) {
    caller_t *caller = argument;
    for (int round = 0; round < CONCURRENT_ROUNDS; ++round) {
        json_t *frames = dispatch_frames(caller->channel, "tools/call",
            token_call_params("cc.work", "tok"));
        if (json_array_size(frames) != 4u ||
            !json_object_get(json_array_get(frames, 3u), "result")) {
            caller->failures++;
        }
        if (frames) json_decref(frames);
    }
    return NULL;
}

/*
 * The contract says a wrapper for one channel may run concurrently with a
 * wrapper for another, and that a middleware sharing state across channels
 * owns its own locking. This runs that shape so the sanitizers can look at
 * it: two channels, two threads, one middleware, no runtime lock between
 * them, and per-request wrapper state that must never be shared.
 */
static int test_wrap_sink_runs_concurrently_across_channels(void) {
    maelys_mcp_runtime_t *runtime = NULL;
    ASSERT_TRUE(build_concurrent_runtime(&runtime) == MAELYS_MCP_OK);
    concurrent_sink_t shared = {0};
    ASSERT_TRUE(pthread_mutex_init(&shared.mutex, NULL) == 0);
    maelys_mcp_middleware_t middleware = {
        .name = "concurrent", .context = &shared, .wrap_sink = concurrent_wrap
    };
    ASSERT_TRUE(maelys_mcp_runtime_add_middleware(runtime, &middleware, NULL) ==
        MAELYS_MCP_OK);
    caller_t first = {0};
    caller_t second = {0};
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &first.channel) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_channel_create(runtime, NULL, &second.channel) ==
        MAELYS_MCP_OK);

    pthread_t thread;
    ASSERT_TRUE(pthread_create(&thread, NULL, call_repeatedly, &second) == 0);
    call_repeatedly(&first);
    ASSERT_TRUE(pthread_join(thread, NULL) == 0);
    ASSERT_TRUE(first.failures == 0 && second.failures == 0);

    /* Wrapped once per request and released once per request, with no
     * wrapper state surviving a request or crossing a channel. */
    ASSERT_TRUE(shared.wraps == 2 * CONCURRENT_ROUNDS);
    ASSERT_TRUE(shared.releases == shared.wraps);
    ASSERT_TRUE(shared.frames == 4 * 2 * CONCURRENT_ROUNDS);

    ASSERT_TRUE(maelys_mcp_channel_destroy(first.channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_channel_destroy(second.channel) == MAELYS_MCP_OK);
    ASSERT_TRUE(maelys_mcp_runtime_destroy(runtime) == MAELYS_MCP_OK);
    ASSERT_TRUE(pthread_mutex_destroy(&shared.mutex) == 0);
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
        {"destroy runs in reverse order", test_destroy_runs_in_reverse_order},
        {"resolve renames and injects", test_resolve_renames_and_injects},
        {"a type change must convert in resolve",
            test_type_change_must_convert_in_resolve},
        {"a rename cannot route around a deny",
            test_rename_cannot_route_around_a_deny},
        {"resolve failures", test_resolve_failures},
        {"call substitutes for the provider",
            test_call_substitutes_for_the_provider},
        {"call substitution failures", test_call_substitution_failures},
        {"result rewrites in order", test_result_rewrites_in_order},
        {"result transformation failures", test_result_transformation_failures},
        {"result redaction versus output schema",
            test_result_redaction_versus_output_schema},
        {"list transforms every catalog", test_list_transforms_every_catalog},
        {"a list failure fails the listing",
            test_list_failure_fails_the_listing},
        {"wrap_sink keeps progress ahead of the response",
            test_wrap_sink_keeps_progress_ahead_of_the_response},
        {"wrap_sink order is reversed", test_wrap_sink_order_is_reversed},
        {"wrap_sink that swallows the response",
            test_wrap_sink_that_swallows_the_response},
        {"wrap_sink failure answers the request",
            test_wrap_sink_failure_answers_the_request},
        {"wrap_sink defaults to pass through",
            test_wrap_sink_defaults_to_pass_through},
        {"wrap_sink runs concurrently across channels",
            test_wrap_sink_runs_concurrently_across_channels}
    };
    int failures = maelys_run_tests(cases, sizeof(cases) / sizeof(cases[0]));
    if (failures == 0) puts("test_middleware: OK");
    return failures;
}
