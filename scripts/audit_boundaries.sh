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

# The launch seam, mechanically. A second fork/exec site is a second bypass of
# whatever confinement the launcher applies, so process creation is permitted
# only under src/process/ - the one directory the Executor adapter replaces.
# Without this rule the next provider kind grows its own fork in review-sized
# increments; with it, a bypass cannot merge. See docs/launch-contract-design.md.
search_process_primitives() {
  local pattern=$1
  shift
  if command -v rg >/dev/null 2>&1; then
    rg -n "$pattern" "$@" --glob '*.[ch]' --glob '!src/process/*'
  else
    grep -R -n -E --include='*.c' --include='*.h' --exclude-dir='process' \
      "$pattern" "$@"
  fi
}

process_primitive='(^|[^[:alnum:]_])(fork|execve|execvp|posix_spawn|waitpid|socketpair)[[:space:]]*\('
if search_process_primitives "$process_primitive" src host; then
  echo "Process launch primitive outside src/process/" >&2
  exit 1
fi

# The seam's own conformance suite proves a provider lifecycle runs on a
# launcher that starts no process at all. A fork in that file would mean it was
# testing the POSIX path again by accident. socketpair is deliberately not in
# this narrower pattern: the fake launcher's transport is one, and a socketpair
# creates no process.
process_creation='(^|[^[:alnum:]_])(fork|execve|execvp|posix_spawn|waitpid)[[:space:]]*\('
if [ -f tests/test_process_launcher.c ] &&
   search_c "$process_creation" tests/test_process_launcher.c; then
  echo "The launcher conformance suite must not create processes itself" >&2
  exit 1
fi

# The transport split, mechanically. docs/http-transport-design.md puts the
# HTTP server layer in host/ so that libmaelys_mcp stays free of listen(2), and
# H1's merge criterion is that this script "confirms libmaelys_mcp gained no
# networking symbol". Without a rule, the next transport grows a listener inside
# the library in review-sized increments; with it, one cannot merge.
#
# sys/socket.h is deliberately absent from the list: the library already uses it
# for the provider socketpair transports, which are local descriptors and not
# networking. What no library file may reach for is an internet address family
# or a listener primitive.
network_header='^#include[[:space:]]*<(netinet/|arpa/inet\.h|netdb\.h)'
if search_c "$network_header" include src; then
  echo "Networking header inside libmaelys_mcp; the server layer belongs in host/" >&2
  exit 1
fi

# `listen` is not in this pattern on purpose: src/modules/subscriptions.c has a
# static function by that name, and a rule that has to be suppressed is a rule
# nobody trusts. socket/bind/accept/getaddrinfo/inet_pton are unambiguous, and a
# listener cannot be built without them.
listener_primitive='(^|[^[:alnum:]_])(socket|bind|accept|accept4|getaddrinfo|inet_pton|inet_ntop)[[:space:]]*\('
if search_c "$listener_primitive" include src; then
  echo "Listener primitive inside libmaelys_mcp; the server layer belongs in host/" >&2
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
