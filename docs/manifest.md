# The host provider manifest

`--manifest /absolute/path` loads a JSON file that declares the host's whole
provider set in one place, instead of one `--provider` flag per native
provider on the command line. It is the only way to configure an
`mcp-proxy` (docs/mcp-proxy.md) provider from the CLI - `--provider` alone
only ever spawns a native `maelys-provider` process.

This is host-level configuration, not part of the public library:
`host/manifest.h` and `host/manifest.c` compile into the `maelys-mcp` binary
only (see the Makefile), and nothing under `include/` changes because of it.

Two manifest versions are accepted side by side: `manifestVersion` is `1` or
`2`. A v1 document works exactly as it always has - byte-identical behaviour,
including which keys are legal - and everything below that is new to v2 is
called out as such.

## Format

A v1 document:

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

A v2 document, adding `args` (native only) and `executionProfile` (both
kinds):

```json
{
  "manifestVersion": 2,
  "providers": [
    { "type": "native", "path": "/abs/example-provider",
      "args": ["--root", "/srv/docs"],
      "executionProfile": "trusted-local",
      "describeTimeoutMs": 5000, "callTimeoutMs": 300000 },
    { "type": "mcp-proxy", "path": "/abs/github-mcp", "argv": ["--stdio"],
      "executionProfile": "trusted-local",
      "toolPrefix": "gh." }
  ],
  "allowEffects": ["apply", "commit"]
}
```

Top level:

| Key | Required | Notes |
|---|---|---|
| `manifestVersion` | yes | Must be `1` or `2`. Selects which keys a provider entry may use - see below. |
| `providers` | yes | Non-empty array. |
| `allowEffects` | no | Array of `"apply"` \| `"commit"` \| `"execute"`. |

Every provider entry:

| Key | Required | Notes |
|---|---|---|
| `type` | yes | `"native"` or `"mcp-proxy"`. |
| `path` | yes | Absolute (must start with `/`). |

A `"native"` entry additionally accepts `describeTimeoutMs`, `callTimeoutMs`,
`shutdownTimeoutMs`, `maxMessageBytes` - the fields of
`maelys_mcp_provider_process_options_t` - in both versions, plus `args` and
`executionProfile` under `manifestVersion: 2` only. A `"mcp-proxy"` entry
additionally accepts `argv`, `toolPrefix`, `defaultEffect`, `schemaPolicy`,
`connectTimeoutMs`, `callTimeoutMs`, `maxMessageBytes` - the fields of
`maelys_mcp_proxy_options_t` (docs/mcp-proxy.md) - in both versions, plus
`executionProfile` under `manifestVersion: 2` only. The native and proxy key
sets are disjoint by design: `argv` on a `"native"` entry is an unknown key,
not a silently ignored one, and the same the other way around for `args`.
That disjointness is unchanged by v2, which adds one key to each side and one
key to both.

**`args` and `executionProfile` are unknown keys under `manifestVersion: 1`**,
exactly like any other key that does not belong to the version in use. A v1
document that sets `args` was written against a v2 understanding and
mislabelled its `manifestVersion`; rejecting it by name is more useful than
silently ignoring the field, which is precisely what `manifestVersion`
exists to prevent.

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

### `args` (native, v2 only): extra arguments, never a whole argv

`args` is a JSON array of strings, each validated the same way `argv`
entries are (a real string, no embedded NUL). It is **EXTRA arguments only**,
never the complete vector: the runtime always compiles

```
argv[0] = path
argv[1] = "--provider"
argv[2..] = args[0..]
```

`--provider` is what puts a `maelys-provider` binary into provider mode. A
manifest cannot write `argv[1]` - if it could, it could invoke the same
trusted binary in any other mode it supports, turning a provider declaration
into an arbitrary program invocation without the operator writing anything
that looks like one. `args` fails closed on that: it can only ever add
arguments after the two the runtime already owns. Absent and `[]` are the
same thing - zero extra arguments, byte-identical to a v1 native entry.

**This is the opposite of `mcp-proxy`'s `argv`, which is already the
upstream's WHOLE vector**, defaulting to `{path}` when absent. The asymmetry
is deliberate, not an inconsistency to fix: the runtime holds a protocol
invariant about a native `maelys-provider` binary (it must be launched with
`--provider` and nothing else in that position) and holds no equivalent
invariant about a third-party `mcp-proxy` upstream, which has an arbitrary
CLI (`--stdio`, `serve`, a subcommand, a script path) the runtime could not
compile correctly even if it tried. The runtime dictates argv exactly where
it owns an invariant about the program being launched, and delegates it
where it does not - see "Two layers of argv" in
docs/launch-contract-design.md for the full reasoning. The two keys keep
their existing, asymmetric names (`args` for native, `argv` for proxy)
rather than being renamed to look symmetric, because renaming `argv` for
cosmetic symmetry would break every existing v1 manifest for no real gain.

**Bounds:** at most 64 entries, at most 8192 bytes total (including one
separator byte per entry, the way real argv memory needs a terminator
between entries). Both are rejected at manifest load with a precise,
location-naming error, e.g.:

```
providers[3].args must have at most 64 entries, found 65
providers[3].args must be at most 8192 bytes total (including separators), found 8193
```

Without a bound, an oversized `args` would become `execve`'s `E2BIG` failure
*after* the fork, in the child, where the only signal available is a generic
launch failure. The bound turns that into a precise, load-time error naming
the offending key before anything is started. The specific numbers are
policy, not derivation - generous for any real provider, small enough that
the failure stays legible.

### `executionProfile` (both kinds, v2 only): opaque pass-through

`executionProfile` is a non-empty string, validated for shape only - the
host never interprets it, compares it to a list, or branches on it. It is
carried through the launch seam, read back by a launcher through
`maelys_mcp_process_request_execution_profile`
(docs/launch-contract-design.md), and handed to whatever process launcher the
embedder installed.

**Absence and the literal string `"trusted-local"` are distinct.** `NULL`
(the key absent) means "the launcher's own default"; `"trusted-local"` is an
explicit request that the stock POSIX launcher happens to satisfy. The stock
POSIX launcher accepts only those two things and refuses every other
profile, loudly, naming the profile and the provider index - a manifest that
asks for confinement (`"executionProfile": "seatbelt-readonly"`, for
example) must never be answered with an unsandboxed process and no
diagnostic. That refusal happens when the provider is spawned, not when the
manifest is loaded: the host does not know at load time which launcher will
be installed, so a bad profile in `providers[7]` is reported after
`providers[0..6]` have already started, and is fatal to host startup, naming
the provider index.

## Validation

Strict, at every level: **an unrecognized key anywhere - top level or inside
a provider - is an error naming the key and its location**
(`providers[1].path`, `allowEffects[0]`, ...), never a silently ignored
extra. There is no warning tier. `manifestVersion` selects which key table a
provider entry is checked against, so this applies to `args` and
`executionProfile` exactly as it applies to every v1 key: legal where the
version says they are, unknown everywhere else.

Loading happens in three stages, in order:

1. **JSON syntax.** jansson's own parser, so a malformed file is reported
   with jansson's line and column.
2. **Semantic validation.** The whole document is walked - required keys,
   types, unknown keys, value ranges (`manifestVersion` is `1` or `2`, a
   non-empty `providers`, an absolute `path`, a recognized `schemaPolicy`
   string, a recognized effect string, `argv`/`args` entries all strings,
   `args` within its bounds, `executionProfile` a non-empty string, ...) -
   and the walk does not stop at the first provider that is wrong; every
   provider is checked and the first failure anywhere is reported.
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

## Secrets do not go in `args` (or anywhere else in the manifest)

**There is no secrets field in this manifest, in either version, and `args`
must never be used as one.** `argv` is world-readable through `ps`, appears
in crash reports and core dumps, is copied into any process-listing
telemetry the host machine runs, and on Linux is readable from
`/proc/<pid>/cmdline` for the process's whole lifetime. A token in `args` is
a token in the clear the moment the provider starts.

**There is no heuristic secret detection** - no scanning for `--token=`, no
entropy tests, nothing that tries to catch a credential and reject the
manifest. A heuristic that catches most secrets teaches operators that the
ones it misses were checked; a flat prohibition that catches none, and says
so, teaches them to look instead. The runtime cannot tell a token from a
build hash apart, and pretending otherwise would be worse than saying
nothing.

If a native provider needs a credential, the alternatives, in the order
they become available:

1. **A credentials file with restricted permissions, whose path is passed in
   `args`.** Available today: the path is not a secret, the file's mode is
   the control.
2. **An inherited descriptor.** Not part of this manifest or this phase -
   the launch seam passes exactly one descriptor today, the protocol
   transport - but it is where this goes next, named here so the door stays
   visible.
3. **Executor-resolved secrets.** A process launcher sits between
   configuration and `execve` and could inject a credential the manifest
   only names by reference. Out of this repository, by construction
   (docs/launch-contract-design.md).

## Deliberately not decided here

- **No hot reload.** The manifest is read once, at startup; a changed file
  has no effect on a running host.
- **No secrets field**, in either version - see above.
- **No executable verification.** `path` is trusted as given - no signature
  or checksum check against what actually runs.
- **No manifest key for the `ISOLATED` descriptor layout.** Whether a native
  provider's protocol travels on a dedicated descriptor with stdout wired to
  stderr is derived by the host from the provider's kind, not configured -
  see "The layout is a function of the child's protocol type" in
  docs/launch-contract-design.md. No key is planned for it, in any version.
- **No environment passthrough or per-provider environment map**, in either
  version - the fixed environment stays a closed allowlist the runtime
  computes, never one the manifest extends.

Each is a plausible follow-up and would be its own ADR; none is decided
here.
