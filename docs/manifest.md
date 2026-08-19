# The host provider manifest (v1)

`--manifest /absolute/path` loads a JSON file that declares the host's whole
provider set in one place, instead of one `--provider` flag per native
provider on the command line. It is the only way to configure an
`mcp-proxy` (docs/mcp-proxy.md) provider from the CLI - `--provider` alone
only ever spawns a native `maelys-provider` process.

This is host-level configuration, not part of the public library:
`host/manifest.h` and `host/manifest.c` compile into the `maelys-mcp` binary
only (see the Makefile), and nothing under `include/` changes because of it.

## Format

```json
{
  "manifestVersion": 1,
  "providers": [
    { "type": "native", "path": "/abs/example-provider",
      "describeTimeoutMs": 5000, "callTimeoutMs": 300000, "shutdownTimeoutMs": 2000,
      "maxMessageBytes": 1048576 },
    { "type": "mcp-proxy", "path": "/abs/github-mcp", "argv": ["--stdio"],
      "toolPrefix": "gh.", "defaultEffect": "execute", "schemaPolicy": "skip",
      "connectTimeoutMs": 10000, "callTimeoutMs": 30000, "maxMessageBytes": 1048576 }
  ],
  "allowEffects": ["apply", "commit"]
}
```

Top level:

| Key | Required | Notes |
|---|---|---|
| `manifestVersion` | yes | Must be exactly `1`. |
| `providers` | yes | Non-empty array. |
| `allowEffects` | no | Array of `"apply"` \| `"commit"` \| `"execute"`. |

Every provider entry:

| Key | Required | Notes |
|---|---|---|
| `type` | yes | `"native"` or `"mcp-proxy"`. |
| `path` | yes | Absolute (must start with `/`). |

A `"native"` entry additionally accepts `describeTimeoutMs`, `callTimeoutMs`,
`shutdownTimeoutMs`, `maxMessageBytes` - the fields of
`maelys_mcp_provider_process_options_t`. A `"mcp-proxy"` entry additionally
accepts `argv`, `toolPrefix`, `defaultEffect`, `schemaPolicy`,
`connectTimeoutMs`, `callTimeoutMs`, `maxMessageBytes` - the fields of
`maelys_mcp_proxy_options_t` (docs/mcp-proxy.md). The two key sets are
disjoint by design: `argv` on a `"native"` entry is an unknown key, not a
silently ignored one, and the same the other way around.

All numeric fields are optional; `0` or absent both mean "the existing
default" - the same default the underlying spawn call already applies to a
zero field (e.g. `MAELYS_MCP_DEFAULT_PROVIDER_CALL_TIMEOUT_MS`), independent
of any `--provider-*-timeout-ms` flag the host was started with. `argv`,
when present, is a JSON array of strings and becomes the upstream's argv
exactly (`{path}` alone is the default when `argv` is absent, matching
`maelys_mcp_proxy_options_t`). `defaultEffect` and `allowEffects` entries are
parsed with `maelys_mcp_tool_effect_parse`; `allowEffects` entries are
further restricted to `"apply"`, `"commit"`, `"execute"` - the same
restriction `--allow-effect` enforces, since gating `"read"` or `"preview"`
is meaningless (see `host/main.c`). `schemaPolicy` is one of `"strict"`
(the default), `"skip"`, `"passthrough"` - see docs/mcp-proxy.md.

## Validation

Strict, at every level: **an unrecognized key anywhere - top level or inside
a provider - is an error naming the key and its location**
(`providers[1].path`, `allowEffects[0]`, ...), never a silently ignored
extra. There is no warning tier.

Loading happens in three stages, in order:

1. **JSON syntax.** jansson's own parser, so a malformed file is reported
   with jansson's line and column.
2. **Semantic validation.** The whole document is walked - required keys,
   types, unknown keys, value ranges (`manifestVersion == 1`, a non-empty
   `providers`, an absolute `path`, a recognized `schemaPolicy` string, a
   recognized effect string, `argv` entries all strings, ...) - and the walk
   does not stop at the first provider that is wrong; every provider is
   checked and the first failure anywhere is reported.
3. **Construction.** Only starts once stage 2 has accepted the entire
   document. A manifest that is invalid in `providers[3]` never leaves
   `providers[0..2]` half-built: construction either produces the whole
   parsed manifest or nothing, and a rejected manifest is a `manifest_load`
   failure that touches nothing the caller can observe as a partial result.

## Composition with `--provider` / `--allow-effect`

`--manifest` composes with the existing flags rather than replacing them:

- Manifest providers are added **after** every `--provider` provider, in the
  manifest's own array order.
- `--allow-effect` and the manifest's `allowEffects` are **OR-ed together** -
  neither one replaces the other, and either alone is enough to gate an
  effect on.

## Skipped-tool reporting

When a manifest-declared `mcp-proxy` provider uses `"schemaPolicy": "skip"`
and at least one of its upstream tools was actually dropped (an unsupported
`inputSchema` - see docs/mcp-proxy.md), the host prints one line to stderr
per affected provider, naming the dropped tools. This is guarded to fire
only when something was actually skipped, so a clean run's stderr stays
empty (`scripts/test_stdio.sh` asserts exactly that).

## Deliberately not in v1

- **No hot reload.** The manifest is read once, at startup; a changed file
  has no effect on a running host.
- **No secrets.** There is no field for credentials or tokens; a provider
  that needs them takes them from its own environment or arguments, outside
  the manifest.
- **No executable verification.** `path` is trusted as given - no signature
  or checksum check against what actually runs.

Each is a plausible follow-up and would be its own ADR; none is decided here.
