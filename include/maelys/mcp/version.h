#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generated from VERSION by scripts/generate-version-header.sh. Do not edit
 * MAJOR/MINOR/PATCH by hand — edit VERSION and regenerate.
 */
#define MAELYS_MCP_VERSION_MAJOR 0u
#define MAELYS_MCP_VERSION_MINOR 18u
#define MAELYS_MCP_VERSION_PATCH 0u

/*
 * The ABI number changes whenever a released public C layout or calling
 * convention changes incompatibly. It is independent from the package version.
 */
#define MAELYS_MCP_ABI_VERSION 5u

const char *maelys_mcp_version_string(void);
unsigned int maelys_mcp_abi_version(void);

#ifdef __cplusplus
}
#endif
