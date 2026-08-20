/*
 * A minimal maelys-provider/5 process provider built on the public C SDK
 * (src/provider/provider_sdk.c), rather than the hand-rolled JSON
 * tests/helpers/adversarial_provider.c's own nested fixtures use. Its only
 * job is to prove maelys_mcp_provider_sdk_request_client interoperates with
 * the real host - process_provider.c's relay, src/modules/mrtr.c's
 * capability check - end to end over a real provider process, not just
 * against the scripted fake host tests/test_provider_sdk.c drives.
 *
 * One tool, "nested.ask", mirrors adversarial_provider.c's fixture of the
 * same name closely enough that tests/test_nested_requests.c's existing
 * happy-path and undeclared-capability harnesses work against this provider
 * unchanged, just pointed at a different executable.
 */
#include "maelys/mcp/provider_sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static maelys_mcp_result_t call_tool(
    maelys_mcp_provider_sdk_t *sdk,
    void *context,
    const maelys_mcp_provider_request_t *request,
    maelys_mcp_provider_result_t *out_result,
    char **out_error) {
    (void)context;
    if (strcmp(request->tool_name, "nested.ask") != 0) {
        if (out_error) *out_error = strdup("unknown tool");
        return MAELYS_MCP_ERR_NOT_FOUND;
    }
    json_t *params = json_pack("{s:s}", "message", "Proceed?");
    json_t *client_result = NULL;
    char *nested_error = NULL;
    maelys_mcp_result_t status = maelys_mcp_provider_sdk_request_client(
        sdk, "elicitation/create", params, &client_result, &nested_error);
    if (params) json_decref(params);
    char text[256];
    if (status == MAELYS_MCP_OK) {
        json_t *answer = json_is_object(client_result) ?
            json_object_get(client_result, "answer") : NULL;
        (void)snprintf(text, sizeof(text), "%s",
            json_is_string(answer) ? json_string_value(answer) : "no-answer");
    } else if (status == MAELYS_MCP_ERR_DENIED) {
        (void)snprintf(text, sizeof(text), "error:denied");
    } else {
        (void)snprintf(text, sizeof(text), "error:%s",
            nested_error ? nested_error : "failed");
    }
    if (client_result) json_decref(client_result);
    free(nested_error);
    out_result->type = MAELYS_MCP_PROVIDER_RESULT_COMPLETE;
    out_result->content = json_pack("[{s:s,s:s}]", "type", "text", "text", text);
    return out_result->content ? MAELYS_MCP_OK : MAELYS_MCP_ERR_MEMORY;
}

int main(void) {
    json_t *schema = json_pack("{s:s}", "type", "object");
    if (!schema) return 1;
    maelys_mcp_tool_t tool = {
        .name = "nested.ask",
        .title = "Ask the client (SDK)",
        .description = "Opens a nested client request via the C provider SDK.",
        .input_schema = schema,
        .effect = MAELYS_MCP_EFFECT_READ
    };
    maelys_mcp_provider_sdk_config_t config = {
        .name = "sdk-nested",
        .version = "1",
        .tools = &tool,
        .tool_count = 1u,
        .call = call_tool
    };
    maelys_mcp_result_t status = maelys_mcp_provider_sdk_serve(&config, NULL);
    json_decref(schema);
    return status == MAELYS_MCP_OK ? 0 : 1;
}
