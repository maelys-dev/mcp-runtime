#!/usr/bin/env bash
#
# Single source of truth for the package version: the VERSION file. This
# script derives include/maelys/mcp/version.h's MAJOR/MINOR/PATCH macros from
# it, so nothing ever hand-edits both files. version.h stays a normal
# committed header (so `#include`-ing it works from a plain checkout, with no
# build step), but `make check-version-header` verifies it was produced by
# this exact script — any manual edit that lets the two drift is caught.
#
# Usage: scripts/generate-version-header.sh [--check]
#   (no args)  regenerate include/maelys/mcp/version.h from VERSION
#   --check    exit 1 if the committed header does not match, without writing
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

version="$(cat VERSION)"
if ! [[ "$version" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  echo "VERSION must be plain SemVer (X.Y.Z), got: $version" >&2
  exit 1
fi
major="${BASH_REMATCH[1]}"; minor="${BASH_REMATCH[2]}"; patch="${BASH_REMATCH[3]}"

target="include/maelys/mcp/version.h"
generated="$(cat <<HEADER
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generated from VERSION by scripts/generate-version-header.sh. Do not edit
 * MAJOR/MINOR/PATCH by hand — edit VERSION and regenerate.
 */
#define MAELYS_MCP_VERSION_MAJOR ${major}u
#define MAELYS_MCP_VERSION_MINOR ${minor}u
#define MAELYS_MCP_VERSION_PATCH ${patch}u

/*
 * The ABI number changes whenever a released public C layout or calling
 * convention changes incompatibly. It is independent from the package version.
 */
#define MAELYS_MCP_ABI_VERSION 4u

const char *maelys_mcp_version_string(void);
unsigned int maelys_mcp_abi_version(void);

#ifdef __cplusplus
}
#endif
HEADER
)"

if [ "${1:-}" = "--check" ]; then
  if [ ! -f "$target" ] || [ "$(cat "$target")" != "$generated" ]; then
    echo "error: $target is out of sync with VERSION ($version)." >&2
    echo "Run: scripts/generate-version-header.sh" >&2
    exit 1
  fi
  echo "$target matches VERSION ($version)"
else
  printf '%s\n' "$generated" > "$target"
  echo "wrote $target for version $version"
fi
