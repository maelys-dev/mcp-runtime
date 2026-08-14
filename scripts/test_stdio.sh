#!/usr/bin/env bash
set -euo pipefail

host=${1:?host binary required}
provider=${2:?provider binary required}
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

"$host" --allow-effect invalid >"$tmp_dir/invalid-out" 2>"$tmp_dir/invalid-err" && exit 1
grep -q -- '--allow-effect accepts apply, commit, or execute' "$tmp_dir/invalid-err"

meta='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"stdio-test","version":"1"},"io.modelcontextprotocol/clientCapabilities":{}}'
printf '%s\n' \
  "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{$meta}}" \
  "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{$meta}}" \
  "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"example.echo\",\"arguments\":{\"message\":\"stdio\"},$meta}}" \
  'not-json' \
  | "$host" --provider "$provider" >"$tmp_dir/out" 2>"$tmp_dir/err"

test ! -s "$tmp_dir/err"
test "$(wc -l <"$tmp_dir/out" | tr -d ' ')" = 4
grep '"id":1' "$tmp_dir/out" | grep -q '"supportedVersions":\["2026-07-28","2025-11-25"\]'
grep '"id":1' "$tmp_dir/out" | grep -q '"resultType":"complete"'
grep '"id":2' "$tmp_dir/out" | grep -q '"name":"example.echo"'
grep '"id":3' "$tmp_dir/out" | grep -q '"structuredContent":{"message":"stdio"'
grep '"id":null' "$tmp_dir/out" | grep -q '"code":-32700'

printf '%s\n' \
  '{"jsonrpc":"2.0","id":10,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"legacy","version":"1"}}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}' \
  '{"jsonrpc":"2.0","id":11,"method":"tools/list","params":{}}' \
  | "$host" --provider "$provider" >"$tmp_dir/legacy" 2>"$tmp_dir/legacy-err"
test ! -s "$tmp_dir/legacy-err"
test "$(wc -l <"$tmp_dir/legacy" | tr -d ' ')" = 2
grep '"id":10' "$tmp_dir/legacy" | grep -q '"protocolVersion":"2025-11-25"'
test "$(grep '"id":11' "$tmp_dir/legacy" | grep -o '"name":"example\.[^"]*"' | wc -l | tr -d ' ')" = 2

echo "test_stdio: OK"
