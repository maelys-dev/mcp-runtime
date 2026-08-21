/*
 * The adapter seam, driven by a recording writer with no socket involved.
 *
 * This is one half of H1's merge criterion and the reason the seam is a
 * function rather than a descriptor: an orchestrator behind its own listener,
 * and a fuzz target with no listener at all, reach the adapter exactly the way
 * this file does.
 *
 * H1's half asserts the CONTRACT rather than the placeholder answer's content:
 * exactly one of begin_json/begin_stream, end_stream exactly once per
 * begin_stream, the exchange published before any work and retired before
 * _handle returns, and a cancel after the return that is a no-op.
 *
 * H2's half is the REJECTION MATRIX below - one named case per MCP-header rule
 * in docs/http-transport-design.md, each carrying the rule it exists for in its
 * `rule` column so a reader can check the matrix against the design rather than
 * against the implementation. Every case is a negative one except where a
 * positive is what proves a rule does NOT fire.
 */
#include "maelys/mcp/http.h"
#include "tests/test_support.h"

#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct recording_writer {
    int begin_json_calls;
    int begin_stream_calls;
    int event_calls;
    int keepalive_calls;
    int end_stream_calls;
    int status_only_calls;
    int status;
    maelys_mcp_http_stream_end_t disposition;
    char body[1024];
    size_t body_length;
} recording_writer_t;

static maelys_mcp_result_t record_begin_json(
    void *context, int status, const char *body, size_t length) {
    recording_writer_t *writer = context;
    ++writer->begin_json_calls;
    writer->status = status;
    if (length < sizeof(writer->body)) {
        memcpy(writer->body, body, length);
        writer->body[length] = '\0';
        writer->body_length = length;
    }
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t record_begin_stream(void *context) {
    recording_writer_t *writer = context;
    ++writer->begin_stream_calls;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t record_event(void *context, const char *json, size_t length) {
    recording_writer_t *writer = context;
    (void)json;
    (void)length;
    ++writer->event_calls;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t record_keepalive(void *context) {
    recording_writer_t *writer = context;
    ++writer->keepalive_calls;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t record_end_stream(
    void *context, maelys_mcp_http_stream_end_t disposition) {
    recording_writer_t *writer = context;
    ++writer->end_stream_calls;
    writer->disposition = disposition;
    return MAELYS_MCP_OK;
}

static maelys_mcp_result_t record_status_only(
    void *context, int status, const char *const *extra, size_t extra_count) {
    recording_writer_t *writer = context;
    (void)extra;
    (void)extra_count;
    ++writer->status_only_calls;
    writer->status = status;
    return MAELYS_MCP_OK;
}

static maelys_mcp_http_response_writer_t make_writer(recording_writer_t *state) {
    maelys_mcp_http_response_writer_t writer = {
        .context = state,
        .begin_json = record_begin_json,
        .begin_stream = record_begin_stream,
        .write_event = record_event,
        .write_keepalive = record_keepalive,
        .end_stream = record_end_stream,
        .status_only = record_status_only
    };
    return writer;
}

/*
 * The embedder's own header representation, which is the point of the lookup
 * being a callback: the adapter never learns what shape it has.
 *
 * `asked` records every name the adapter asked for, which is what lets a test
 * assert a negative - that Authorization is never among them.
 */
#define MAX_ASKED 16

typedef struct slice_headers {
    /* Flat name/value pairs, NULL-terminated. */
    const char *const *pairs;
    int lookups;
    const char *asked[MAX_ASKED];
    int asked_count;
} slice_headers_t;

static int slice_lookup(
    void *context, const char *name, const char **out_value, size_t *out_length) {
    slice_headers_t *headers = context;
    ++headers->lookups;
    if (headers->asked_count < MAX_ASKED) headers->asked[headers->asked_count++] = name;
    const char *found = NULL;
    for (size_t index = 0u; headers->pairs && headers->pairs[index]; index += 2u) {
        if (strcasecmp(headers->pairs[index], name) != 0) continue;
        /* Present more than once is reported absent, per the seam's contract. */
        if (found) return 0;
        found = headers->pairs[index + 1u];
    }
    if (!found) return 0;
    if (out_value) *out_value = found;
    if (out_length) *out_length = strlen(found);
    return 1;
}

static int asked_for(const slice_headers_t *headers, const char *name) {
    for (int index = 0; index < headers->asked_count; ++index) {
        if (strcasecmp(headers->asked[index], name) == 0) return 1;
    }
    return 0;
}

static maelys_mcp_http_request_t make_request(
    slice_headers_t *headers, const char *body) {
    maelys_mcp_http_request_t request = {
        .method = "POST",
        .path = "/mcp",
        .header_lookup = slice_lookup,
        .header_context = headers,
        .body = body,
        .body_length = strlen(body),
        .principal = NULL,
        .cancel_fd = -1
    };
    return request;
}

/* ------------------------------------------------------ the H1 contract half */

/* A body that passes every H2 rule, so the H1 contract cases still reach the
 * placeholder they were written to observe. */
#define VALID_BODY \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":" \
    "{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}"

static const char *const VALID_HEADERS[] = {
    "MCP-Protocol-Version", "2026-07-28",
    "Mcp-Method", "tools/list",
    NULL
};

static int json_mode_answers_once(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {.pairs = VALID_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, VALID_BODY);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_OK);
    /* Exactly one of begin_json / begin_stream, at most once. */
    ASSERT_TRUE(state.begin_json_calls == 1);
    ASSERT_TRUE(state.begin_stream_calls == 0);
    ASSERT_TRUE(state.end_stream_calls == 0);
    ASSERT_TRUE(state.status_only_calls == 0);
    /* The placeholder survives H2 unchanged: 503 with -32600 and no id. H2
     * validates and still does not dispatch, so an endpoint that does not
     * dispatch must not look like one that did. */
    ASSERT_TRUE(state.status == 503);
    ASSERT_TRUE(strstr(state.body, "-32600") != NULL);
    ASSERT_TRUE(strstr(state.body, "\"id\"") == NULL);
    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}

static int stream_mode_ends_exactly_once(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_STREAM, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {.pairs = VALID_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, VALID_BODY);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(state.begin_stream_calls == 1);
    ASSERT_TRUE(state.begin_json_calls == 0);
    ASSERT_TRUE(state.end_stream_calls == 1);
    ASSERT_TRUE(state.disposition == MAELYS_MCP_HTTP_STREAM_COMPLETE);
    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}

static int cancellation_selects_the_aborted_disposition(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_STREAM, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {.pairs = VALID_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, VALID_BODY);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    maelys_mcp_http_exchange_t *exchange = NULL;
    /*
     * The published handle is the point: a canceller has something to name, and
     * it names the exchange rather than the request, so the request struct's
     * address never becomes a concurrent identity.
     */
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange) ==
        MAELYS_MCP_OK);
    /* Retired before _handle returned. */
    ASSERT_TRUE(exchange == NULL);
    /* Cancelling a retired handle is a no-op rather than a use-after-free. */
    maelys_mcp_http_exchange_cancel(exchange);
    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}

typedef struct cancelling_writer {
    recording_writer_t recording;
    maelys_mcp_http_exchange_t **exchange;
} cancelling_writer_t;

static maelys_mcp_result_t cancel_then_begin_stream(void *context) {
    cancelling_writer_t *writer = context;
    maelys_mcp_http_exchange_cancel(*writer->exchange);
    return record_begin_stream(&writer->recording);
}

static int a_live_cancel_aborts_the_stream(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_STREAM, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {.pairs = VALID_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, VALID_BODY);
    maelys_mcp_http_exchange_t *exchange = NULL;
    cancelling_writer_t state = {.recording = {0}, .exchange = &exchange};
    maelys_mcp_http_response_writer_t writer = make_writer(&state.recording);
    writer.context = &state;
    writer.begin_stream = cancel_then_begin_stream;
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, &exchange) ==
        MAELYS_MCP_OK);
    ASSERT_TRUE(state.recording.begin_stream_calls == 1);
    ASSERT_TRUE(state.recording.end_stream_calls == 1);
    ASSERT_TRUE(state.recording.disposition == MAELYS_MCP_HTTP_STREAM_ABORTED);
    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}

/*
 * H1 pinned this as "lookups == 0", with a comment saying the claim worth
 * keeping was the one that had to survive H2. H2 is here and the adapter now
 * reads headers, so the counter is replaced by the claim itself: whatever it
 * asks for, it never asks for Authorization. The credential is the server
 * layer's and the adapter has no business seeing it.
 */
static int the_adapter_never_asks_for_authorization(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &adapter) == MAELYS_MCP_OK);
    static const char *const pairs[] = {
        "Authorization", "Bearer secret",
        "MCP-Protocol-Version", "2026-07-28",
        "Mcp-Method", "tools/call",
        "Mcp-Name", "search",
        NULL
    };
    slice_headers_t headers = {.pairs = pairs};
    maelys_mcp_http_request_t request = make_request(&headers,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"search\",\"_meta\":"
        "{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}");
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) ==
        MAELYS_MCP_OK);
    /* It validated - so it did ask for something. */
    ASSERT_TRUE(headers.lookups > 0);
    ASSERT_TRUE(asked_for(&headers, "MCP-Protocol-Version"));
    ASSERT_TRUE(asked_for(&headers, "Mcp-Method"));
    ASSERT_TRUE(asked_for(&headers, "Mcp-Name"));
    /* And never for this one. */
    ASSERT_TRUE(!asked_for(&headers, "Authorization"));
    maelys_mcp_http_adapter_destroy(adapter);
    return 0;
}

static int arguments_are_validated(void) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        (maelys_mcp_http_placeholder_t)7, &adapter) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, NULL) == MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_create(
        MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &adapter) == MAELYS_MCP_OK);
    slice_headers_t headers = {.pairs = VALID_HEADERS};
    maelys_mcp_http_request_t request = make_request(&headers, VALID_BODY);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(NULL, &request, &writer, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, NULL, &writer, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, NULL, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    /* An incomplete writer is an argument error rather than a crash: the seam
     * is public, so a partially-filled struct is a thing an embedder will do. */
    maelys_mcp_http_response_writer_t partial = writer;
    partial.end_stream = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &request, &partial, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    /* H2 cannot validate without a lookup, so a request with none is an
     * argument error rather than an exchange that skips the rules. */
    maelys_mcp_http_request_t no_lookup = request;
    no_lookup.header_lookup = NULL;
    ASSERT_TRUE(maelys_mcp_http_adapter_handle(adapter, &no_lookup, &writer, NULL) ==
        MAELYS_MCP_ERR_ARGUMENT);
    maelys_mcp_http_adapter_destroy(adapter);
    maelys_mcp_http_adapter_destroy(NULL);
    return 0;
}

/* ------------------------------------------------ the H2 rejection matrix */

/* `expect_status` 503 means the case passes validation and reaches the
 * placeholder; 400 means it is refused, and `expect_code` says by which rule. */
typedef struct matrix_case {
    const char *rule;      /* the design rule this case exists for */
    const char *name;
    const char *const *headers;
    const char *body;
    int expect_status;
    int expect_code;
    /* 1 when the refusal must echo the request's id (2), 0 when it must be
     * null. Ignored on a 503. */
    int expect_id;
    /* A substring of the refusal message, or NULL. Two rows of the status
     * table share 400 and -32600 - a JSON-RPC response body and everything
     * else - so the code alone cannot tell them apart and the message is what
     * keeps them separate rows rather than one. */
    const char *expect_message;
} matrix_case_t;

#define META "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}"

static const char *const H_VERSION_ONLY[] = {
    "MCP-Protocol-Version", "2026-07-28", NULL};
static const char *const H_NONE[] = {NULL};
static const char *const H_VERSION_DUPLICATED[] = {
    "MCP-Protocol-Version", "2026-07-28",
    "MCP-Protocol-Version", "2026-07-28", NULL};
static const char *const H_VERSION_WRONG[] = {
    "MCP-Protocol-Version", "2025-11-25", NULL};
static const char *const H_VERSION_LEGACY_AGREEING[] = {
    "MCP-Protocol-Version", "2025-11-25", "Mcp-Method", "tools/list", NULL};
static const char *const H_VERSION_AND_METHOD[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/list", NULL};
/*
 * Header sets that isolate ONE rule: the version rule cannot be the reason a
 * request is refused if a second rule is also firing, and a matrix whose cases
 * all trip two rules at once cannot tell which one is load-bearing. Each of
 * these carries a matching Mcp-Method so the version rule is the only thing
 * left that can refuse.
 */
static const char *const H_METHOD_ONLY[] = {"Mcp-Method", "tools/list", NULL};
static const char *const H_VERSION_WRONG_WITH_METHOD[] = {
    "MCP-Protocol-Version", "2025-11-25", "Mcp-Method", "tools/list", NULL};
static const char *const H_METHOD_WRONG[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/call", NULL};
static const char *const H_METHOD_CASE[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "Tools/List", NULL};
static const char *const H_METHOD_DUPLICATED[] = {
    "MCP-Protocol-Version", "2026-07-28",
    "Mcp-Method", "tools/list", "Mcp-Method", "tools/list", NULL};

#define CALL_HEADERS(name_value) \
    {"MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/call", \
     "Mcp-Name", (name_value), NULL}

static const char *const H_CALL_NO_NAME[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/call", NULL};
static const char *const H_CALL_NAME_OK[] = CALL_HEADERS("search");
static const char *const H_CALL_NAME_WRONG[] = CALL_HEADERS("other");
static const char *const H_CALL_NAME_SENTINEL_OK[] = CALL_HEADERS("=?base64?c2VhcmNo?=");
static const char *const H_CALL_NAME_SENTINEL_WRONG[] = CALL_HEADERS("=?base64?b3RoZXI=?=");
static const char *const H_CALL_NAME_SENTINEL_UTF8[] = CALL_HEADERS("=?base64?c8OpY3VyaXTDqQ==?=");
static const char *const H_CALL_NAME_BAD_ALPHABET[] = CALL_HEADERS("=?base64?c2Vh*mNo?=");
static const char *const H_CALL_NAME_URLSAFE[] = CALL_HEADERS("=?base64?a-b_?=");
static const char *const H_CALL_NAME_BAD_LENGTH[] = CALL_HEADERS("=?base64?c2VhcmNoZ?=");
static const char *const H_CALL_NAME_BAD_PADBITS[] = CALL_HEADERS("=?base64?YR==?=");
static const char *const H_CALL_NAME_BAD_PADBITS_ONE[] = CALL_HEADERS("=?base64?YWJ=?=");
static const char *const H_CALL_NAME_INNER_PAD[] = CALL_HEADERS("=?base64?YQ==YQ==?=");
static const char *const H_CALL_NAME_INVALID_UTF8[] = CALL_HEADERS("=?base64?//4=?=");
static const char *const H_CALL_NAME_EMBEDDED_NUL[] = CALL_HEADERS("=?base64?YQBi?=");
static const char *const H_CALL_NAME_SHORT_PATTERN[] = CALL_HEADERS("=?base64?=");
static const char *const H_CALL_NAME_COLLISION_RAW[] = CALL_HEADERS("=?base64?QQ==?=");
static const char *const H_CALL_NAME_COLLISION_ENCODED[] =
    CALL_HEADERS("=?base64?PT9iYXNlNjQ/UVE9PT89?=");
/*
 * A payload that uses every boundary of the Base64 alphabet - 'A', 'Z', 'a',
 * 'z', '0', '9', '+' and '/' all appear in it - and decodes to clean UTF-8.
 *
 * It exists because mutating base64_value's range tests one boundary at a time
 * (`c >= 'A'` to `c > 'A'`, `c <= 'Z'` to `c < 'Z'`, and so on) SURVIVED a
 * matrix whose every other payload happened to avoid those six characters. A
 * decoder that silently stops recognising 'A' is a decoder that mis-decodes a
 * name, and nothing here noticed.
 */
#define ALPHABET_NAME "<5}RXZP~E?B@ss?pK~kzCKu2g6KL<b<SQ_woiouF8<f"
#define ALPHABET_B64 "PDV9UlhaUH5FP0JAc3M/cEt+a3pDS3UyZzZLTDxiPFNRX3dvaW91Rjg8Zg=="
static const char *const H_CALL_NAME_ALPHABET[] =
    CALL_HEADERS("=?base64?" ALPHABET_B64 "?=");

/* Prefix without suffix: not a sentinel, so it is compared literally. */
static const char *const H_CALL_NAME_PREFIX_ONLY[] = CALL_HEADERS("=?base64?c2VhcmNo");
/* The URL-safe spelling of "fn5+", which is base64 for "~~~". */
static const char *const H_CALL_NAME_URLSAFE_DECODABLE[] = CALL_HEADERS("=?base64?fn5-?=");

static const char *const H_READ_NO_NAME[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "resources/read", NULL};
static const char *const H_READ_NAME_RAW[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "resources/read",
    "Mcp-Name", "file:///tmp/a%20b", NULL};
static const char *const H_READ_NAME_CANONICAL[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "resources/read",
    "Mcp-Name", "file:///tmp/a b", NULL};
static const char *const H_LIST_WITH_NAME[] = {
    "MCP-Protocol-Version", "2026-07-28", "Mcp-Method", "tools/list",
    "Mcp-Name", "irrelevant", NULL};

#define REQ(method, params) \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"" method "\",\"params\":{" params "}}"
#define NOTE(method, params) \
    "{\"jsonrpc\":\"2.0\",\"method\":\"" method "\",\"params\":{" params "}}"

static const matrix_case_t MATRIX[] = {
    /* --- body parse and classification (the -32700 / -32600 adapter rows) --- */
    {"body not valid JSON -> 400 -32700",
     "truncated json", H_VERSION_ONLY, "{\"jsonrpc\":", 400, -32700, 0, NULL},
    {"body valid JSON but not an object -> 400 -32700",
     "array body", H_VERSION_ONLY, "[1,2,3]", 400, -32700, 0, NULL},
    {"body valid JSON but not an object -> 400 -32700",
     "bare string body", H_VERSION_ONLY, "\"hello\"", 400, -32700, 0, NULL},
    {"body valid JSON but not an object -> 400 -32700",
     "empty body", H_VERSION_ONLY, "", 400, -32700, 0, NULL},
    {"body is a JSON-RPC response (has id, no method) -> 400 -32600 no id",
     "response carrying a result", H_VERSION_ONLY,
     "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}", 400, -32600, 0,
     "JSON-RPC response"},
    {"body is a JSON-RPC response (has id, no method) -> 400 -32600 no id",
     "response carrying an error", H_VERSION_ONLY,
     "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-1,\"message\":\"x\"}}",
     400, -32600, 0, "JSON-RPC response"},
    {"anything else -> 400",
     "neither method nor id", H_VERSION_ONLY, "{\"jsonrpc\":\"2.0\"}", 400, -32600, 0,
     "not a JSON-RPC request or notification"},
    {"anything else -> 400",
     "method is not a string", H_VERSION_ONLY,
     "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":7}", 400, -32600, 0, NULL},
    {"a notification is a request WITHOUT an id, so an explicit null id is neither",
     "method with a null id", H_VERSION_ONLY,
     "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"tools/list\"}", 400, -32600, 0, NULL},

    /* --- MCP-Protocol-Version --- */
    {"MCP-Protocol-Version required, exactly once -> 400 -32020",
     "version header absent", H_NONE, REQ("tools/list", META), 400, -32020, 1, NULL},
    {"a repeated protocol header is reported absent and refused, never merged",
     "version header duplicated", H_VERSION_DUPLICATED,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"the header must equal the body's _meta version -> 400 -32020",
     "body carries no _meta version", H_VERSION_ONLY,
     REQ("tools/list", "\"a\":1"), 400, -32020, 1, NULL},
    {"the header must equal the body's _meta version -> 400 -32020",
     "header and _meta disagree", H_VERSION_WRONG,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"an agreeing pair naming an unserved version is NOT the adapter's refusal: "
     "-32022 is the runtime's row and stays there",
     "legacy version agreeing with the body", H_VERSION_LEGACY_AGREEING,
     "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{\"_meta\":"
     "{\"io.modelcontextprotocol/protocolVersion\":\"2025-11-25\"}}}", 503, 0, 0, NULL},

    /* --- Mcp-Method --- */
    {"Mcp-Method required on requests -> 400 -32020",
     "method header absent on a request", H_VERSION_ONLY,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"a repeated protocol header is reported absent and refused",
     "method header duplicated", H_METHOD_DUPLICATED,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"Mcp-Method must equal the body's method byte for byte -> 400 -32020",
     "method header names another method", H_METHOD_WRONG,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"header VALUES are compared case-sensitively",
     "method header differs only in case", H_METHOD_CASE,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"Mcp-Method is required on requests, so a notification without one passes",
     "notification with no method header", H_VERSION_ONLY,
     NOTE("notifications/cancelled", META), 503, 0, 0, NULL},
    {"present-but-wrong is a mismatch whatever the body kind",
     "notification with a mismatched method header", H_METHOD_WRONG,
     NOTE("notifications/cancelled", META), 400, -32020, 0, NULL},

    /* --- Mcp-Name --- */
    {"Mcp-Name required on tools/call -> 400 -32020",
     "name header absent on tools/call", H_CALL_NO_NAME,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1, NULL},
    {"Mcp-Name required on resources/read -> 400 -32020",
     "name header absent on resources/read", H_READ_NO_NAME,
     REQ("resources/read", "\"uri\":\"file:///tmp/a\"," META), 400, -32020, 1, NULL},
    {"Mcp-Name must equal params.name -> 400 -32020",
     "name header names another tool", H_CALL_NAME_WRONG,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1, NULL},
    {"Mcp-Name must equal params.name",
     "literal name matches", H_CALL_NAME_OK,
     REQ("tools/call", "\"name\":\"search\"," META), 503, 0, 0, NULL},
    {"Mcp-Name is required only where the spec requires it; elsewhere there is "
     "no body field to compare against",
     "name header on tools/list is ignored", H_LIST_WITH_NAME,
     REQ("tools/list", META), 503, 0, 0, NULL},

    /* --- the Base64 sentinel --- */
    {"a value with the prefix AND the suffix is decoded before comparison",
     "sentinel decodes and matches", H_CALL_NAME_SENTINEL_OK,
     REQ("tools/call", "\"name\":\"search\"," META), 503, 0, 0, NULL},
    {"decoding happens before comparison, so a decoded mismatch is still a mismatch",
     "sentinel decodes and mismatches", H_CALL_NAME_SENTINEL_WRONG,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1, NULL},
    {"the decoded bytes must be valid UTF-8; a multibyte name round-trips",
     "sentinel carrying multibyte UTF-8", H_CALL_NAME_SENTINEL_UTF8,
     REQ("tools/call", "\"name\":\"s\\u00e9curit\\u00e9\"," META), 503, 0, 0, NULL},
    {"non-alphabet characters are a rejection",
     "sentinel with a non-alphabet byte", H_CALL_NAME_BAD_ALPHABET,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1, NULL},
    {"there is no URL-safe alphabet",
     "sentinel using - and _", H_CALL_NAME_URLSAFE,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1, NULL},
    {"wrong padding length is a rejection",
     "sentinel payload not a multiple of four", H_CALL_NAME_BAD_LENGTH,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1, NULL},
    {"non-zero bits in the padding are a rejection",
     "sentinel with non-zero bits under two pad bytes", H_CALL_NAME_BAD_PADBITS,
     REQ("tools/call", "\"name\":\"a\"," META), 400, -32020, 1, NULL},
    {"non-zero bits in the padding are a rejection",
     "sentinel with non-zero bits under one pad byte", H_CALL_NAME_BAD_PADBITS_ONE,
     REQ("tools/call", "\"name\":\"ab\"," META), 400, -32020, 1, NULL},
    {"padding appears only at the tail",
     "sentinel with padding in the middle", H_CALL_NAME_INNER_PAD,
     REQ("tools/call", "\"name\":\"a\"," META), 400, -32020, 1, NULL},
    /*
     * The next two, and the UTF-8 rules generally, are DEFENCE IN DEPTH and
     * cannot be killed by a behavioural test - a mutation that deletes any of
     * them survives this matrix, correctly. The body value is produced by
     * jansson, which refuses to parse a string that is not valid UTF-8 or that
     * carries \u0000, so a decoded value violating one of those rules can never
     * equal the body value and the comparison refuses it anyway. The design
     * says as much for the NUL case: accepting one "could only produce a
     * comparison that can never succeed". The rules stay because rejecting at
     * the decoder is clearer than rejecting at the comparison, and because the
     * http-headers fuzz target covers the memory safety the comparison does
     * not.
     */
    {"the decoded bytes must be valid UTF-8",
     "sentinel decoding to invalid UTF-8", H_CALL_NAME_INVALID_UTF8,
     REQ("tools/call", "\"name\":\"search\"," META), 400, -32020, 1, NULL},
    {"the decoded bytes must contain no NUL",
     "sentinel decoding to an embedded NUL", H_CALL_NAME_EMBEDDED_NUL,
     REQ("tools/call", "\"name\":\"a\"," META), 400, -32020, 1, NULL},
    {"prefix and suffix must not overlap: below 11 bytes the value is literal",
     "the 10-byte pattern is taken literally", H_CALL_NAME_SHORT_PATTERN,
     REQ("tools/call", "\"name\":\"=?base64?=\"," META), 503, 0, 0, NULL},
    {"a literal value matching the pattern must have been encoded by the client; "
     "the mismatch that results is a genuine HeaderMismatch",
     "raw sentinel-looking name is decoded, not taken literally",
     H_CALL_NAME_COLLISION_RAW,
     REQ("tools/call", "\"name\":\"=?base64?QQ==?=\"," META), 400, -32020, 1, NULL},
    {"the same collision, encoded as the client should have encoded it, passes",
     "encoded sentinel-looking name", H_CALL_NAME_COLLISION_ENCODED,
     REQ("tools/call", "\"name\":\"=?base64?QQ==?=\"," META), 503, 0, 0, NULL},

    /* --- resources/read compares the RAW uri --- */
    {"resources/read compares against the raw params.uri, never the "
     "canonicalized form",
     "raw percent-encoded uri matches the raw body value", H_READ_NAME_RAW,
     REQ("resources/read", "\"uri\":\"file:///tmp/a%20b\"," META), 503, 0, 0, NULL},
    {"normalizing before comparing would let two header values both pass",
     "canonicalized uri does not match the raw body value", H_READ_NAME_CANONICAL,
     REQ("resources/read", "\"uri\":\"file:///tmp/a%20b\"," META), 400, -32020, 1, NULL},

    /* --- ordering --- */
    {"parse precedes compare: a body with no fields has nothing to disagree with",
     "unparseable body with mismatched headers too", H_METHOD_WRONG,
     "{not json", 400, -32700, 0, NULL},

    /*
     * --- one rule at a time ---
     *
     * Each of these was added because a mutation of the rule it names SURVIVED
     * the matrix: the case that was supposed to cover the rule was also
     * tripping a second rule, so removing the first changed nothing observable.
     */
    {"MCP-Protocol-Version required, exactly once -> 400 -32020",
     "version absent while every other rule is satisfied", H_METHOD_ONLY,
     REQ("tools/list", META), 400, -32020, 1, NULL},
    {"the header must equal the body's _meta version -> 400 -32020",
     "no _meta version while every other rule is satisfied", H_VERSION_AND_METHOD,
     REQ("tools/list", "\"a\":1"), 400, -32020, 1, NULL},
    {"the header must equal the body's _meta version -> 400 -32020",
     "version disagrees while every other rule is satisfied",
     H_VERSION_WRONG_WITH_METHOD, REQ("tools/list", META), 400, -32020, 1, NULL},
    {"a value carrying the prefix but NOT the suffix is not a sentinel and is "
     "taken literally",
     "prefix without suffix is compared literally", H_CALL_NAME_PREFIX_ONLY,
     REQ("tools/call", "\"name\":\"=?base64?c2VhcmNo\"," META), 503, 0, 0, NULL},
    {"a malformed sentinel is a rejection, never a fallback to a literal compare",
     "malformed payload is not retried as a literal", H_CALL_NAME_BAD_ALPHABET,
     REQ("tools/call", "\"name\":\"=?base64?c2Vh*mNo?=\"," META), 400, -32020, 1, NULL},
    {"there is no URL-safe alphabet",
     "URL-safe spelling that would otherwise decode to the body's name",
     H_CALL_NAME_URLSAFE_DECODABLE,
     REQ("tools/call", "\"name\":\"~~~\"," META), 400, -32020, 1, NULL},

    {"every boundary of the Base64 alphabet decodes to the byte it names",
     "payload using A, Z, a, z, 0, 9, + and /", H_CALL_NAME_ALPHABET,
     REQ("tools/call", "\"name\":\"" ALPHABET_NAME "\"," META), 503, 0, 0, NULL},
    {"one wrong byte out of that payload is still a mismatch",
     "alphabet payload against a different name", H_CALL_NAME_ALPHABET,
     REQ("tools/call", "\"name\":\"" ALPHABET_NAME "x\"," META), 400, -32020, 1, NULL},

    /* --- the happy path --- */
    {"a request that satisfies every rule reaches the placeholder",
     "valid tools/list request", H_VERSION_AND_METHOD,
     REQ("tools/list", META), 503, 0, 0, NULL}
};

static int check_matrix_case(const matrix_case_t *test_case) {
    maelys_mcp_http_adapter_t *adapter = NULL;
    if (maelys_mcp_http_adapter_create(
            MAELYS_MCP_HTTP_PLACEHOLDER_JSON, &adapter) != MAELYS_MCP_OK) {
        return 1;
    }
    slice_headers_t headers = {.pairs = test_case->headers};
    maelys_mcp_http_request_t request = make_request(&headers, test_case->body);
    recording_writer_t state = {0};
    maelys_mcp_http_response_writer_t writer = make_writer(&state);
    int failed = 0;
    if (maelys_mcp_http_adapter_handle(adapter, &request, &writer, NULL) !=
        MAELYS_MCP_OK) {
        fprintf(stderr, "  [%s] _handle did not return OK\n", test_case->name);
        failed = 1;
        goto done;
    }
    /* Whatever the verdict, the seam's shape holds: one begin_json, no stream. */
    if (state.begin_json_calls != 1 || state.begin_stream_calls != 0 ||
        state.status_only_calls != 0) {
        fprintf(stderr, "  [%s] writer was not driven exactly once in JSON mode\n",
            test_case->name);
        failed = 1;
        goto done;
    }
    if (state.status != test_case->expect_status) {
        fprintf(stderr, "  [%s] expected status %d, got %d\n",
            test_case->name, test_case->expect_status, state.status);
        failed = 1;
        goto done;
    }
    if (test_case->expect_status == 503) goto done;

    json_error_t error;
    json_t *body = json_loads(state.body, 0, &error);
    if (!body) {
        fprintf(stderr, "  [%s] refusal body is not JSON: %s\n",
            test_case->name, state.body);
        failed = 1;
        goto done;
    }
    json_t *code = json_object_get(json_object_get(body, "error"), "code");
    if (!json_is_integer(code) ||
        json_integer_value(code) != test_case->expect_code) {
        fprintf(stderr, "  [%s] expected code %d, got %s\n",
            test_case->name, test_case->expect_code, state.body);
        failed = 1;
    }
    /* The id rule: a -32020 can echo the id because the body parsed by the time
     * a header could be compared; the rows above it have nothing to echo. */
    json_t *id = json_object_get(body, "id");
    if (test_case->expect_id) {
        if (!json_is_integer(id) || json_integer_value(id) != 2) {
            fprintf(stderr, "  [%s] expected the request id echoed, got %s\n",
                test_case->name, state.body);
            failed = 1;
        }
    } else if (!json_is_null(id)) {
        fprintf(stderr, "  [%s] expected a null id, got %s\n",
            test_case->name, state.body);
        failed = 1;
    }
    if (test_case->expect_message &&
        !strstr(state.body, test_case->expect_message)) {
        fprintf(stderr, "  [%s] expected the message to carry \"%s\", got %s\n",
            test_case->name, test_case->expect_message, state.body);
        failed = 1;
    }
    json_decref(body);
done:
    maelys_mcp_http_adapter_destroy(adapter);
    return failed;
}

static int the_mcp_header_rejection_matrix(void) {
    int failures = 0;
    for (size_t index = 0u; index < sizeof(MATRIX) / sizeof(*MATRIX); ++index) {
        if (check_matrix_case(&MATRIX[index]) != 0) {
            fprintf(stderr, "  rule: %s\n", MATRIX[index].rule);
            ++failures;
        }
    }
    ASSERT_TRUE(failures == 0);
    return 0;
}

int main(void) {
    static const maelys_test_case_t cases[] = {
        {"JSON mode calls begin_json exactly once", json_mode_answers_once},
        {"stream mode ends exactly once per begin_stream", stream_mode_ends_exactly_once},
        {"the exchange handle is retired before _handle returns",
            cancellation_selects_the_aborted_disposition},
        {"a cancel while the exchange is live aborts the stream",
            a_live_cancel_aborts_the_stream},
        {"the adapter never asks for Authorization",
            the_adapter_never_asks_for_authorization},
        {"arguments are validated rather than trusted", arguments_are_validated},
        {"every MCP-header rule refuses what it says it refuses",
            the_mcp_header_rejection_matrix}
    };
    return maelys_run_tests(cases, sizeof(cases) / sizeof(*cases));
}
