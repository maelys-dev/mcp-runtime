#pragma once

#include "maelys/mcp/principal.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The two v1 authenticators (docs/authenticated-principal-design.md, "v1
 * authenticators"). Neither is interesting on purpose: the value of the seam is
 * the seam, not the credentials.
 *
 * They live in host/ rather than in the library for the reason
 * docs/middleware-design.md:560 already settled for configuration - the library
 * keeps mechanism, the host keeps policy - and because the seam they implement
 * (include/maelys/mcp/principal.h) is deliberately types-only so that a
 * unix-socket or mTLS listener can supply its own without either of these being
 * in the way.
 */

/*
 * loopback-trust. One process-wide singleton principal, refcounted but never
 * freed, because the principal *is* "whoever can reach this socket" - stated as
 * such rather than inferred from anything the caller sent.
 *
 * It refuses to be installed on a non-loopback bind, which
 * maelys_http_auth_requires_loopback is how the listener asks.
 */
maelys_mcp_result_t maelys_http_auth_loopback_create(maelys_mcp_authenticator_t *out);

/*
 * static-bearer. One principal per configured token, allocated at configuration
 * time. Compares in constant time and never logs the token or any prefix of it.
 *
 * This is a TEST AND SINGLE-TENANT mechanism. A runtime whose only network
 * credential is a shared secret in a config file is not a multi-tenant server,
 * and docs/authenticated-principal-design.md's MRTR restriction says so in the
 * form that matters: only loopback-trust and a genuinely mono-tenant
 * single-token configuration may be described as safe.
 */
maelys_mcp_result_t maelys_http_auth_static_bearer_create(
    const char *const *tokens,
    size_t token_count,
    maelys_mcp_authenticator_t *out);

/* 1 when this authenticator may only be installed on a loopback bind. */
int maelys_http_auth_requires_loopback(const maelys_mcp_authenticator_t *authenticator);

/* The principal's identifier, for logs and for a host policy that wants to
 * decide on it. Never the credential. */
const char *maelys_http_auth_principal_name(
    const maelys_mcp_authenticator_t *authenticator,
    const maelys_mcp_principal_t *principal);

#ifdef __cplusplus
}
#endif
