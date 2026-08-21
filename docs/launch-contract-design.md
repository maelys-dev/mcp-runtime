# Process launch contract — design

> **Status: design only. Nothing here is implemented.** This document
> specifies the seam through which every child process this runtime starts
> will be launched, the manifest evolution that configures it, and the phasing
> that gets there. It is written now, before any code, so that the decisions
> and their reasons survive and so the implementation can be reviewed against
> them rather than against memory.
>
> Every claim about *current* behaviour below is cited `file:line` against
> `origin/main` at 0.16.0 and was read, not remembered. Claims about future
> behaviour are in the future tense throughout.
>
> The consumer this seam exists for — Maelys Executor, the sandboxing policy
> enforcement point — is **out of this repository**. Phase M4.6 shows only how
> it consumes the seam. Nothing in `src/`, `host/` or `include/` will ever
> know what a sandbox is.

## Purpose

The runtime forks and execs children in two places, and the two copies have
already drifted.

`src/provider/process_provider.c:976-1104` (`spawn_process`) creates a
`socketpair` (`:981`), marks both ends close-on-exec (`:985`), forks (`:991`),
and in the child `dup2`s the peer end onto both stdin and stdout
(`:1000-1001`) before `execve` (`:1013`) with a fixed two-element argv
(`:1002`) and a fixed three-variable environment (`:1003-1012`).

`src/provider/mcp_proxy.c:701-796` (`spawn_upstream`) does the same thing:
`socketpair` (`:706`), close-on-exec (`:710`), fork (`:718`), `dup2` onto
stdin and stdout (`:727-728`), `execve` (`:744`) with the identical fixed
environment (`:734-743`) — differing only in that argv comes from the caller
(`:716-717`) rather than being compiled by the runtime.

Three helper functions are duplicated verbatim between the two files:
`set_close_on_exec` (`process_provider.c:971-974`, `mcp_proxy.c:697-700`),
`monotonic_milliseconds` (`process_provider.c:878-882`, `mcp_proxy.c:615-619`)
and `wait_for_child` (`process_provider.c:884-897`, `mcp_proxy.c:621-634`).

That duplication is the ordinary maintenance argument. It is not the real one.
**The real argument is that a second fork/exec site is a second bypass.** A
sandboxing PEP that governs native providers but not proxied upstreams governs
nothing: an operator who wants to escape it only has to declare their program
as an `mcp-proxy` upstream, where the manifest already hands them full argv
(`docs/manifest.md:58-60`). Both kinds must go through one seam, or the seam
is decorative.

## What the seam is

Three public types, in the repo's existing shape.

`maelys_mcp_response_sink_t` (four function pointers plus a `void *context`)
and `maelys_mcp_provider_config_t` (`.call`, `.destroy`, `.context`) are the
precedents for a callback record here. The launch seam splits that shape in
two — a `const` ops table and a binding that pairs it with per-launcher state
— because a launcher is bound **per provider**, so many bindings will share
one ops table, and the table can live in `.rodata`.

```c
/* include/maelys/mcp/process_launcher.h */

/*
 * What to launch. Compiled by the runtime, never by an MCP request
 * (docs/security-model.md). Every field is read before spawn returns; a
 * launcher must copy anything it needs afterwards.
 */
typedef struct maelys_mcp_process_spec {
    /* Absolute, like every other executable path this runtime accepts. */
    const char *executable_path;
    /*
     * The COMPLETE argv, argv[0] included, NULL-terminated. Never the
     * "extra arguments" a manifest writes - see "Two layers of argv" below.
     */
    char *const *argv;
    /*
     * Opaque to the runtime, which never parses, compares or branches on it.
     * NULL means "the launcher's own default". The default POSIX launcher
     * accepts NULL and "trusted-local" and refuses everything else, loudly.
     */
    const char *execution_profile;
    /* The frame bound the runtime will enforce on this transport. Handed
     * down so a launcher can size a buffer or refuse an absurd value; the
     * runtime enforces it regardless of what the launcher does with it. */
    size_t max_message_bytes;
    /* The budget each rung of the shutdown ladder gets. */
    unsigned int stop_timeout_ms;
    /* Which descriptor arrangement the child gets; see "The child's
     * descriptors". MAELYS_MCP_PROCESS_FD_STDIO is zero, so a zeroed spec
     * reproduces today's behaviour. */
    maelys_mcp_process_fd_layout_t fd_layout;
} maelys_mcp_process_spec_t;

/* What a successful spawn produced. */
typedef struct maelys_mcp_process_instance {
    /*
     * The duplex protocol transport. Owned by the RUNTIME from the moment
     * spawn returns MAELYS_MCP_OK; the launcher must never touch it again.
     * Guaranteed by the launcher to be >= 0, close-on-exec, blocking, and
     * SIGPIPE-safe on write (a write to a dead peer returns EPIPE).
     */
    int protocol_fd;
    /*
     * Whatever the launcher needs to identify what it started. Owned by the
     * LAUNCHER. The runtime stores it, hands it back to wait/stop/destroy,
     * and never dereferences, compares, prints or reaps it. It may be a
     * boxed pid_t, an OCI container id, a VM handle, or an id held by a
     * future maelys-executord. May be NULL if the launcher needs no state.
     */
    void *handle;
} maelys_mcp_process_instance_t;

typedef enum maelys_mcp_process_stop {
    /* Ask. SIGTERM on POSIX; whatever the substrate's polite request is. */
    MAELYS_MCP_PROCESS_STOP_GRACEFUL = 0,
    /* Insist. SIGKILL on POSIX. Must not block on the child. */
    MAELYS_MCP_PROCESS_STOP_FORCED = 1
} maelys_mcp_process_stop_t;

typedef struct maelys_mcp_process_ops {
    /*
     * Start it. On MAELYS_MCP_OK, *out_instance is filled and every
     * resource it names has the ownership stated above. On anything else,
     * *out_instance is untouched, nothing was left running, and the runtime
     * MUST NOT call destroy. *out_error, when set, is caller-owned.
     */
    maelys_mcp_result_t (*spawn)(
        void *context,
        const maelys_mcp_process_spec_t *spec,
        maelys_mcp_process_instance_t *out_instance,
        char **out_error);
    /*
     * Has it exited? Bounded by timeout_ms; *out_exited is 1 if the launcher
     * observed termination within the budget, 0 if it did not. Reaps if the
     * substrate needs reaping. Never called on the request path - only on
     * the shutdown ladder.
     */
    maelys_mcp_result_t (*wait)(
        void *context, void *handle, unsigned int timeout_ms, int *out_exited);
    /* Signal termination. Non-blocking. Idempotent: the ladder calls it
     * twice on a stubborn child, and a stop after exit is not an error. */
    maelys_mcp_result_t (*stop)(
        void *context, void *handle, maelys_mcp_process_stop_t mode);
    /*
     * Release the handle and everything behind it, reaping if that has not
     * happened. Called EXACTLY ONCE per spawn that returned OK, and never
     * for one that did not. A handle is invalid the instant this returns.
     */
    void (*destroy)(void *context, void *handle);
} maelys_mcp_process_ops_t;

typedef struct maelys_mcp_process_launcher {
    /* For diagnostics only; the runtime never branches on it. */
    const char *name;
    const maelys_mcp_process_ops_t *ops;
    void *context;
} maelys_mcp_process_launcher_t;

/* The launcher that reproduces today's behaviour. Statically allocated,
 * stateless, safe to share across providers and threads. */
const maelys_mcp_process_launcher_t *maelys_mcp_posix_launcher(void);
```

### No `struct_size`

`maelys_mcp_process_spec_t` and `maelys_mcp_process_ops_t` carry **no
self-describing size field, and never will**. The repo's ABI discipline
already answers this question and answers it differently:
`docs/abi-policy.md:23-24` states that adding a field to a released public
structure *is* an ABI break and that a compatible extension takes the form of
a new structure or a new versioned constructor, with
`MAELYS_MCP_ABI_VERSION` (`include/maelys/mcp/version.h:19`, currently `3u`)
carrying the change.

The ABI-3 precedent is exactly this shape and is worth reading as the model:
`maelys_mcp_runtime_config_t` lost three fields when the middleware chain
replaced `authorize`/`audit`/`policy_context`, the constant went 2 → 3, and
the migration was documented rather than papered over (`CHANGELOG.md:229`).
No compatibility shim was exported; a new entry point,
`maelys_mcp_runtime_add_compat_policy`, took the old values instead.

A `struct_size` field would buy silent tolerance of mismatched builds. This
project does not want silent tolerance of mismatched builds; it wants
`maelys_mcp_abi_version()` to be checkable and a link error otherwise
(`docs/abi-policy.md:12-14`). Two mechanisms for the same job would double the
audit surface for no gain — the same argument the middleware design made
against `authorize` coexisting with the chain.

**Found while checking this: `docs/abi-policy.md` is stale.** It documents ABI
1 → 2 at `:32-36` and never mentions ABI 3, which shipped and is the current
value of the constant. That is a documentation bug, not a design question, and
is listed under "Contradictions found" below.

## Ownership, stated exactly

The failure modes this seam can produce are fd leaks, zombie children,
double closes and double reaps. Each is a sentence, not an inference.

| Resource | Owner before spawn | Owner after `spawn` == OK | Owner after `spawn` != OK |
|---|---|---|---|
| `protocol_fd` | does not exist | **runtime** | does not exist |
| `handle` | does not exist | **launcher** (runtime holds a token) | does not exist |
| the child itself | does not exist | **launcher** | never started, or already reaped by the launcher |
| `*out_error` | — | not set | **caller**, `free()` it |

- **The runtime closes `protocol_fd` exactly once**, in its own teardown,
  before calling `destroy`. A launcher that closes it after returning OK
  causes a use-after-close in the reader thread; a launcher that keeps a
  second copy open prevents the EOF the runtime relies on for death detection.
- **The runtime never reaps.** `waitpid` will not appear outside
  `src/process/` — enforced mechanically, see M4.1. Today the runtime reaps
  in both providers' teardown (`process_provider.c:941-947`,
  `mcp_proxy.c:646-664`); under the seam those calls become `wait`/`stop`/
  `destroy` and the reaping moves inside the POSIX launcher.
- **`destroy` is exactly-once, and it is the runtime's job to make it so.**
  The runtime nulls its stored handle before calling `destroy`, so a second
  teardown is a no-op rather than a double free. A launcher may assume it is
  never called twice; the M4.4 suite proves the runtime holds up its end.
- **A spawn that failed leaves nothing.** No fd, no child, no handle, and
  emphatically no obligation on the caller to clean up. The launcher unwinds
  its own partial state before returning non-OK.

### The one case where both sides could be right

`spawn` returns `MAELYS_MCP_OK` with `protocol_fd < 0`, or with an fd that is
not a valid descriptor. The launcher believes it succeeded; the runtime cannot
use what it was given, and a child may be running.

The contract resolves it in the runtime's favour, because the runtime is the
only side that can detect it: **on OK with an unusable `protocol_fd`, the
runtime calls `stop(FORCED)` then `destroy`, and fails the spawn with
`MAELYS_MCP_ERR_PROTOCOL`** and a message naming the launcher. It does not
close the fd (there is nothing valid to close) and it does not leak the child
(it stopped it). This is a launcher contract violation, so it is loud; it is
also a conformance case in M4.4, because "the runtime survives a broken
launcher" is exactly the property a pluggable seam has to earn.

Validity is checked as `protocol_fd >= 0` plus one `fcntl(fd, F_GETFD)`. That
is cheap, happens once per provider, and catches the realistic mistake (an
uninitialized or already-closed fd) without pretending to catch every one.

## Two layers of argv

This is the ambiguity most likely to produce a wrong implementation, so it is
stated as an invariant rather than described.

**Manifest layer.** For a `"native"` provider, the new `args` key is
**extra arguments only**. The runtime compiles the full vector:

```
argv[0] = <path>
argv[1] = "--provider"
argv[2..] = args[0..]
argv[n] = NULL
```

Today that vector is a literal two-element array,
`process_provider.c:1002`. `args` extends it and cannot displace either of the
first two elements.

**Launcher layer.** `maelys_mcp_process_spec_t.argv` is **always the complete
vector, argv[0] included**. No launcher ever receives "extras". A launcher
that reorders, prepends or interprets argv is broken; it passes the vector to
the substrate as given.

So there is exactly one place where a partial vector exists — between manifest
parsing and spec construction, inside the host — and exactly one place where
argv is compiled. Everything below the seam sees whole vectors.

### The asymmetry with the proxy, and why it is right

`maelys_mcp_proxy_options_t.argv` (`include/maelys/mcp/provider.h:287-288`) is
already the upstream's **whole** argv, defaulting to `{executable_path}` when
NULL (`mcp_proxy.c:716-717`), and the manifest exposes it that way
(`docs/manifest.md:58-60`). Native `args` will be supplements. Two keys,
similar names, opposite meanings. That is worth justifying rather than
tolerating:

- **The runtime has a protocol invariant to protect for native providers and
  none for upstreams.** `--provider` is what puts a `maelys-provider` binary
  into provider mode. If a manifest could write `argv[1]`, it could invoke the
  same trusted binary in *any other* mode it supports — turning a
  provider declaration into an arbitrary program invocation, without the
  operator writing anything that looks like one. Extras-only fails closed on
  that.
- **An upstream MCP server has an arbitrary CLI.** `--stdio`, `serve`,
  `--transport stdio`, a subcommand, a script path. The runtime makes no claim
  about how a third-party server is invoked and could not compile a correct
  vector if it tried. Full argv is the only shape that works.

The principle underneath: **the runtime dictates argv exactly where it owns an
invariant about the program being launched, and delegates it where it does
not.** The two keys are asymmetric because the two relationships are.

They will keep their existing names — `args` for native, `argv` for proxy —
and `docs/manifest.md` will state the asymmetry in the key table itself rather
than in a footnote. Renaming the proxy's `argv` to make the pair symmetric
would break every existing v1 manifest for a cosmetic gain.

### Secrets do not go in `args`

**A documentation-level prohibition, with no runtime detection.**

argv is world-readable through `ps`, appears in crash reports and core dumps,
is copied into any process-listing telemetry the host machine runs, and on
Linux is readable from `/proc/<pid>/cmdline` for the lifetime of the process.
A token in `args` is a token in the clear.

`docs/manifest.md:113-115` already says v1 has no secrets field and that a
provider needing credentials takes them from its own environment or arguments.
`args` makes the second half of that sentence newly attractive and newly
dangerous, so v2's documentation will say plainly: **do not put credentials,
tokens, keys or passwords in `args`**, and will name the alternatives:

1. **A credentials file with restricted permissions**, path passed in `args`.
   The path is not a secret; the file's mode is the control. Available today.
2. **An inherited descriptor.** Out of scope for M4 — the seam passes exactly
   one descriptor, the protocol transport — but it is where this goes, and
   naming it keeps the door visible.
3. **Executor-resolved secrets.** The launcher is the right place: it already
   sits between configuration and `execve`, and an Executor that can apply a
   sandbox profile can equally inject a credential the manifest only names.
   Out of this repository, by construction.

There will be **no heuristic secret detection** — no scanning for
`--token=`, no entropy tests. A heuristic that catches most secrets teaches
operators that the ones it misses were checked. A prohibition that catches
none, and says so, teaches them to look. The runtime cannot tell a token from
a build hash, and pretending otherwise is worse than silence.

## `executionProfile`

A pass-through string. The runtime never parses it, compares it against a
list, or branches on it. It carries it from manifest to
`maelys_mcp_process_spec_t.execution_profile` and hands it to the launcher.

**The default POSIX launcher accepts `NULL` and the exact string
`"trusted-local"`, and refuses everything else** with
`MAELYS_MCP_ERR_ARGUMENT` and an error naming the profile it was given and the
launcher that refused it. It never silently ignores a profile it does not
implement.

That is the whole point of the field. A manifest that says
`"executionProfile": "seatbelt-readonly"` is a request for confinement. A
runtime with no Executor installed that starts the provider anyway has
answered a security request with an unsandboxed process and no diagnostic.
Failing to start is the only correct behaviour, and it must be the *default*
behaviour, because the deployments most likely to hit it are exactly the ones
that thought they had a sandbox.

### Where the refusal happens, and why not earlier

The refusal belongs to the **launcher's `spawn`**, not to manifest validation.

Manifest validation checks only that the value is a non-empty clean string —
the same treatment `toolPrefix` gets (`host/manifest.c:204-208`). It cannot
check more, because at parse time the host does not know which launcher will
be installed. An embedder that has linked an Executor launcher understanding
`"seatbelt-readonly"` must be able to load that manifest; a host running the
stock POSIX launcher must not. Only the launcher knows.

The cost is that the failure surfaces at provider start rather than at
manifest load, so a bad profile in `providers[7]` is reported after
`providers[0..6]` have already spawned. That is worth naming because
`docs/manifest.md:74-88` is proud of the opposite property for validation, and
this does not have it. The mitigation is that the error is fatal to host
startup and names the provider index, so the operator gets one precise message
rather than a degraded runtime — and the host already tears down cleanly on a
provider spawn failure.

## The child's descriptors

Today the child's fd 0 and fd 1 are both the socketpair peer
(`process_provider.c:1000-1001`, `mcp_proxy.c:727-728`). **The child's stdout
*is* the protocol.** Anything else that writes to fd 1 — a linked C library's
`printf`, a Node native addon, `fs.writeSync(1, ...)`, a chatty dependency —
injects bytes into the frame stream.

The runtime already refuses to live with that risk *for itself*.
`maelys_mcp_isolate_stdout` (`src/transport/stdio_isolation.c:8-35`) duplicates
the real stdout to a private close-on-exec descriptor at fd ≥ 3 and points fd
1 at stderr, before any provider or runtime initialization
(`host/main.c:131-136`, and `docs/security-model.md:40-42` states the
guarantee). The host protects its own protocol channel at the kernel level and
leaves its children to protect theirs in userspace.

### What the SDKs actually do

**Python does the real thing.** `_isolated_transport`
(`sdk/python/src/maelys_mcp_provider/__init__.py:804-808`) is
`maelys_mcp_isolate_stdout` in Python: `os.dup` the stdout fd, mark it
non-inheritable, `os.dup2(stderr, stdout)`, and write frames to the duplicate.
It is on by default (`isolate_stdout: bool = True`, `:815`, used at `:819`).
After that, *nothing* in the process can reach the protocol through fd 1.

**TypeScript does not.** `serveProvider` writes frames with
`process.stdout.write` (`sdk/typescript/src/index.js:514`) and defends the
channel by reassigning three console methods to a stderr writer —
`console.log`, `console.info`, `console.warn` only (`:537-541`), restored on
exit (`:638`). fd 1 remains the protocol. The SDK's own README says so:
"Libraries must still avoid writing directly to `process.stdout`"
(`sdk/typescript/README.md:28-30`). It is an honest warning about a hole, not
a fix.

So the strength of stdout isolation is currently a per-SDK property, ranging
from real to advisory, and it will keep ranging as SDKs are added.

### The arrangement

Move it into the launcher, where it holds for every language at once:

```
MAELYS_MCP_PROCESS_FD_STDIO      (0, the default)
    fd 0  <- socketpair peer          today's arrangement, byte-identical
    fd 1  <- socketpair peer
    fd 2  unchanged (inherited stderr)

MAELYS_MCP_PROCESS_FD_ISOLATED
    fd 0  <- /dev/null                nothing to read from stdin
    fd 1  <- dup of fd 2              stdout IS stderr, at the kernel level
    fd 2  unchanged (inherited stderr)
    fd 3  <- socketpair peer          duplex protocol transport
    env   MAELYS_PROVIDER_FD=3
```

Under `ISOLATED`, a provider *cannot* corrupt its protocol through fd 1,
whatever its language runtime, whatever its dependencies, whether or not its
SDK cooperates, and including any grandchild it spawns. `fs.writeSync(1, ...)`
lands on stderr. A native addon writing to fd 1 lands on stderr. This is
strictly stronger than any per-SDK redirect because it is enforced by the
parent before `execve` and cannot be undone by the child except deliberately.

`MAELYS_PROVIDER_FD` is added to the fixed environment
(`process_provider.c:1003-1012`). That environment stays a closed allowlist —
it gains a fourth entry whose value the *runtime* computes, and no
caller-supplied or request-supplied variable is ever added. That distinction
is what keeps `docs/security-model.md:8-9` true.

SDK side, in every language: if `MAELYS_PROVIDER_FD` is set and parses as a
descriptor, use it for reading and writing; otherwise read stdin and write
stdout as today. Python's `isolate_stdout` becomes redundant under
`ISOLATED` and stays correct under `STDIO`.

### Correcting the framing: this is a declaration, not a negotiation

It is tempting to describe `MAELYS_PROVIDER_FD` as negotiation with
"absent ⇒ stdout, zero breakage". Half of that is true and the half that is
not matters.

The env var works perfectly in one direction: an **old launcher** (no
variable) driving a **new SDK** gets today's behaviour, because the SDK falls
back to stdio. Zero breakage, exactly as claimed.

The other direction does not negotiate at all. A **new launcher** must choose
the arrangement *before* `execve`, when it knows nothing about the binary it
is about to run. If it picks `ISOLATED` and the provider's SDK predates
`MAELYS_PROVIDER_FD`, that provider reads fd 0 and writes fd 1 — and speaks to
`/dev/null` and stderr respectively. It never answers.

Two things make that tolerable, and they should be built deliberately rather
than discovered:

1. **fd 0 is `/dev/null`, not the socket.** An old SDK reading stdin gets
   immediate EOF and exits. The runtime then sees the child die before
   `provider/describe` — the M4.4 "death before describe" case — and reports a
   fast, precise failure. Wiring fd 0 to the socket instead would produce a
   hang until the describe timeout, which is a far worse diagnostic.
2. **`STDIO` is the zero value**, so nothing gets `ISOLATED` by accident. It
   is selected, per provider, by whoever configured that provider.

Which means the arrangement is a property of the *deployment*, not of the
protocol, and it needs a configuration surface. **It is not in manifest v2** —
v2 carries `args` and `executionProfile`, one bump, both, and no third key.
In M4.2 the field is set by the embedder through the spec. The two candidate
resolutions for exposing it, for the owner:

- a v3 manifest key (`"stdoutIsolation": true`), explicit and dull; or
- **let `executionProfile` imply it** — a profile is precisely a statement
  about how a thing is launched, and `"trusted-local-isolated"` (or a profile
  an Executor defines) selecting `ISOLATED` needs no new key ever.

The second is more elegant and is the recommendation, with one honest cost: it
overloads a field whose defining property is that the *runtime* never
interprets it. The launcher would interpret it — which is allowed, since
interpreting profiles is the launcher's entire job — but the mapping then
lives in launcher documentation rather than in the manifest schema. Open
question 1.

### The TypeScript quick fix, and its limits

Independent of the seam, the TS SDK can close most of its hole today. Capture
the real writer first, then point `process.stdout.write` at stderr:

```js
const protocolWrite = process.stdout.write.bind(process.stdout);
process.stdout.write = process.stderr.write.bind(process.stderr);
await serveProvider(provider, { writeLine: (line) => protocolWrite(`${line}\n`) });
```

Order is load-bearing: capture before reassign, or the protocol writes to
stderr too.

Honest limits — this is defense in depth, not the guarantee `ISOLATED` gives:

- `fs.writeSync(1, ...)` and any other direct syscall on fd 1 bypass it
  entirely.
- Native addons writing to fd 1 in C bypass it entirely.
- A child process the provider spawns inherits the real fd 1, which is still
  the protocol socket.
- A library that captured `process.stdout.write` before this ran keeps the
  real one.
- Anything holding `process.stdout` as a stream and calling other methods on
  it may route around the patched `write`.

It should ship, because it converts the most common failure (a dependency
calling `console.log`'s underlying writer, or `process.stdout.write` directly)
from a corrupted protocol into a stderr line. It should ship *with these
limits written down*, because the difference between "mostly protected" and
"protected" is the difference between this and `ISOLATED`.

## Provider death through the seam

Most of this exists. The section states what is verified, then what M4.3 adds,
because "uniform provider-death handling" would otherwise read as new work
that is largely already done.

**Verified as already working:**

- A read failure or EOF on the transport latches a failure.
  `process_reader_main` calls `set_process_failure_locked`
  (`process_provider.c:193-200`); `proxy_reader_main` calls
  `set_proxy_failure_locked` (`mcp_proxy.c:204-211`). `MAELYS_MCP_ERR_NOT_FOUND`
  from the line reader — its EOF signal — is mapped to
  `MAELYS_MCP_ERR_PROVIDER` in both.
- The latch is a latch: `set_process_failure_locked` returns immediately if
  `process->failed` is already set (`process_provider.c:36`), so the first
  cause is the one reported and later noise cannot overwrite it.
- An in-flight exchange wakes on the failure and returns its message rather
  than blocking to its deadline (`process_provider.c:647-653`,
  `mcp_proxy.c:437-443`).
- A worker blocked on a *nested* client request is reached across and settled,
  rather than stranded until the nested deadline
  (`process_provider.c:51-54`, reasoned at `:44-50`, with the lock-order
  justification at `src/internal/internal.h:101-107`).
- **There is no auto-restart, anywhere.** `spawn_process` has exactly one call
  site (`process_provider.c:1227`) and `spawn_upstream` exactly one
  (`mcp_proxy.c:1057`), both inside their one-shot public spawn entry points,
  and the token "restart" does not occur in `src/`, `include/` or `docs/`. A
  dead provider stays dead until the embedder rebuilds it.

**What M4.3 genuinely adds:**

1. **Death is detected through the fd, and only through the fd.** Today that
   is true by accident on the request path and false on the teardown path,
   where both providers call `waitpid` directly. Under the seam it becomes a
   stated invariant: the runtime has no PID, so EOF on `protocol_fd` is its
   sole liveness signal, and `wait` is called only while shutting down. This
   is what makes an OCI or `maelys-executord` handle possible at all, and it
   is a constraint that must be enforced (M4.1's boundary rule) rather than
   merely intended.
2. **One shutdown ladder instead of two different ones.** The current
   sequences differ in order and in first signal.
   `process_destroy` (`process_provider.c:920-947`): `provider/shutdown`
   exchange → set `closing` → `shutdown(SHUT_RDWR)` → join reader → close fd →
   `wait_for_child` → `SIGTERM` → wait → `SIGKILL` → blocking `waitpid`.
   `proxy_destroy` (`mcp_proxy.c:636-664`): set `closing` →
   `shutdown(SHUT_WR)` (EOF is how an MCP stdio server is asked to exit) →
   `wait_for_child` → `shutdown(SHUT_RDWR)` → join → close fd → `SIGTERM` →
   wait → `SIGKILL` → blocking `waitpid`.
   Under the seam, the kind-specific goodbye stays above the seam — a
   `provider/shutdown` exchange for native, a half-close for proxy, because
   those are protocol facts, not launch facts — and everything after it is one
   ladder: close `protocol_fd` → `stop(GRACEFUL)` → `wait(stop_timeout_ms)` →
   `stop(FORCED)` → `destroy`.
3. **The signalling asymmetry is repaired by construction.** See
   "Contradictions found" — two of `spawn_process`'s error paths send
   `SIGTERM` and then block in `waitpid` forever. Once only the launcher
   signals, there is exactly one escalation policy and it is bounded at every
   rung.
4. **`stop(GRACEFUL)` acquires a substrate-independent meaning:** "request
   termination by whatever means this substrate offers". `SIGTERM` for POSIX,
   a stop request for a container, a control message for an executord. The
   runtime never learns which, and never needs a signal number.

## Phasing

| Phase | Scope | Merge criterion |
|---|---|---|
| **M4.1** | Internal extraction into `src/process/posix_launcher.c` plus an internal vtable in `src/process/launcher.h`. Both consumers converted. The three duplicated helpers collapse to one copy. No public API change, no header under `include/`, behaviour byte-identical. | `make check`, `make asan` and `make tsan` green **with no test file modified**. `spawn_process` and `spawn_upstream` contain no `fork`, `execve`, `dup2`, `socketpair`, `kill` or `waitpid`. `scripts/audit_boundaries.sh` gains the process-primitive rule below and passes. |
| **M4.2** | Public `include/maelys/mcp/process_launcher.h`; `maelys_mcp_provider_spawn_with_launcher` and `maelys_mcp_provider_proxy_spawn_with_launcher`. `maelys_mcp_provider_spawn_with_options` and `maelys_mcp_provider_proxy_spawn` become thin wrappers binding `maelys_mcp_posix_launcher()`. `MAELYS_MCP_ABI_VERSION` → 4. | A new `tests/test_process_launcher.c` installs a fake launcher backed by a `socketpair` and an in-process thread serving `provider/describe`, drives describe → call → destroy to completion, and **contains no `fork`** — asserted by the boundary script, which permits process primitives only in `src/process/`. Every existing test unchanged. |
| **M4.3** | The single shutdown ladder; death handling stated and enforced as fd-only; the `SIGTERM`/`SIGKILL` asymmetry repaired. | One ladder implementation, called by both provider kinds. A test kills the child mid-call for **both** kinds and asserts the same result code and the same shape of failure message. No `waitpid` or `kill` outside `src/process/`. |
| **M4.4** | Seam conformance suite, run against **both** the POSIX launcher and the fake launcher. | All eleven cases below pass under both launchers, and under `make asan` and `make tsan`. |
| **M4.5** | Manifest v2: `args` and `executionProfile`, one version bump carrying both. `manifestVersion` 1 **and** 2 accepted. | Every existing v1 fixture passes unmodified. New v2 fixtures cover `args` on native, `args` rejected on proxy, `argv` still rejected on native, `executionProfile` accepted and refused. An unrecognized profile fails host startup with a message naming the profile and the provider index. |
| **M4.6** | **Out of this repository.** The Executor adapter is orchestrator-side. | Nothing merges here. The criterion is that the mapping in "M4.6, elsewhere" holds without a seam change. |

### The boundary rule, which is the load-bearing part of M4.1

`scripts/audit_boundaries.sh` will gain a rule of the shape:

```sh
if search_c '(^|[^[:alnum:]_])(fork|execve|execvp|posix_spawn|waitpid|socketpair)[[:space:]]*\(' \
     src host --glob '!src/process/*'; then
  echo "Process launch primitive outside src/process/" >&2
  exit 1
fi
```

(exact invocation to match the script's `rg`/`grep` fallback structure at
`scripts/audit_boundaries.sh:4-12`).

This is what makes the seam a seam rather than a convention. Without it, the
next provider kind grows its own `fork` in review-sized increments and the
Executor governs two of three launch sites. With it, a bypass cannot merge.

**A correction to the framing this design was given:** the script does *not*
currently enforce general layering that merely needs teaching about
`src/process/`. Its four checks are forbidden application includes (`:14-18`),
shell execution primitives (`:20-23`), a public header including a private one
(`:25-28`), and tools dispatch leaking into `src/core` (`:30-33`). Only the
last two are layering at all, and neither mentions directories the way this
needs. So M4.1 **adds** a rule rather than updating one. The good news is that
the shell-primitive check at `:20-23` is precisely this rule's older sibling —
same technique, same file, same style — so it is a small, idiomatic addition
and not a new mechanism.

Two smaller mechanical notes for M4.1: `LIB_SOURCES` in the Makefile is an
explicit list (`Makefile:45-67`), not a glob, so `src/process/posix_launcher.c`
must be added there and to `DEPENDENCY_SOURCES` (`:69`); and the new file must
not trip the existing shell-primitive check, which it will not, since the
seam never goes near `system`, `popen` or `sh -c`.

### The M4.4 cases

Each runs against both launchers unless marked. "Both kinds" means native and
proxy.

1. **Spawn failure.** Launcher returns non-OK. No fd leaked, no child, no
   `destroy` call, error propagated verbatim. Both kinds.
2. **Death before describe.** Child exits immediately after spawn. Failure is
   `MAELYS_MCP_ERR_PROVIDER`, arrives well before the describe timeout, and
   teardown is clean.
3. **Describe timeout.** Child spawns and says nothing. Fails at
   `describe_timeout_ms`, not later; the ladder still reaps.
4. **Crash mid-call.** Child dies with a call outstanding. The blocked caller
   wakes with the latched failure rather than at its deadline (the property
   `process_provider.c:647-653` already provides — this pins it to the seam).
5. **Graceful stop.** Child exits on `stop(GRACEFUL)`. `stop(FORCED)` is never
   reached; asserted by a counting fake launcher.
6. **Forced escalation.** Child ignores `GRACEFUL`. `FORCED` is reached after
   `stop_timeout_ms` and the teardown completes — no unbounded wait.
7. **Double destroy.** The runtime's teardown path is driven twice. `destroy`
   reaches the launcher exactly once; asserted by a counting fake launcher.
8. **Invalid fd from launcher.** `spawn` returns OK with `protocol_fd = -1`.
   The runtime calls `stop(FORCED)` then `destroy`, fails with
   `MAELYS_MCP_ERR_PROTOCOL`, and leaks nothing.
9. **Both provider kinds through one launcher.** One fake launcher instance
   serves a native provider and a proxy upstream in the same runtime; both
   work; the launcher observes two spawns and two destroys.
10. **Old APIs unchanged.** `maelys_mcp_provider_spawn`,
    `maelys_mcp_provider_spawn_with_options` and
    `maelys_mcp_provider_proxy_spawn` behave exactly as before, with the
    existing tests as written.
11. **Handle opacity.** A fake launcher returns a handle that is *not* a valid
    pointer to anything the runtime could dereference (a small integer cast to
    `void *`) and the runtime round-trips it through `wait`, `stop` and
    `destroy` untouched. This is the test that would fail the day someone
    "optimizes" the handle into a `pid_t`.

Case 11 is the one worth insisting on. Every other case tests behaviour; that
one tests the abstraction, and it is the only one that fails loudly if the
seam quietly re-acquires a PID assumption.

## Manifest v2

One version bump, carrying both additions. `manifestVersion` 2 exists because
`args` and `executionProfile` land together; there is no v2-with-only-`args`.

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

### Schema changes

| Key | Where | Required | Rule |
|---|---|---|---|
| `manifestVersion` | top level | yes | Exactly `1` **or** `2`. |
| `args` | `"native"` entries, v2 only | no | Array of strings, each non-empty after the NUL check. **Extra arguments only**; the runtime supplies `argv[0]` and `--provider`. Absent and `[]` are the same thing. |
| `executionProfile` | both kinds, v2 only | no | Non-empty string. Never interpreted by the runtime; validated for shape only. |

Everything else is unchanged, and that includes the parts that make this
strict:

- **Unknown keys stay fatal at every level** (`host/manifest.c:62-76`, and the
  guarantee in `docs/manifest.md:69-72`). `args` joins `NATIVE_KEYS`
  (`host/manifest.c:46-49`) and `executionProfile` joins both key tables
  (`:46-53`).
- **`argv` on a native entry stays an unknown key**, and `args` on a proxy
  entry is one too. The disjointness `docs/manifest.md:50-52` describes
  survives verbatim; v2 adds one key to each side of it and one to both.
- **v1 documents keep working unchanged.** A v1 document using `args` or
  `executionProfile` is rejected as an unknown key, because it is one — the
  key tables are selected by version. That is stricter than merely accepting
  both, and it is the right stance: a v1 manifest that names `args` was
  written against a v2 understanding and mislabelled, and running it with the
  argument silently dropped is exactly the failure `manifestVersion` exists to
  prevent.
- The three-stage load (`host/manifest.c:11-25`, `:389-419`) is untouched:
  syntax, then whole-document semantics, then construction that cannot fail
  except on allocation.

### Bounds on `args`

Proposed: **at most 64 entries, at most 8 KiB total including separators.**
Rejected at manifest validation with the offending location, in the style of
`validate_optional_uint` (`host/manifest.c:84-98`).

The argument for a bound at all: without one, an oversized `args` becomes
`E2BIG` from `execve` *after* the fork, in the child, where the only signal
available is `_exit(127)` (`process_provider.c:1014`) — which the parent reads
as a generic launch failure. A bound turns that into a precise error naming
`providers[3].args` at load time, before anything is started. The specific
numbers are policy rather than derivation (`ARG_MAX` is at least 256 KiB on
every platform this runs on), chosen to be generous for real providers and
small enough that the failure is legible. Open question 4.

### Host plumbing

`manifest_provider_t` (`host/manifest.h:26-56`) gains `char **args` /
`size_t args_count` and `char *execution_profile`, freed in `manifest_clear`
(`host/manifest.c:421-434`) alongside the existing `argv` loop. The compiled
full vector is built in `host/main.c` at spawn time, not stored in the
manifest struct — the manifest holds what the document said, and the spec
holds what the launcher gets.

## `docs/security-model.md` — draft section

To be added after the existing "Trust boundary" section, replacing nothing.
Drafted here so it can be reviewed as prose rather than assembled later.

> ### Process externality is the sandbox prerequisite
>
> A provider registered in-process through `maelys_mcp_provider_create` or the
> provider SDK runs **inside the runtime's trust boundary**, and no
> configuration changes that. It shares the runtime's address space, its file
> descriptors, its signal dispositions and its heap. A segmentation fault, a
> stack overflow, an `abort()` or a heap corruption in such a provider takes
> the runtime down with it, and a memory-safety error in it can read or
> rewrite any runtime state, including another provider's. The only failure
> mode a native in-process provider can express *safely* is a controlled error
> return.
>
> This is not a defect to be fixed later. It is what in-process means. The
> mitigation is a design rule: **a provider whose code is not trusted at the
> same level as the runtime must not be in-process.**
>
> **Sandboxing requires process externality.** Every mechanism worth the name
> — seccomp, Seatbelt, Landlock, bubblewrap, containers, separate VMs —
> operates on a process or a process tree. None can confine code sharing the
> confiner's address space. The runtime therefore reaches sandboxed execution
> only through the process launch seam
> (`docs/launch-contract-design.md`), which starts every child through one
> vtable, and only for provider kinds that are external processes. The stock
> POSIX launcher applies no confinement and says so: it accepts no
> `executionProfile` other than `"trusted-local"` and refuses to start a
> provider that asks for anything else, rather than starting it unconfined.
>
> Both external provider kinds — native `maelys-provider` children and
> `mcp-proxy` upstreams — go through that one seam. A launch path that
> bypassed it would bypass every future confinement with it, which is why the
> boundary audit forbids process-creation primitives outside `src/process/`.
>
> **The runtime never accepts an executable path, an argument vector, an
> environment variable, an execution profile or a shell fragment from an MCP
> request.** All of them come from host configuration — command-line flags or
> the manifest — which is read once at startup. Nothing a client sends can
> reach `execve`. This holds for every field the launch seam carries, including
> the ones added in manifest v2: `args` and `executionProfile` are
> configuration, never protocol.
>
> Argument vectors are visible to any process that can read the process table
> and to crash reporting. **Credentials must not appear in `args`**; use a
> permission-restricted credentials file whose path is passed instead.

## M4.6, elsewhere

The Executor adapter is orchestrator-side and is named here only to show that
the seam is sufficient for it. Nothing in this repository changes for it.

It implements `maelys_mcp_process_ops_t` and nothing else:

| Op | Executor implementation |
|---|---|
| `spawn` | Resolve `execution_profile` to a policy (Datalog-decided, per the sandbox direction), build the confinement — `sandbox_init` / `sandbox-exec` on macOS, `bwrap` or a seccomp-and-namespace setup on Linux — and start `argv` inside it, returning the duplex transport and its own handle. Refuse a profile it does not know, exactly as the POSIX launcher refuses an unknown one. |
| `wait` | Poll the confined process or the executord for termination, bounded. |
| `stop` | The substrate's request/kill pair. |
| `destroy` | Release the confinement and reap. |

The properties that make this work, each of which is a decision above rather
than a coincidence: the handle is opaque, so a container id or an executord
ticket fits with no runtime change; argv is a complete vector, so nothing
downstream needs to know the runtime's `--provider` convention; the profile is
pass-through, so the runtime carries a policy name it cannot interpret; and
death is observed through the transport, so a confined process with no PID the
runtime can see is still a process the runtime can supervise.

If the adapter needs a fifth operation, the seam was wrong. That is the
concrete falsifiable claim this design makes, and it is worth writing down so
it can be checked against a real implementation rather than agreed with.

## Deliberately not in v1

Each of these is a plausible follow-up; none is decided here, in the style of
`docs/manifest.md:109-119`.

- **No restart or supervision.** The seam can start and stop; it cannot
  restart. There is no auto-restart today (verified above) and adding one
  would need a policy — backoff, attempt limits, whether a restarted provider
  re-describes, what happens to a catalog that changes underneath live
  channels — that belongs to the embedder, not to a launch vtable.
- **No environment beyond the fixed set plus `MAELYS_PROVIDER_FD`.** No
  passthrough of the host's environment, no per-provider variables in the
  manifest. `docs/security-model.md:8-9` is a defence worth keeping intact,
  and a manifest environment map is the most obvious place a token would
  arrive — the same argument that keeps secrets out of `args`.
- **No descriptor passing beyond the protocol transport.** The `ISOLATED`
  layout passes exactly one. A credentials-by-descriptor mechanism is where
  this goes next and is named under the secrets prohibition, but it is not
  designed here.
- **No resource limits in the spec.** No `rlimit`, no cgroup, no CPU or memory
  bound. Those are confinement, they belong behind `execution_profile`, and
  putting them in the spec would put half a sandbox in the runtime — which is
  precisely the shape this design exists to avoid.
- **No launcher for in-process providers.** `src/provider/provider_sdk.c` and
  `maelys_mcp_provider_create` are unaffected. An in-process provider is
  inside the trust boundary by definition; a launch seam has nothing to
  contribute.
- **No per-call launcher.** One launcher is bound per provider at spawn and
  lives for that provider's lifetime.
- **No spawn timeout distinct from the describe timeout.** A launcher that
  blocks forever in `spawn` hangs startup. The mitigation today is that
  `spawn` is a fork and an exec and cannot block; the mitigation for a remote
  executord is that launcher's own problem. Naming it because a
  `maelys-executord` over a socket is exactly the case where it stops being
  true.
- **No executable verification.** Inherited unchanged from
  `docs/manifest.md:116-117`: `path` is trusted as given, with no signature or
  checksum check. The seam does not make this better or worse, but it is the
  natural place someone will expect it, so it is restated rather than left to
  be rediscovered.
- **No Windows.** `fork`/`execve` semantics are assumed throughout. The seam
  is what would *make* a Windows port expressible — a `CreateProcess` launcher
  — but nothing here is designed against it.
- **No hot reload.** Unchanged from v1.

## Contradictions found in current code and docs

Reported as findings, not fixed here; this document changes no code.

1. **`spawn_process` can hang forever on two error paths.** On the
   `SO_NOSIGPIPE` failure (`process_provider.c:1019-1025`) and on the context
   allocation failure (`:1027-1033`), the cleanup sends `SIGTERM` and then
   calls `waitpid(pid, NULL, 0)` with no `WNOHANG` and no timeout. A child
   that ignores or blocks `SIGTERM` — a shell wrapper, a Node process with a
   handler installed, a Python process in an uninterruptible section — leaves
   the runtime blocked in `waitpid` indefinitely, during host startup, with no
   diagnostic. The very next error paths in the same function
   (`:1039-1042`, `:1048-1051`, `:1060-1063`, `:1071-1074`, `:1096-1099`) send
   `SIGKILL` at the same site, so the function is inconsistent with itself.
   `mcp_proxy.c:752-754` sends `SIGKILL` at the structurally identical point,
   so the two files disagree as well. M4.3 removes this by construction — one
   ladder, bounded at every rung — but it is a live latent hang on `main`
   today and does not need to wait for M4.
2. **`docs/abi-policy.md` is stale by one ABI generation.** It describes ABI
   1 → 2 and ABI 2's semantics (`:32-36`) and never mentions ABI 3, which has
   shipped: `MAELYS_MCP_ABI_VERSION` is `3u`
   (`include/maelys/mcp/version.h:19`) and the break is documented in
   `CHANGELOG.md:229`. Anyone reading the policy document for the current ABI
   contract — as this design was asked to — gets a version-old answer. A
   paragraph on ABI 3 mirroring the ABI 2 one would fix it.
3. **The boundary script does not enforce the layering it is credited with.**
   Described above under M4.1: its four checks (`scripts/audit_boundaries.sh:14-33`)
   are application includes, shell primitives, public-header inclusion and
   tools dispatch in `src/core`. There is no general directory layering rule,
   so `src/process/` requires a new rule rather than an amended one.
4. **The two provider kinds disagree about how to ask a child to exit, and
   neither is wrong.** `process_destroy` sends a `provider/shutdown` request
   and waits for its answer (`process_provider.c:926-928`); `proxy_destroy`
   half-closes the socket because EOF on stdin is how an MCP stdio server is
   told to stop (`mcp_proxy.c:645-647`). Both are correct for their protocol.
   Flagged because a reader of the M4.3 "uniform death handling" line may
   expect this difference to be unified too, and it must not be: it is a
   protocol fact and stays above the seam.
5. **`docs/security-model.md` is written against 0.10.0.** Line 82 opens
   "Version 0.10.0 is still a local stdio runtime"; `VERSION` reads `0.16.0`.
   The statement remains true, but the pinned version number will keep aging.
   Noted while drafting the new section rather than changed as a drive-by.

## Open questions for the owner

Everything else in this document is decided, with the argument attached. These
five are not, and each is a place where the design would change materially
depending on the answer.

1. **Does `executionProfile` select the fd layout?** The recommendation is yes
   — a profile is a statement about how a thing is launched, and folding the
   layout into it means no manifest key is ever needed. The cost is that the
   layout mapping lives in launcher documentation rather than in the manifest
   schema, and the field's defining property ("the runtime never interprets
   it") starts carrying more weight than it looks like it carries. The
   alternative is a v3 key. This is the only question that changes the shape
   of a future manifest version.
2. **Is absence of `execution_profile` the same as `"trusted-local"`, or
   distinct?** The draft treats `NULL` as "the launcher's own default" and
   `"trusted-local"` as an explicit request that the POSIX launcher happens to
   satisfy. Collapsing them is simpler; keeping them distinct lets a future
   launcher default to *confined* while still refusing an unknown explicit
   profile. The recommendation is to keep them distinct, because a launcher
   whose default is confinement is the one worth being able to write.
3. **Should `wait` be in the vtable at all?** It could be folded into
   `stop(FORCED)` defined as blocking-until-reaped, giving a three-op table.
   The argument for keeping it: the escalation ladder needs to know whether
   the graceful stop worked, and a launcher whose substrate is remote may not
   be able to make `stop` synchronous cheaply. The argument against: `wait` is
   the one op a remote executord may find expensive to implement honestly, and
   a polling `wait` over a socket is a worse primitive than a blocking one.
   Four ops is the recommendation; the trade is real.
4. **The `args` bound: 64 entries / 8 KiB, or something else?** These are
   chosen, not derived. Any bound is better than none, for the reason given;
   the numbers are policy.
5. **May M4.1 repair the `SIGTERM`-then-blocking-`waitpid` hang, or must it
   wait for M4.3?** M4.1's criterion is "behaviour byte-identical", and this is
   a behaviour change — on an error path that is currently a hang. Repairing it
   inside M4.1 is the honest engineering choice and slightly weakens the
   phase's own criterion. Repairing it separately, before M4 starts, is the
   cleanest and is the recommendation: it is a two-line fix on `main` and it
   should not be hostage to a refactor.
