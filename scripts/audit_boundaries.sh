#!/usr/bin/env bash
set -euo pipefail

search_c() {
  local pattern=$1
  shift
  if command -v rg >/dev/null 2>&1; then
    rg -n "$pattern" "$@" --glob '*.[ch]'
  else
    grep -R -n -E --include='*.c' --include='*.h' "$pattern" "$@"
  fi
}

forbidden='yavena-hermes|projectctl/|runtime/maelys_|policy/maelys_|agents/'
if search_c "^#include.*($forbidden)" include src host providers tests; then
  echo "Forbidden application dependency found" >&2
  exit 1
fi

if search_c '(^|[^[:alnum:]_])(system|popen)[[:space:]]*\(|/bin/(ba)?sh|sh[[:space:]]+-c' src host; then
  echo "Shell execution primitive found" >&2
  exit 1
fi

if search_c '^#include[[:space:]]+"src/' include; then
  echo "Public header includes a private implementation header" >&2
  exit 1
fi

if search_c '"tools/(list|call)"' src/core; then
  echo "Tools dispatch leaked back into the protocol core" >&2
  exit 1
fi

echo "audit_boundaries: OK"
