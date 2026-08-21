#pragma once

#include "maelys/mcp/error.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The authenticated transport principal seam (docs/authenticated-principal-design.md).
 *
 * This header is deliberately types-only and deliberately not in
 * include/maelys/mcp/http.h. A principal is a property of *a transport that has
 * an authentication boundary*, not of HTTP: a unix-socket listener with
 * SO_PEERCRED and an mTLS listener want the same seam, and burying it in the
 * HTTP listener would mean rewriting it the first time one of them appears.
 *
 * The library implements none of it. It fills
 * maelys_mcp_channel_config_t::context, whose lifetime the ABI 4
 * context_release callback already owns (include/maelys/mcp/channel.h), and
 * everything else - which credentials exist, which authenticators ship, what a
 * principal is - belongs to the transport and the host. The library keeps
 * mechanism; the host keeps policy.
 */

/* Opaque to the runtime and to every transport. The authenticator's own type. */
typedef struct maelys_mcp_principal maelys_mcp_principal_t;

typedef enum maelys_mcp_credential_kind {
    MAELYS_MCP_CREDENTIAL_NONE = 0,       /* loopback trust, no material */
    MAELYS_MCP_CREDENTIAL_BEARER = 1,     /* an Authorization: Bearer value */
    MAELYS_MCP_CREDENTIAL_PEER = 2,       /* unix-socket peer credentials */
    MAELYS_MCP_CREDENTIAL_TLS_CLIENT = 3  /* a verified client certificate */
} maelys_mcp_credential_kind_t;

/*
 * A tagged view over transport-established facts ONLY, buildable from headers
 * alone so that it can be built before a single byte of request body has been
 * read - which is what makes "authenticate before the body" expressible rather
 * than merely recommended.
 *
 * There is deliberately no `headers` member and no parsed-payload member. An
 * authenticator that wanted to decide on clientInfo.name, on _meta, or on a
 * client-written routing header would have to be handed it, and it is not
 * handed it. The prohibition lives in this type signature, not only in prose.
 */
typedef struct maelys_mcp_transport_credentials {
    maelys_mcp_credential_kind_t kind;
    /* The peer address the transport observed, never a header. */
    const char *peer;
    /* Length-explicit, because a credential is not a C string. */
    const void *material;
    size_t material_length;
} maelys_mcp_transport_credentials_t;

typedef struct maelys_mcp_authenticator {
    const char *name;
    void *context;

    /*
     * Called once per inbound transport unit - one POST, one stdio process,
     * one accepted unix-socket connection - before any channel exists.
     * On MAELYS_MCP_OK, *out_principal is a retained reference the caller
     * releases exactly once.
     *
     * MAELYS_MCP_ERR_DENIED   - the credential was present and rejected.
     * MAELYS_MCP_ERR_ARGUMENT - the credential was absent or malformed.
     * anything else           - the authenticator could not decide.
     *
     * The three are distinct because a transport must map them to three
     * different answers, and conflating "rejected" with "could not decide" is
     * how an outage becomes an authorization bypass: over HTTP the first two
     * are 401 and the third is 503, never the other way round.
     */
    maelys_mcp_result_t (*authenticate)(
        void *context,
        const maelys_mcp_transport_credentials_t *credentials,
        maelys_mcp_principal_t **out_principal);

    /* Refcount operations on the authenticator's own type. Both are
     * thread-safe, because under detached channel destruction the release
     * runs on whichever in-flight operation finished last rather than on the
     * thread that authenticated. The runtime calls neither. */
    void (*retain)(void *context, maelys_mcp_principal_t *principal);
    void (*release)(void *context, maelys_mcp_principal_t *principal);

    /* Called once from the transport's teardown, after the last channel that
     * could have carried one of its principals is destroyed. */
    void (*destroy)(void *context);
} maelys_mcp_authenticator_t;

#ifdef __cplusplus
}
#endif
