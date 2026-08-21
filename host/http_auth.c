#include "host/http_auth.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The principal type is the authenticator's own - include/maelys/mcp/principal.h
 * only forward-declares it, and the runtime never looks inside. Both
 * authenticators here share one shape because both answer the same question
 * ("which configured identity is this?") and neither has any state beyond a
 * name and a refcount.
 */
struct maelys_mcp_principal {
    pthread_mutex_t mutex;
    size_t references;
    /* Whether the free is ours to perform. loopback-trust's singleton has
     * static storage, so its release drops the count and stops. */
    int heap_allocated;
    char *name;
};

/* ---------------------------------------------------------------- loopback */

typedef struct loopback_context {
    maelys_mcp_principal_t principal;
} loopback_context_t;

static void principal_retain(void *context, maelys_mcp_principal_t *principal) {
    (void)context;
    if (!principal) return;
    pthread_mutex_lock(&principal->mutex);
    ++principal->references;
    pthread_mutex_unlock(&principal->mutex);
}

static void principal_release(void *context, maelys_mcp_principal_t *principal) {
    (void)context;
    if (!principal) return;
    pthread_mutex_lock(&principal->mutex);
    size_t remaining = principal->references ? --principal->references : 0u;
    pthread_mutex_unlock(&principal->mutex);
    if (remaining || !principal->heap_allocated) return;
    pthread_mutex_destroy(&principal->mutex);
    free(principal->name);
    free(principal);
}

static maelys_mcp_result_t loopback_authenticate(
    void *context,
    const maelys_mcp_transport_credentials_t *credentials,
    maelys_mcp_principal_t **out_principal) {
    loopback_context_t *loopback = context;
    if (!loopback || !credentials || !out_principal) return MAELYS_MCP_ERR_ARGUMENT;
    /*
     * The credential kind is checked rather than ignored. This authenticator's
     * whole claim is that reaching the socket IS the credential, and that claim
     * is only true for the kind that carries no material; being handed a bearer
     * token here would mean the listener was configured for one authenticator
     * and is calling another.
     */
    if (credentials->kind != MAELYS_MCP_CREDENTIAL_NONE) return MAELYS_MCP_ERR_ARGUMENT;
    principal_retain(context, &loopback->principal);
    *out_principal = &loopback->principal;
    return MAELYS_MCP_OK;
}

static void loopback_destroy(void *context) {
    loopback_context_t *loopback = context;
    if (!loopback) return;
    pthread_mutex_destroy(&loopback->principal.mutex);
    free(loopback->principal.name);
    free(loopback);
}

maelys_mcp_result_t maelys_http_auth_loopback_create(maelys_mcp_authenticator_t *out) {
    if (!out) return MAELYS_MCP_ERR_ARGUMENT;
    loopback_context_t *loopback = calloc(1u, sizeof(*loopback));
    if (!loopback) return MAELYS_MCP_ERR_MEMORY;
    loopback->principal.name = strdup("loopback");
    if (!loopback->principal.name) {
        free(loopback);
        return MAELYS_MCP_ERR_MEMORY;
    }
    if (pthread_mutex_init(&loopback->principal.mutex, NULL) != 0) {
        free(loopback->principal.name);
        free(loopback);
        return MAELYS_MCP_ERR_MEMORY;
    }
    /* Refcounted but never freed by a release: the singleton outlives every
     * channel that borrows it and is reclaimed by destroy(). */
    loopback->principal.references = 1u;
    loopback->principal.heap_allocated = 0;
    memset(out, 0, sizeof(*out));
    out->name = "loopback-trust";
    out->context = loopback;
    out->authenticate = loopback_authenticate;
    out->retain = principal_retain;
    out->release = principal_release;
    out->destroy = loopback_destroy;
    return MAELYS_MCP_OK;
}

/* ------------------------------------------------------------ static bearer */

typedef struct bearer_entry {
    char *token;
    size_t token_length;
    maelys_mcp_principal_t *principal;
} bearer_entry_t;

typedef struct bearer_context {
    bearer_entry_t *entries;
    size_t count;
} bearer_context_t;

/*
 * Constant time in the token's length and in the candidate's, so that neither a
 * match prefix nor a length can be recovered by timing. The comparison folds
 * the length difference into the accumulator rather than returning early on it.
 */
static int constant_time_equal(
    const char *left, size_t left_length,
    const char *right, size_t right_length) {
    unsigned char difference = (unsigned char)((left_length ^ right_length) != 0);
    size_t span = left_length < right_length ? left_length : right_length;
    for (size_t index = 0; index < span; ++index) {
        difference |= (unsigned char)(left[index] ^ right[index]);
    }
    return difference == 0u;
}

/*
 * The transport hands the whole Authorization value across as opaque material
 * and never parses it (docs/http-transport-design.md's header table says so),
 * so stripping the scheme is this authenticator's job. `Bearer` is matched
 * case-insensitively because RFC 9110 makes the auth-scheme token
 * case-insensitive; the credential itself is matched byte for byte.
 */
static int strip_bearer_scheme(
    const char *material, size_t length,
    const char **out_token, size_t *out_length) {
    static const char scheme[] = "bearer";
    if (length < sizeof(scheme)) return 0;
    for (size_t index = 0; index < sizeof(scheme) - 1u; ++index) {
        char byte = material[index];
        if (byte >= 'A' && byte <= 'Z') byte = (char)(byte + 32);
        if (byte != scheme[index]) return 0;
    }
    size_t offset = sizeof(scheme) - 1u;
    if (material[offset] != ' ' && material[offset] != '\t') return 0;
    while (offset < length && (material[offset] == ' ' || material[offset] == '\t')) {
        ++offset;
    }
    if (offset >= length) return 0;
    *out_token = material + offset;
    *out_length = length - offset;
    return 1;
}

static maelys_mcp_result_t bearer_authenticate(
    void *context,
    const maelys_mcp_transport_credentials_t *credentials,
    maelys_mcp_principal_t **out_principal) {
    bearer_context_t *bearer = context;
    if (!bearer || !credentials || !out_principal) return MAELYS_MCP_ERR_ARGUMENT;
    /* Absent or malformed is ERR_ARGUMENT and present-and-rejected is
     * ERR_DENIED. Both become 401 over HTTP, but conflating them here would
     * make the distinction unavailable to an authenticator that needs it. */
    if (credentials->kind != MAELYS_MCP_CREDENTIAL_BEARER || !credentials->material) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    const char *token = NULL;
    size_t token_length = 0;
    if (!strip_bearer_scheme(credentials->material, credentials->material_length,
        &token, &token_length)) {
        return MAELYS_MCP_ERR_ARGUMENT;
    }
    /*
     * Every entry is compared, with no early exit on the first match, so the
     * time this takes depends on the number of configured tokens and not on
     * which one matched or on whether any did.
     */
    maelys_mcp_principal_t *matched = NULL;
    for (size_t index = 0; index < bearer->count; ++index) {
        int equal = constant_time_equal(bearer->entries[index].token,
            bearer->entries[index].token_length, token, token_length);
        if (equal) matched = bearer->entries[index].principal;
    }
    if (!matched) return MAELYS_MCP_ERR_DENIED;
    principal_retain(context, matched);
    *out_principal = matched;
    return MAELYS_MCP_OK;
}

static void bearer_destroy(void *context) {
    bearer_context_t *bearer = context;
    if (!bearer) return;
    for (size_t index = 0; index < bearer->count; ++index) {
        /* The token is zeroed before it is freed so that a shared secret does
         * not survive in freed heap for the rest of the process's life. */
        if (bearer->entries[index].token) {
            memset(bearer->entries[index].token, 0,
                bearer->entries[index].token_length);
            free(bearer->entries[index].token);
        }
        principal_release(context, bearer->entries[index].principal);
    }
    free(bearer->entries);
    free(bearer);
}

static maelys_mcp_principal_t *principal_create(const char *name) {
    maelys_mcp_principal_t *principal = calloc(1u, sizeof(*principal));
    if (!principal) return NULL;
    principal->name = strdup(name);
    if (!principal->name) {
        free(principal);
        return NULL;
    }
    if (pthread_mutex_init(&principal->mutex, NULL) != 0) {
        free(principal->name);
        free(principal);
        return NULL;
    }
    principal->references = 1u;
    principal->heap_allocated = 1;
    return principal;
}

maelys_mcp_result_t maelys_http_auth_static_bearer_create(
    const char *const *tokens,
    size_t token_count,
    maelys_mcp_authenticator_t *out) {
    if (!out || !tokens || !token_count) return MAELYS_MCP_ERR_ARGUMENT;
    bearer_context_t *bearer = calloc(1u, sizeof(*bearer));
    if (!bearer) return MAELYS_MCP_ERR_MEMORY;
    bearer->entries = calloc(token_count, sizeof(*bearer->entries));
    if (!bearer->entries) {
        free(bearer);
        return MAELYS_MCP_ERR_MEMORY;
    }
    for (size_t index = 0; index < token_count; ++index) {
        if (!tokens[index] || !*tokens[index]) {
            bearer_destroy(bearer);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        char name[64];
        /* The principal is named by its slot, never by its token or by any
         * prefix of it - a log line that leaks four characters of a shared
         * secret has leaked four characters of a shared secret. */
        int written = snprintf(name, sizeof(name), "bearer-%zu", index);
        if (written < 0 || (size_t)written >= sizeof(name)) {
            bearer_destroy(bearer);
            return MAELYS_MCP_ERR_ARGUMENT;
        }
        bearer->entries[index].token_length = strlen(tokens[index]);
        bearer->entries[index].token = strdup(tokens[index]);
        bearer->entries[index].principal = principal_create(name);
        if (!bearer->entries[index].token || !bearer->entries[index].principal) {
            bearer->count = index + 1u;
            bearer_destroy(bearer);
            return MAELYS_MCP_ERR_MEMORY;
        }
        bearer->count = index + 1u;
    }
    memset(out, 0, sizeof(*out));
    out->name = "static-bearer";
    out->context = bearer;
    out->authenticate = bearer_authenticate;
    out->retain = principal_retain;
    out->release = principal_release;
    out->destroy = bearer_destroy;
    return MAELYS_MCP_OK;
}

int maelys_http_auth_requires_loopback(const maelys_mcp_authenticator_t *authenticator) {
    return authenticator && authenticator->authenticate == loopback_authenticate;
}

const char *maelys_http_auth_principal_name(
    const maelys_mcp_authenticator_t *authenticator,
    const maelys_mcp_principal_t *principal) {
    (void)authenticator;
    return principal ? principal->name : NULL;
}
