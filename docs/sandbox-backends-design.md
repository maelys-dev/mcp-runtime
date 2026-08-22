# Public sandbox backends — design

> **Status: design only. Nothing here is implemented.** This is milestone **M3**
> of the launch-contract track (`docs/launch-contract-design.md:1734-1745`),
> targeted at release **0.21.0**. It specifies two in-tree process launchers —
> Seatbelt on macOS, bubblewrap on Linux — the static profile vocabulary they
> apply, the host configuration that authorizes a profile, and the phasing that
> gets there. It is written before any code so that the decisions and their
> reasons survive, and so the implementation can be reviewed against them rather
> than against memory. That is the form `docs/http-transport-design.md` and
> `docs/launch-contract-design.md` established.
>
> **Every claim about current behaviour is cited `file:line` against `1dc974a`
> (0.20.0), and was read, not remembered.** Claims about future behaviour are in
> the future tense throughout.
>
> **The Seatbelt sections rest on experiment, not recollection.** Six properties
> of `sandbox-exec` that this design depends on were run on macOS 15.5
> (build 24F74) before being written down, and one of them overturned the
> obvious implementation. What was run, and what it returned, is in
> [What was measured, and what it changed](#what-was-measured-and-what-it-changed).
> The corresponding Linux properties were **not** measured — no Linux machine was
> in reach — and every unmeasured Linux claim is marked as such rather than
> stated flatly.
>
> **One correction to the milestone table this document inherits.**
> `docs/launch-contract-design.md:1744` says M3 is "Integration in `mcp-runtime`
> 0.20". 0.20.0 was consumed by M2, the ABI 5 implementation itself
> (`CHANGELOG.md:3`), so M3 lands in 0.21.0. The scope also grew: that row
> describes integration, and this milestone ships **backends**, because
> `mcp-runtime` must be able to confine a provider with nothing else installed.

## Contents

1. [Why the backends are in this repository](#why-the-backends-are-in-this-repository)
2. [The line this document does not cross](#the-line-this-document-does-not-cross)
3. [What was measured, and what it changed](#what-was-measured-and-what-it-changed)
4. [What a backend is](#what-a-backend-is)
5. [The decisions](#the-decisions)
6. [Profile definitions: the operator's vocabulary](#profile-definitions-the-operators-vocabulary)
7. [`executionProfile` × `sandboxMode` × `--allow-profile`](#executionprofile--sandboxmode----allow-profile)
8. [The v1 primitive surface](#the-v1-primitive-surface)
9. [The Seatbelt backend](#the-seatbelt-backend)
10. [The bubblewrap backend](#the-bubblewrap-backend)
11. [The network is not a boolean](#the-network-is-not-a-boolean)
12. [Backend selection, and a platform with no backend](#backend-selection-and-a-platform-with-no-backend)
13. [Naming: "process provider" is two different things](#d20--process-provider-and-in-process-provider-stop-being-near-homonyms) *(decision D20)*
14. [Test strategy](#test-strategy)
15. [CI, and the fact that this repository has never run a test on macOS](#ci-and-the-fact-that-this-repository-has-never-run-a-test-on-macos)
16. [Phases](#phases)
17. [Not in v1](#not-in-v1)
18. [Open questions](#open-questions)

## Why the backends are in this repository

`docs/security-model.md:139-146` describes the trust boundary as it stands:
configured provider binaries are trusted local code, the fixed environment
prevents accidental credential inheritance, and nothing sandboxes filesystem,
network, CPU or memory. The last sentence of that paragraph — "A later sandbox
adapter may use OS facilities or containers" — has until now pointed out of the
repository, at Maelys Executor.

That arrangement has one consequence nobody chose: **`mcp-runtime` on its own
cannot confine anything.** Every mechanism it has for confinement is a refusal.
`src/process/posix_launcher.c:147-149` is the whole of it:

```c
static int profile_is_accepted(const char *profile) {
    return !profile || strcmp(profile, "trusted-local") == 0;
}
```

A manifest that says `"executionProfile": "seatbelt-readonly"` gets
`MAELYS_MCP_ERR_ARGUMENT` and a message naming the profile
(`src/process/posix_launcher.c:245-253`), which is the correct answer and an
unhelpful one: the operator asked for confinement, was told no, and has nowhere
to go except a separate distribution. A runtime whose only answer to "sandbox
this provider" is "install something else" has made sandboxing an add-on, and
security that is an add-on is security most deployments do not have.

So the base backends become public and in-tree. After M3, a plain
`mcp-runtime` build on macOS or Linux can start a provider that cannot read the
user's home directory, cannot write outside a named tree, and cannot open a
socket — with no Maelys distribution present and no out-of-tree launcher
linked.

This does not make the seam redundant. It makes the seam **used by something
that ships**, which is the same argument `src/process/posix_launcher.c:23-29`
already makes for the POSIX launcher reading the request through the public
getters rather than reaching around them: a contract whose only implementations
are out of tree is a contract nobody in tree has to keep working.

## The line this document does not cross

Applying a profile is mechanism. Deciding a profile is policy. **The mechanism
is public and lives here; the engine that decides is private and does not.**

| Here, public | Elsewhere, private |
|---|---|
| Named profiles an operator wrote by hand | Profiles a Datalog PDP computes per call, per principal, per repository state |
| Two backends: Seatbelt, bubblewrap | OCI, Firecracker, Apple Container |
| Launching on this machine | A remote `executord` launching on another |
| `executionProfile` resolved by table lookup | `executionProfile` resolved by inference over facts |

The boundary is load-bearing rather than decorative, and it is drawn where it is
because of what a launcher is: `include/maelys/mcp/process_launcher.h:302-417`
is four operations, and none of them takes a policy question. A backend that
resolves a name to a fixed rule set needs no facts about the caller. A backend
that *derives* a rule set needs the identity, the request, the repository, and
an inference engine — a different kind of component, with a different release
cadence and a different threat model.

Naming the private half here is deliberate. `docs/launch-contract-design.md:1734-1745`
already carries the E-track in its milestone table, and a design that pretended
the private engine did not exist would leave the next reader to rediscover why
the public profile format has no conditionals in it. It has none because
conditionals are the other side of the line.

**What this means for the file format:** the profile file is a *vocabulary*, not
a program. It has no variables, no conditionals, no includes, no inheritance and
no expressions. Every one of those is the beginning of a policy language, and
the policy language is not this repository's.

## What was measured, and what it changed

Six properties of `/usr/bin/sandbox-exec` were run on macOS 15.5 (24F74), on the
binary shipped by the OS (`-rwxr-xr-x root wheel 102560 May 4 2025`). They are
recorded because two of them are the reason this design looks the way it does,
and because a reader who wants to disbelieve them can re-run them.

**1. Rule order is last-match-wins.** With `(allow default)` and a
deny/allow pair naming the same subpath, the read succeeded when `allow` came
last and failed with `Operation not permitted` when `deny` came last. Same two
rules, opposite order, opposite outcome.

**2. A path interpolated raw into a rule can delete a `deny`.** Given the
template `(allow file-read* (subpath "<PATH>"))` appended after a
`(deny file-read* (subpath "<SECRET>"))`, the path

```
/tmp/x")) (allow file-read* (subpath "/")) (allow file-read* (subpath "/tmp
```

closes the rule, opens a new one covering `/`, and — by property 1 — wins,
because it lands after the deny. The confined `cat` printed the secret and
exited 0. With `"` escaped as `\"` and nothing else changed, the same path
produced `Operation not permitted`. **This is the injection this design's first
phase exists to make impossible**, and it is not theoretical: it is one shell
line away from a working bypass.

**3. `(param "NAME")` is accepted where a path pattern is accepted, and a
hostile parameter value does not inject.** `(deny file-read* (subpath (param
"DENIED")))` behaved identically to the literal form, and passing the payload
from property 2 as a `-D` value produced no rule at all — the deny held.
`subpath` and `literal` both accept a `param`, and one rule may carry several.

**This is the measurement that changed the design.** The obvious implementation
— generate profile text with paths escaped into it — makes correctness depend on
an escaping function being right for every path anyone ever configures.
Parameters make the profile text contain **no operator-supplied bytes at all**.
Escaping survives as defence in depth, not as the control. See
[D12](#d12--paths-travel-as-parameters-never-as-profile-text).

**4. An undefined parameter is a hard refusal, not an empty value.**
`(param "MISSING")` with no matching `-D` produced
`invalid data type of path filter; expected pattern, got boolean` and
`sandbox-exec` refused to start the child. A generator that emits a rule
referencing a parameter it forgot to pass therefore **cannot** silently produce
a weaker sandbox; it produces a spawn failure. That property is worth more than
it looks, and this design leans on it.

**5. `sandbox-exec` execs in place: the pid is preserved and the exit status is
the child's own.** A test harness forked, exec'd
`sandbox-exec -p ... ./selfreport`, and compared pids: the pid the harness
waited on and the pid the confined program reported were the same number. A
program returning 7 was reported `WIFEXITED, WEXITSTATUS == 7`; a program
raising `SIGKILL` was reported `WIFSIGNALED, WTERMSIG == 9`.

Consequence: on macOS the ABI 5 exit-status contract
(`include/maelys/mcp/process_launcher.h:104-118`) is satisfied *for free*, and
the stop ladder's `kill()` reaches the real child. The Seatbelt backend is
structurally the POSIX launcher with a different argv. See
[D18](#d18--exit-status-fidelity-is-exact-on-macos-and-lossy-on-linux).

**6. `(deny default)` alone cannot start a dynamically linked binary.**
`sandbox-exec -p '(version 1)(deny default)' /bin/cat /etc/hosts` failed with
`execvp() of '/bin/cat' failed: Operation not permitted` — before `cat` ran at
all. A usable deny-default profile therefore needs a **base preamble** of
allowances that have nothing to do with the operator's intent and everything to
do with what dyld, the shared cache and the loader need on this OS version. That
preamble is exactly the part an OS update can break, which is why
[D14](#d14--last-match-wins-and-the-base-preamble-are-tested-invariants-not-comments)
makes it a test rather than a comment.

**Not measured.** Everything about bubblewrap. No Linux machine was available
while this was written, and the repository has no Linux fixture that could stand
in. Every bubblewrap claim below is marked **(unverified)** where it is an
assertion about `bwrap` behaviour rather than a decision about this design, and
phase [S4](#s4--the-bubblewrap-backend) begins by measuring them. Two of them —
signal forwarding and exit-status fidelity — are load-bearing enough that S4
cannot merge without pinning them, and they appear again in
[Open questions](#open-questions).

## What a backend is

A backend is an **ABI 5 launcher and nothing else**: a
`maelys_mcp_process_ops_t` table with the four operations
(`include/maelys/mcp/process_launcher.h:302-417`), created through
`maelys_mcp_process_launcher_create` (`:469`), with a public constructor beside
`maelys_mcp_posix_launcher_create` (`:515`).

```c
maelys_mcp_result_t maelys_mcp_seatbelt_launcher_create(
    const maelys_mcp_sandbox_profiles_t *profiles,
    maelys_mcp_process_launcher_t **out_launcher,
    char **out_error);

maelys_mcp_result_t maelys_mcp_bubblewrap_launcher_create(
    const maelys_mcp_sandbox_profiles_t *profiles,
    maelys_mcp_process_launcher_t **out_launcher,
    char **out_error);
```

Both return a new reference, exactly like any other launcher, and both are
released with `maelys_mcp_process_launcher_release` (`:490`). The profile set is
**retained** by the launcher, not borrowed: a launcher that outlives the
configuration it was built from is the use-after-free ABI 4 was refactored to
prevent (`:420-439`), and there is no reason to reintroduce it one layer up.

Everything the seam already guarantees is inherited rather than re-decided:

| Inherited from ABI 5 | Where |
|---|---|
| The complete argv reaches the substrate as given, `argv[0]` included, nothing reordered or interpreted | `include/maelys/mcp/process_launcher.h:158-168` |
| The environment is a closed allowlist the runtime compiled, applied under the platform rule | `:212-298` |
| `STDIO` and `ISOLATED` descriptor layouts | `:57-86` |
| Full exit status: `exited`, `exit_code`, `term_signal`, exactly one of the last two | `:104-118` |
| The bounded graceful→forced stop ladder, run by the runtime | `:88-93`, `:378-398` |
| `release` exactly once per successful spawn, local only, never blocking | `:399-416` |
| Refuse a profile you cannot apply, loudly, naming it | `:175-184` |

The last row is the one this milestone changes the meaning of. Today "a profile
this launcher cannot apply" means everything except `NULL` and `trusted-local`.
After M3 the sandbox backends implement a set of names, and the refusal applies
to everything outside it — same rule, larger set.

Both files live in `src/process/`, which is not a preference:
`scripts/audit_boundaries.sh:41-44` forbids `fork`, `execve`, `execvp`,
`posix_spawn`, `waitpid` and `socketpair` anywhere else under `src` and `host`.
`scripts/audit_boundaries.sh:20-23` separately forbids `system`, `popen` and
`sh -c`, so **no backend may shell out to build its command line** — `bwrap` and
`sandbox-exec` are reached by `execve` with a vector the backend built, never by
a shell fragment. Both rules are already in force and neither needs amending;
they are quoted here because they constrain the implementation and an
implementer who discovers them at `make check` time will have written the wrong
code first.

## The decisions

Twenty decisions, each with the reasoning that produced it. Decisions **D1**,
**D4**, **D7**, **D8**, **D16** and **D17** implement constraints set by the
repository owner and are recorded with their justification rather than
re-argued; the rest are this document's.

| # | Decision | One line |
|---|---|---|
| [D1](#d1--the-base-backends-are-public-and-in-tree) | Public backends | `mcp-runtime` confines on its own, with no Maelys distribution present |
| [D2](#d2--a-backend-is-a-launcher-and-nothing-else) | Launcher, nothing else | No new seam, no second vtable, no runtime awareness of confinement |
| [D3](#d3--executionprofile-selects-the-launcher) | Profile selects launcher | The host picks POSIX or sandbox per provider; the POSIX launcher is untouched |
| [D4](#d4--profiles-are-static-named-and-written-by-the-operator) | Static named profiles | The runtime resolves a name to a table entry and never computes a rule |
| [D5](#d5--one-json-file-loaded-by---sandbox-profiles-parsed-in-host) | One JSON file, parsed in `host/` | `--sandbox-profiles /abs/path.json`; the library takes a built object, not a filename |
| [D6](#d6--profile-names-reuse-the-platform-token-grammar) | Name grammar | The platform-token grammar, 1–64 bytes; `trusted-local` is reserved |
| [D7](#d7----allow-profile-is-the---allow-effect-symmetry) | `--allow-profile` | The manifest declares, the host authorizes; unauthorized is a startup refusal |
| [D8](#d8--sandboxmode-is-disabledrequired-and-nothing-else) | `sandboxMode` | `disabled` \| `required`, manifest v3 plus `--require-sandbox`, OR toward stricter, no `preferred` |
| [D9](#d9--every-refusal-that-can-happen-at-startup-happens-at-startup) | Refuse early | Definition, authorization and expressibility are checked before any provider spawns |
| [D10](#d10--the-v1-primitive-surface-is-six-primitives) | Six primitives | read, write, exec, network, unix sockets, delete protection |
| [D11](#d11--a-backend-refuses-a-profile-it-cannot-express-in-full) | Refuse, never weaken | An inexpressible primitive fails launcher creation, naming primitive and backend |
| [D12](#d12--paths-travel-as-parameters-never-as-profile-text) | Parameters, not text | Generated SBPL contains no operator-supplied bytes; escaping is defence in depth |
| [D13](#d13---p-never--f-no-temporary-profile-file) | `-p`, never `-f` | The profile is an argv element; no temp file, no lifetime, no race |
| [D14](#d14--last-match-wins-and-the-base-preamble-are-tested-invariants-not-comments) | Tested invariants | Rule order and the deny-default preamble are pinned by live tests |
| [D15](#d15--argv0-must-equal-the-executable-path-or-the-backend-refuses) | `argv[0]` constraint | Both wrappers set `argv[0]` from the program path; a mismatch is refused, not silently altered |
| [D16](#d16--the-network-is-off-or-inherited-and-nothing-else-in-v1) | Network v1 | `none` \| `inherit`; loopback proxy and domain allowlists deferred, and named |
| [D17](#d17--the-backend-is-chosen-by-platform-with-no-flag-to-choose-it) | Platform selection | One backend per platform, chosen at compile time; no flag picks one |
| [D18](#d18--exit-status-fidelity-is-exact-on-macos-and-lossy-on-linux) | Status fidelity | Measured exact on macOS; the Linux wrapper's loss is reported, never fabricated around |
| [D19](#d19--the-seatbelt-backend-does-not-merge-without-a-macos-ci-job) | macOS CI | This repository has never run a test on macOS; the Seatbelt backend cannot be the first untested thing |
| [D20](#d20--process-provider-and-in-process-provider-stop-being-near-homonyms) | Naming | Only *in-process* is unsandboxable; the docs stop making the reader infer which is meant |

### D1 — The base backends are public and in-tree

**Decision.** Seatbelt and bubblewrap backends ship in `src/process/`, with
public constructors in `include/maelys/mcp/`, as part of this repository.

**Why.** The argument is in [Why the backends are in this repository](#why-the-backends-are-in-this-repository)
and reduces to one sentence: a runtime whose only response to a confinement
request is a refusal has made confinement someone else's product.
`docs/security-model.md:166-171` already states that sandboxing requires process
externality and that the runtime reaches it only through the launch seam. M3 is
that sentence acquiring an implementation.

**The cost, stated.** Two OS-specific mechanisms enter a codebase that has kept
its platform surface to a `PATH` string and two condition-variable helpers
(`src/process/launcher.h:52-55`, `src/core/common.c:16`, `:73`, `:97`). That is
a real increase in what this project must keep working on two operating systems,
and [D19](#d19--the-seatbelt-backend-does-not-merge-without-a-macos-ci-job) is
the price of admission.

### D2 — A backend is a launcher and nothing else

**Decision.** No new seam, no new vtable, no configuration path into the
runtime, no code above `src/process/` that knows what a sandbox is.

**Why.** `include/maelys/mcp/process_launcher.h:13-23` says the seam exists so
that confinement "can be installed once and govern every launch", and that
"Nothing in this runtime knows what confinement is; a launcher does." Making the
in-tree backends ordinary launchers is what keeps that sentence true after they
land. The alternative — a sandbox-aware code path beside the launcher path —
would give confinement a second entry point, and a second entry point is a
second thing to bypass.

The mechanical proof is already written and already runs:
`scripts/audit_boundaries.sh:41-44` fails the build if a process primitive
appears outside `src/process/`. If a backend needed anything above the seam, it
would show up there.

### D3 — `executionProfile` selects the launcher

**Decision.** The host chooses a launcher **per provider**, from that provider's
own `executionProfile`:

| `executionProfile` | Launcher bound for that provider |
|---|---|
| absent | POSIX (`maelys_mcp_posix_launcher_create`) |
| `"trusted-local"` | POSIX |
| any other name | the platform sandbox backend |

A sandbox backend is therefore **never asked to start a child without a
profile**, and has no unconfined code path at all.

**Why.** The alternative is a per-host launcher plus a notion of "the backend's
default profile", and that notion is where silent degradation lives. If the
Seatbelt backend had a default, an operator who forgot `executionProfile` on one
provider would get whatever the default was — most plausibly nothing, and
silently. With per-provider selection there is nothing to forget: an absent
profile picks a launcher that says, in its own header, that it applies no
confinement (`include/maelys/mcp/process_launcher.h:502-514`).

Per-provider selection costs nothing structurally.
`maelys_mcp_provider_spawn_with_launcher` and
`maelys_mcp_provider_proxy_spawn_with_launcher`
(`include/maelys/mcp/process_launcher.h:536`, `:542`) already take a launcher per
call, and the launcher is refcounted per provider (`:426-439`), so one host
holding two launchers is the arrangement the ABI was designed for rather than a
strain on it.

**What this preserves.** `src/process/posix_launcher.c:147-149` does not change,
and neither does the test that pins it. `tests/test_process_launcher.c:1454-1459`
asserts that the POSIX launcher refuses `"seatbelt-readonly"` with an error
naming the profile. After M3 that test still passes and still means what it
meant: the POSIX launcher never confines anything, whatever else is installed.
It stops being the *only* answer without becoming a *different* answer.

### D4 — Profiles are static, named, and written by the operator

**Decision.** An operator defines a fixed set of named profiles. A manifest
requests one by name. The backend resolves the name to a table entry. An unknown
name is refused. The runtime never computes, derives, merges or specializes a
profile.

**Why.** This is the public side of
[the line this document does not cross](#the-line-this-document-does-not-cross).
It also makes the whole mechanism auditable by reading: the confinement a
provider runs under is one named entry in one file, and answering "what could
this provider reach?" is a lookup rather than an evaluation. Once profiles can
be computed, that question needs the engine, the facts and the moment — which is
a fine property for a policy engine to have and a terrible one for the
mechanism underneath it.

### D5 — One JSON file, loaded by `--sandbox-profiles`, parsed in `host/`

**Decision.** Profiles are defined in a single JSON document at an absolute
path, loaded by a new host flag:

```
--sandbox-profiles /absolute/path/profiles.json
```

The **parser lives in `host/`** (`host/sandbox_profiles.c`), exactly as
`host/manifest.c` does. The **library** takes an opaque, already-built
`maelys_mcp_sandbox_profiles_t`, constructed through a small builder API in
`include/maelys/mcp/sandbox.h`. No file path, no JSON, and no filesystem read
crosses into `libmaelys_mcp`.

**Why JSON.** The manifest is JSON, jansson is already linked, and — more to the
point — `host/manifest.c` has a validation discipline this format should inherit
rather than reinvent: unknown keys are fatal at every level with an error naming
the key and its location (`docs/manifest.md:180-186`), the walk does not stop at
the first bad entry, and construction begins only after the whole document
validates (`docs/manifest.md:188-203`). A second config format in the same
binary with a *weaker* validation story would be the weak one operators
eventually rely on.

**Why the parser is in `host/` and the profile object is in the library.** The
two halves have different audiences. An embedder linking `libmaelys_mcp` wants
to build a profile set programmatically — it may have no config file at all, and
handing it a filename would force it to write one. The `maelys-mcp` binary wants
a file an operator can edit. Splitting them gives both without the library
growing a config-file concept, which is the same split
`host/manifest.h:11-19` already makes and says it makes: manifest handling
"compile[s] into the `maelys-mcp` binary only … and nothing in `include/`
changes because of this file."

**Why one file and not a directory.** No `profiles.d`, no search path, no
implicit discovery. A confinement policy assembled from whatever files happened
to be present is a policy nobody wrote, and the failure mode — a file added by a
package upgrade widening a profile — is silent by construction. This matches
`docs/security-model.md:5`: only explicitly configured things are used.

**Why absolute.** `docs/security-model.md:6` for provider paths, and
`host/manifest.h:91-96` for the manifest itself. One rule about paths in this
host, not two.

**Sketch of the format.** Illustrative; the normative key table is in
[Profile definitions](#profile-definitions-the-operators-vocabulary).

```json
{
  "sandboxProfileVersion": 1,
  "profiles": {
    "docs-readonly": {
      "read":  ["/usr", "/bin", "/opt/homebrew", "/srv/docs"],
      "write": ["/tmp/maelys-docs"],
      "exec":  ["/srv/docs/bin/docs-provider"],
      "network": "none"
    },
    "build-agent": {
      "read":  ["/usr", "/bin", "/srv/src"],
      "write": ["/srv/build"],
      "network": "inherit",
      "unixSockets": ["/var/run/buildd.sock"]
    }
  }
}
```

### D6 — Profile names reuse the platform-token grammar

**Decision.** A profile name is 1 to 64 bytes of **lowercase ASCII letters,
digits, `-`, `.` and `_`**, and may not begin with `-`. `trusted-local` is
**reserved**: an operator may not define it, and a manifest naming it always
selects the POSIX launcher per [D3](#d3--executionprofile-selects-the-launcher).

**Why that grammar.** It is not invented here. It is the grammar
`include/maelys/mcp/process_launcher.h:243-245` already fixes for the
environment-platform token, and it fixes it for a reason stated at `:245`: so
that the token "is always safe to print verbatim in a diagnostic". A profile
name has exactly the same career — it appears in refusals, in startup errors, in
whatever an operator greps — and the one thing every component will do with an
unrecognized one is print it. Two grammars for two tokens with identical
lifecycles would be a distinction nobody could remember the reason for.

The leading-`-` prohibition is separate and specific: profile names reach a
command line ([D13](#d13---p-never--f-no-temporary-profile-file)), and a token
that can start with `-` is a token that can be read as an option by something
downstream.

**Why `trusted-local` is reserved.** It already means "explicitly no
confinement" in two shipped places (`src/process/posix_launcher.c:139-149`,
`docs/manifest.md:165-176`). An operator who redefined it as a confining profile
would produce a manifest whose meaning depended on whether `--sandbox-profiles`
was passed — the same string selecting confinement or its absence. Refusing the
definition at load time costs nothing and removes the case.

### D7 — `--allow-profile` is the `--allow-effect` symmetry

**Decision.** A new repeatable host flag, and a new manifest top-level key:

```
--allow-profile NAME          # repeatable
"allowProfiles": ["NAME", …]  # manifest v3, top level
```

The two are **OR-ed**, exactly as `--allow-effect` and `allowEffects` are
(`host/main.c:189-191`, `docs/manifest.md:211-213`). A provider whose
`executionProfile` names a profile not in the union **fails host startup**,
before any launcher is asked anything.

**Why a second gate at all**, when the profile has to be defined in the profiles
file anyway. Because defining and authorizing are different acts by potentially
different people. The profiles file is a **vocabulary** — plausibly shipped by a
platform team, a package, or a shared configuration repository, and containing
profiles for providers this host does not run. The manifest is written by
whoever configures a deployment. `--allow-profile` is the *running* operator's
per-invocation consent, and it is the same shape of consent `--allow-effect`
already is: the manifest declares what it wants, the host decides whether this
run grants it (`docs/security-model.md:13-14`).

**Why the check is in the host and not the launcher.** Two different refusals
exist and both must:

- *"You did not authorize this profile for this run."* Host policy. Nothing
  about the launcher enters into it, and it is knowable at startup.
- *"I cannot apply this profile."* Launcher fact. Only the launcher knows, which
  is the argument `docs/launch-contract-design.md:575-584` already makes for
  leaving profile refusal to `spawn`.

Running the host check first means a launcher is never handed a profile the
operator did not authorize — so a bug in a backend's name resolution cannot
become an authorization bypass, because the unauthorized name never reaches it.

**Why OR-ing is safe here.** `--allow-effect` and `allowEffects` are OR-ed
because both *grant* and neither can revoke; the union is the operator's intent
stated in two places. The same holds for profile authorization. It does **not**
hold for `sandboxMode`, and [D8](#d8--sandboxmode-is-disabledrequired-and-nothing-else)
says why the composition rule there points the other way.

### D8 — `sandboxMode` is `disabled`|`required` and nothing else

**Decision.** A new per-provider manifest key, **manifest v3 only**:

```json
"sandboxMode": "disabled"   // the default, and the meaning of absent
"sandboxMode": "required"
```

plus a host flag `--require-sandbox` that raises **every** provider to
`required`. The effective mode is `required` if **either** says so.

**There is no `preferred`.** A mode that means "sandbox if you can, otherwise
run anyway" is silent degradation with a configuration key in front of it, and
it is rejected on exactly the grounds `docs/launch-contract-design.md:567-573`
already gives for the profile refusal: "the deployments most likely to hit it
are exactly the ones that thought they had a sandbox."

**Why the default is `disabled`.** Every existing v1 and v2 manifest must keep
working byte-identically, which is the compatibility rule the manifest has held
across two versions already (`docs/manifest.md:13-16`). `disabled` does not mean
"do not sandbox" — a provider with `sandboxMode: "disabled"` and
`executionProfile: "docs-readonly"` is fully confined. It means "an unconfined
start is not by itself an error for this provider". `required` is the assertion
that it is.

**Why v3 rather than reusing v2.** `docs/manifest.md:84-89` established that a
key belonging to a later version is an *unknown key* in an earlier one, rejected
by name, and gave the reason: a document that names a key its declared version
does not have was written against a different understanding and mislabelled.
Adding `sandboxMode` to v2 would retroactively make that guarantee false for
every v2 document already written. So: `manifestVersion` becomes `1`, `2` **or**
`3`; `sandboxMode` and `allowProfiles` are v3-only; `args` and `executionProfile`
remain v2-and-later; v1 keeps its exact key table.

**Why the flag composes toward the stricter value, when `--allow-effect`
composes toward the more permissive one.** The asymmetry is real and it is not
an inconsistency. Both keys express the same principle — *the manifest and the
flags are two voices of one operator, and neither may weaken what the other
said* — and they point in opposite directions because one names a **permission**
and the other names an **obligation**. The union of permissions is the more
permissive value; the union of obligations is the stricter one. A
`--require-sandbox` that could be relaxed by a manifest, or an
`--allow-effect` that could be revoked by one, would each let the weaker of two
statements win, which is the property neither should have.

### D9 — Every refusal that can happen at startup happens at startup

**Decision.** Checked at host startup, before any provider is spawned:

1. every `executionProfile` named by the manifest is **defined** in the loaded
   profile set;
2. every one is **authorized** ([D7](#d7----allow-profile-is-the---allow-effect-symmetry));
3. every profile in the set is **expressible** by the backend that will apply it
   ([D11](#d11--a-backend-refuses-a-profile-it-cannot-express-in-full));
4. `sandboxMode: "required"` has a sandbox backend to use and a profile to name.

Any failure is fatal to host startup, names the offending provider index and
profile, and starts nothing.

**Why.** `docs/launch-contract-design.md:586-593` names the cost of leaving
profile refusal to `spawn`, and names it as a genuine regression against a
property `docs/manifest.md` is proud of: "a bad profile in `providers[7]` is
reported after `providers[0..6]` have already spawned." That cost was
unavoidable when the host could not know which launcher would be installed. With
in-tree backends the host **does** know — it selected the launcher itself
([D3](#d3--executionprofile-selects-the-launcher)) — so the cost is now
avoidable, and this milestone pays down the debt its predecessor recorded.

The launcher's own spawn-time refusal stays. It is the authoritative one and the
only one an out-of-tree embedder gets. The startup checks are an *earlier* answer
to the same question, not a replacement for it, and where they disagree the
launcher wins.

### D10 — The v1 primitive surface is six primitives

**Decision.** A profile is exactly these six keys, all optional, absent meaning
"deny" for every one of them:

| Key | Type | Meaning |
|---|---|---|
| `read` | array of absolute paths | Readable subtrees |
| `write` | array of absolute paths | Writable subtrees (implies readable) |
| `exec` | array of absolute paths | Executable programs |
| `network` | `"none"` \| `"inherit"` | See [D16](#d16--the-network-is-off-or-inherited-and-nothing-else-in-v1) |
| `unixSockets` | array of absolute paths | Reachable AF_UNIX socket paths |
| `protectFromDelete` | array of absolute paths | Subtrees where unlink and rename are denied even where write is allowed |

**Why deny is the meaning of absent.** Six keys that each default to permitting
would make the empty profile `{}` a synonym for no confinement, which is a name
for "unconfined" that does not look like one. `{}` denies everything, and
[measurement 6](#what-was-measured-and-what-it-changed) says such a profile
cannot even start a dynamically linked program — so the empty profile is
useless, obviously useless, and useless in the safe direction.

**Why `write` implies `read`.** A writable tree the child cannot read is not a
configuration anyone wants, and expressing it would require the operator to list
every tree twice. Stating the implication once here is cheaper than every
profile restating it.

**Why no `denyRead`/`denyWrite` exceptions.** They would make rule order
operator-visible, and rule order is the thing
[measurement 1](#what-was-measured-and-what-it-changed) shows is a foot-gun on
macOS and *has no equivalent at all* on Linux, where bubblewrap composes mounts
rather than evaluating rules. A profile format whose meaning depends on an
ordering semantics only one backend has would be two formats wearing one name.
Deferred, and named in [Not in v1](#not-in-v1).

### D11 — A backend refuses a profile it cannot express in full

**Decision.** At **launcher creation**, each backend validates every profile in
the set against its own expressive power. A primitive it cannot enforce is a
refusal naming the profile, the primitive and the backend — never a weaker
application of it.

In v1 this bites twice, both on Linux:

| Primitive | Seatbelt | bubblewrap |
|---|---|---|
| `exec` | `(allow process-exec* …)` | **not expressible** — refuses |
| `protectFromDelete` | `file-write-unlink` is a distinct operation | **not expressible** — refuses |

**Why.** This is the fail-closed rule the whole seam is built on, applied one
level down. `include/maelys/mcp/process_launcher.h:175-181` requires a launcher
to refuse a profile it cannot apply, "loudly", because "a manifest asking for
confinement must not be answered with an unconfined process and no diagnostic".
A profile is not all-or-nothing: it is six statements, and answering five of them
while silently dropping the sixth is the same failure at higher resolution.
`include/maelys/mcp/process_launcher.h:266-280` makes the identical argument
about environment variables, and makes it at length — *"never silently
translated"*, because a silent translation "starts a child in an environment
nobody wrote and nobody can reproduce".

**Why at creation and not at spawn.** Expressibility is a property of the
profile and the backend, both of which exist at startup. Checking it there
converts "the seventh provider failed to start" into "this host will not start,
because `build-agent` uses `exec` and the bubblewrap backend cannot enforce it" —
see [D9](#d9--every-refusal-that-can-happen-at-startup-happens-at-startup).

**The cost, stated plainly, because it is the largest cost in this document.**
One profiles file is not guaranteed to work on both platforms. An operator with
a mixed fleet who uses `exec` or `protectFromDelete` gets a working macOS host
and a Linux host that refuses to start. That is deliberate and it is still
unpleasant. Two mitigations, no more: the refusal is at startup with a message
naming exactly what to remove, and
[Not in v1](#not-in-v1) names the Linux mechanism (`--seccomp`) that would close
the `exec` gap in a later release. What was **not** chosen is the third option —
letting bubblewrap accept the key and enforce it approximately — because an
approximation of `exec` is "the child can execute whatever is bound into its
filesystem view", which is not a weaker version of the primitive but a different
one.

### D12 — Paths travel as parameters, never as profile text

**Decision.** The generated SBPL contains **no operator-supplied bytes**. Every
path is passed as a `sandbox-exec -D` parameter and referenced from the profile
as `(param "NAME")`; the parameter names are generated (`R0`, `R1`, `W0`, …), so
the profile text is composed entirely of fixed tokens and digits.

```
sandbox-exec -D R0=/usr -D R1=/srv/docs -D W0=/tmp/maelys-docs \
             -p '(version 1)(deny default)…(allow file-read* (subpath (param "R0")) (subpath (param "R1")))…' \
             /srv/docs/bin/docs-provider --provider --root /srv/docs
```

Path **escaping and canonicalization still ship, and ship first**
([S1](#s1--canonicalization-and-escaping)). They are defence in depth and the
answer for any future rule that genuinely cannot take a parameter.

**Why this rather than escaping alone.**
[Measurement 2](#what-was-measured-and-what-it-changed) is a working bypass:
one crafted path, interpolated raw, deleted a `deny` rule and the confined
process read the secret. Escaping fixes it — measurement 2 also shows that — but
it fixes it by making correctness depend on one function being right for every
path any operator ever writes, forever, on a syntax whose escaping rules are not
formally specified anywhere Apple publishes.
[Measurement 3](#what-was-measured-and-what-it-changed) shows the parameter form
accepts the same hostile value with no effect at all. Given a control that
removes the injection *class* and a control that defends against instances of
it, the class is the better control and the instance defence is the better
backup.

**And it fails closed if the generator is wrong.**
[Measurement 4](#what-was-measured-and-what-it-changed): a `(param "X")` with no
matching `-D` is a hard `sandbox-exec` refusal, not an empty pattern. A
generator that emits a rule and forgets its parameter produces a spawn failure —
loud, immediate, and impossible to mistake for a running sandbox. The
interpolating design has no equivalent safety net: a bug there produces a
profile that parses.

**Cost.** Longer argv (one `-D` per path) and profile text that is less pleasant
to read in a debugger than a fully expanded profile. Both are addressed by
[S3](#s3--the-seatbelt-backend)'s requirement that the backend be able to render
the effective profile for diagnostics.

### D13 — `-p`, never `-f`: no temporary profile file

**Decision.** The profile reaches `sandbox-exec` through `-p` as an argv
element. The backend never writes a profile file.

**Why.** `sandbox-exec` accepts `-f profile-file`, `-n profile-name` and
`-p profile-string`. `-f` means creating a file, choosing its directory and
mode, and deleting it — which is a second object with a lifetime, a permission
question, a TOCTOU window between write and exec, and a cleanup path that has to
survive every failure branch and the `release` op's prohibition on blocking
(`include/maelys/mcp/process_launcher.h:399-416`). The profile is already a
string the backend built in memory. `-p` keeps it one.

`-n` is out for a different reason: it names a profile Apple ships, which is
neither operator-defined ([D4](#d4--profiles-are-static-named-and-written-by-the-operator))
nor stable across OS versions.

**The cost.** The profile text is visible in `ps`. A profile is a policy, not a
credential — and `docs/manifest.md:224-239` already prohibits secrets in argv
for the whole system, with an argument that applies unchanged here. Worth
naming; not worth `-f`.

### D14 — Last-match-wins and the base preamble are tested invariants, not comments

**Decision.** Two properties of the Seatbelt backend are pinned by live tests
that fork real children:

1. **Rule order.** `(deny default)` is emitted first and allow rules after it.
   A test generates a deny/allow pair in each order, runs a real confined child
   against each, and asserts the outcomes differ in the direction last-match-wins
   predicts.
2. **The base preamble.** The minimum allowances a dynamically linked binary
   needs to reach `main` on this OS are a fixed prelude, and a test asserts that
   a provider starts under the most restrictive shipped profile.

**Why they are tests and not comments.**
[Measurement 1](#what-was-measured-and-what-it-changed) shows the entire meaning
of a profile inverts if a future contributor sorts the rules, appends a
"belt-and-braces" deny at the end, or merges two profiles by concatenation. A
comment saying "order matters" does not fail a build. Test 1 does, and it fails
with a diagnosis rather than a mystery.

[Measurement 6](#what-was-measured-and-what-it-changed) is the reason for test 2,
and the reason is `sandbox-exec`'s deprecation rather than its design. Apple has
deprecated it, and this project uses it anyway because it is what Chrome, Bazel
and the `sandbox-runtime` project use, which makes it the most-exercised
mechanism available. The exposure that creates is that a macOS update can change
what dyld touches, and a base preamble that no longer suffices produces providers
that will not start — or, far worse, a preamble widened in a hurry to make them
start again. **Pinning the preamble converts an OS change into a red test on a
known commit**, which is a debuggable event, instead of a support ticket about
provider startup.

**On `sandbox-runtime`.** The Apache-2.0 `sandbox-runtime` project is used as
a **behavioural specification** for the macOS half — which operations a working
deny-default profile must permit, and what a realistic preamble contains. It is
not translated line by line, and this design's parameter-based construction
([D12](#d12--paths-travel-as-parameters-never-as-profile-text)) is not its
approach. **If any phase ends up deriving substantially from it, that phase adds
a `NOTICE` file with the Apache-2.0 attribution before it merges**, and says so
in its exit criterion. Judging "substantially" is the implementer's obligation
and the reviewer's check; the default assumption is that a preamble arrived at by
reading someone else's preamble is derived from it.

### D15 — `argv[0]` must equal the executable path, or the backend refuses

**Decision.** Both backends require
`maelys_mcp_process_request_arg_at(request, 0)` to be byte-identical to
`maelys_mcp_process_request_executable(request)`. If it is not, `spawn` fails
with `MAELYS_MCP_ERR_ARGUMENT` and a message naming both strings.

**Why this is forced.** Both wrappers take `PROGRAM ARGS...` on their own
command line and set the child's `argv[0]` from the program path.
[Measurement 5](#what-was-measured-and-what-it-changed)'s harness confirms it
directly: `sandbox-exec -p … ./argv0 hello world` produced
`argv0=[./argv0] argc=3 [hello] [world]`. There is no flag on either wrapper for
"exec this program but present that `argv[0]`". So a request whose `argv[0]`
differs from its executable path **cannot** be honoured through a wrapper.

`include/maelys/mcp/process_launcher.h:158-168` is unambiguous about what the
launcher owes: the complete vector, `argv[0]` included, passed "to the substrate
as given", and "a launcher that reorders, prepends or interprets the vector is
broken". A backend that quietly substituted the path for the requested `argv[0]`
would be interpreting the vector. So it refuses instead — the same answer the
contract requires for any other launch fact a launcher cannot honour.

**How much this actually costs: for native providers, nothing.** The runtime
compiles `argv[0] = path` for every native provider
(`docs/manifest.md:110-114`), so the constraint holds by construction and no
manifest can violate it. It binds only an `mcp-proxy` upstream whose manifest
`argv` sets a first element different from `path` — which is legal today
(`include/maelys/mcp/provider.h:320-321`) and rare. Those upstreams can be
sandboxed by making `argv[0]` the path; the refusal says exactly that.

### D16 — The network is off or inherited, and nothing else in v1

**Decision.** `"network"` takes two values in v1:

- `"none"` — no network access. Seatbelt: `(deny network*)` by way of the
  deny-default base. bubblewrap: `--unshare-net`.
- `"inherit"` — no network restriction is applied.

**Deferred, and named so the omission is visible:**

- **Loopback-only** — the child may reach a proxy on the host's loopback and
  nothing else.
- **Domain allowlists** — the child reaches named hosts through that proxy,
  everything else refused.

**Why they are deferred, which is the interesting part.** Loopback-only is
*easy* on macOS — one `network-outbound` rule with a `remote ip` filter — and
**structurally different on Linux**. `--unshare-net` gives the child its own
network namespace whose loopback is its own, not the host's; a proxy listening
on the host's `127.0.0.1` is not reachable from inside it. Making the two agree
needs a veth pair, a userspace network shim, or a pre-connected socket handed
across the seam — real machinery, with its own failure modes, and in the last
case an extension to the launch contract, which today carries exactly one
descriptor (`docs/manifest.md:247-250`).

So a v1 that shipped `"loopback"` would ship a value meaning "reaches the host's
proxy" on macOS and "reaches nothing" on Linux. **One name, two behaviours,
neither documented as the other** — which is precisely the divergence
[D11](#d11--a-backend-refuses-a-profile-it-cannot-express-in-full) refuses to
allow inside a single profile. Refusing it at the format level is the same
decision applied earlier.

Domain allowlisting depends on the proxy and is therefore downstream of that
question, not a separate one.

**What `"inherit"` is honest about.** It applies no network restriction at all —
not a filtered one, not a narrowed one. It is spelled `inherit` rather than
`allow` for that reason: the child gets the host's network position, whatever
that is.

**One asymmetry worth stating now**, because it will surprise someone.
`"none"` does not block AF_UNIX on either platform — unix sockets are
filesystem objects and are governed by `unixSockets` and by the path primitives.
On Linux, `--unshare-net` *does* cut off **abstract** sockets (the `@`-prefixed
namespace), because those are scoped to the network namespace rather than to the
filesystem. **(unverified)** — pinned by a test in [S4](#s4--the-bubblewrap-backend).

### D17 — The backend is chosen by platform, with no flag to choose it

**Decision.** One backend per platform, selected at compile time with
`#ifdef __APPLE__` / `#else`, which is this repository's only platform-branching
convention (`src/process/launcher.h:52-55`, `src/core/common.c:16`, `:73`,
`:97`; there is no use of `__linux__` anywhere in the tree). Both constructors
are **declared unconditionally** in the public header and **defined
unconditionally**; the one that cannot work on this platform returns
`MAELYS_MCP_ERR_ARGUMENT` with a message naming the platform.

There is **no `--sandbox-backend` flag**.

**Why declared on both platforms.** A public header whose shape changes with the
platform is a header an embedder cannot write portable code against, and it
turns "this backend is unavailable here" from a runtime value into a compile
error in someone else's build. Declaring both and refusing one is the same shape
as the ABI check at `include/maelys/mcp/process_launcher.h:456-467`: the entry
point exists, and it says no with a reason.

**Why `MAELYS_MCP_ERR_ARGUMENT` and not a new code.** `maelys_mcp_result_t`
(`include/maelys/mcp/error.h:7-19`) is a released enum. Adding a member is an ABI
event, and ABI 5 shipped four commits ago; spending an ABI number on a code
whose whole content is "wrong platform" is not a trade this milestone should
make. `ERR_ARGUMENT` with a precise `*out_error` is what the launcher-create ABI
mismatch already does.

**Why no flag.** There is exactly one supported backend per platform, so a flag
could only do two things: select the one that already would be selected, or
select the one that cannot work. Neither is worth a flag, and the second is worth
avoiding. `--sandbox-profiles` implies the platform's backend; that is the whole
selection mechanism.

**Runtime availability is probed at launcher creation** — the wrapper binary
exists and is executable — and **not** functionally self-tested. A self-test
proves the helper ran once, not that it will confine this child, and it costs
every host a subprocess at startup. The real proof is the first spawn, whose
failure is fatal to host startup anyway
([D9](#d9--every-refusal-that-can-happen-at-startup-happens-at-startup)). The
residual risk is named rather than hidden: **`bwrap` present but unusable**
(user namespaces restricted) fails at first spawn rather than at create. It
fails *closed* either way, which is the property that matters, but the operator
sees it one step later than ideal. See [Open questions](#open-questions).

**A platform with no backend.** On any platform where neither backend works,
`--sandbox-profiles` fails at startup. A provider with
`sandboxMode: "required"` therefore cannot start, which is
[D8](#d8--sandboxmode-is-disabledrequired-and-nothing-else) doing its job: the
whole point of `required` is that "no sandbox available" is an error and not a
fallback.

### D18 — Exit-status fidelity is exact on macOS and lossy on Linux

**Decision.** The Seatbelt backend inherits the POSIX launcher's status handling
unchanged. The bubblewrap backend reports what `bwrap` gives it and **says so in
its diagnostics where the wrapper has collapsed a signal death into an exit
code** — it never fabricates a `term_signal`.

**Why macOS is free.** [Measurement 5](#what-was-measured-and-what-it-changed):
`sandbox-exec` execs in place. The pid the parent waits on is the pid the
confined program reports; exit 7 arrives as `WIFEXITED/WEXITSTATUS == 7` and an
internal `SIGKILL` as `WIFSIGNALED/WTERMSIG == 9`. The whole of
`src/process/posix_launcher.c:97-137` — `wait_for_child`, its `WNOHANG` deadline
polling, its `ECHILD` handling — applies unchanged, and so does `posix_stop`
(`:463-489`), because `kill(pid, …)` reaches the real child.

**Why Linux is not. (unverified)** `bwrap` does not exec in place: it sets up
namespaces and remains as a monitor while the payload runs as PID 1 of a new pid
namespace. Two consequences follow, and both must be measured before
[S4](#s4--the-bubblewrap-backend) merges:

1. **Exit status.** A monitor that exits with `128+N` when its child is signalled
   has erased the `exited`/`exit_code`/`term_signal` distinction
   `include/maelys/mcp/process_launcher.h:104-118` requires, whose contract says
   "exactly one of the two fields carries the outcome". `bwrap --json-status-fd`
   is the candidate mechanism for recovering it and S4 evaluates it. If it
   cannot be recovered, the backend reports `exited=1` with the code it has and
   the diagnostic says the signal was collapsed — which is a stated loss, not a
   guess.
2. **The stop ladder.** `SIGTERM` to the monitor must reach the payload for the
   graceful rung to mean anything, and `SIGKILL` to the monitor must tear down
   the whole tree. The second is expected to hold structurally — killing PID 1 of
   a pid namespace ends the namespace — which would make forced stop **stronger**
   containment than the POSIX launcher offers, since it cannot leave a stray
   grandchild. The first is the one at risk, and a graceful rung that silently
   does nothing would turn every teardown into a forced kill after the full grace
   budget.

**Why this is written as a known asymmetry rather than smoothed over.** The
alternative is a backend that synthesizes a plausible `term_signal` from
`128+N`, and `128+N` is also a legal exit code a program can return on purpose.
A launcher that guesses would report a provider that deliberately exited 137 as
having been killed. `include/maelys/mcp/process_launcher.h:266-280` refuses
exactly this class of helpfulness for environment variables, in the strongest
language in the header. The same answer applies.

### D19 — The Seatbelt backend does not merge without a macOS CI job

**Decision.** `.github/workflows/ci.yml` gains a `macos-14` job running
`make check`, and it lands **in the same phase as the Seatbelt backend**, as
part of that phase's exit criterion.

**Why this is a decision and not a chore.** This repository has **never run a
test on macOS.** All five jobs in `.github/workflows/ci.yml` are `ubuntu-24.04`
(`:23`, `:45`, `:55`, `:66`, `:76`). macOS appears once in the whole repository —
`macos-14` in the release matrix (`.github/workflows/release.yml:27-29`) — which
fires only on a `v*` tag push and runs `scripts/package-release.sh`, not `make
check`. It builds a tarball and never executes a test.

Shipping a macOS-only security mechanism into that is not an option worth
weighing. The Seatbelt backend would be the single most platform-specific,
most security-critical, least-covered file in the tree, and the first signal
that an OS update broke it would be an operator's provider failing to start —
or, silently and much worse, a preamble widened until it started again.

The repository already knows this failure shape and wrote it down. The
`check-gcc` job exists because "a GCC-only `-Werror` failure is invisible until
the tag-triggered release build's Linux runners hit it — after the tag is
already pushed. See v0.13.0." A macOS-only sandbox failure has the same
structure and a worse consequence.

**Scope.** The job runs `make check` — the full suite, not a sandbox-only
subset. A macOS runner that only ran sandbox tests would leave every *other*
macOS-specific path (`src/core/common.c:97`'s relative condition wait,
`src/process/launcher.h:52-55`'s `PATH`, the absent `--wrap=pthread_create`)
exactly as unverified as they are today, and this is the one chance to fix that
at no extra cost.

### D20 — "Process provider" and "in-process provider" stop being near-homonyms

**Decision.** Three terms, used consistently, with the collective term for the
sandboxable kinds taken from the sentence that already gets this right.

| Term | Meaning | Sandboxable |
|---|---|---|
| **in-process provider** | Registered through `maelys_mcp_provider_create` or the provider SDK; shares the runtime's address space | **No, structurally** |
| **native child provider** | A `maelys-provider` executable started through the seam | Yes |
| **`mcp-proxy` upstream** | A third-party MCP server started through the seam | Yes |
| **external provider kinds** | The two above, collectively | Yes |

**Why it matters now.** `docs/security-model.md:154-165` makes the central
claim of this whole track — that an in-process provider cannot be confined, "not
a defect to be fixed later; it is what in-process means". That claim is
load-bearing for M3, and it is currently phrased in a vocabulary where
"in-process provider" and "process provider" differ by a hyphen and mean nearly
opposite things with respect to confinement. A reader who mis-parses one of them
concludes the opposite of what the security model says.

The bare phrase appears where it can be misread — `docs/architecture.md:303`,
`:320`, `:327`, `docs/mcp-proxy.md:6`, `:130`,
`docs/authenticated-principal-design.md:504`, `docs/test-parity.md:23`,
`README.md:19`, `:131`, `:136` — and `docs/architecture.md:24` compounds it by
calling the in-process case "in-process C provider" in a diagram whose other
entries are "persistent Python provider" and "persistent TypeScript provider",
three names for two categories.

**The anchor already exists.** `docs/security-model.md:171-173` writes "for both
external provider kinds — native `maelys-provider` children and `mcp-proxy`
upstreams". That is the vocabulary; the cleanup propagates it.

**Scope, deliberately narrow.** Prose in `docs/`, `README.md`, and header
comments where the ambiguity is live. **No identifier is renamed** — not
`process_provider.c`, not `maelys_mcp_provider_process_options_t`, not
`MANIFEST_PROVIDER_NATIVE`. A rename touches the public API, the ABI and every
embedder, to fix a problem that exists in English rather than in C.

## Profile definitions: the operator's vocabulary

The normative format, per [D5](#d5--one-json-file-loaded-by---sandbox-profiles-parsed-in-host)
and [D10](#d10--the-v1-primitive-surface-is-six-primitives).

### Document keys

| Key | Where | Required | Rule |
|---|---|---|---|
| `sandboxProfileVersion` | top level | yes | Exactly `1`. |
| `profiles` | top level | yes | Object. Keys are profile names per [D6](#d6--profile-names-reuse-the-platform-token-grammar); at most 64 entries. |
| `read` | a profile | no | Array of absolute paths. Absent = deny. |
| `write` | a profile | no | Array of absolute paths. Implies `read` on the same paths. |
| `exec` | a profile | no | Array of absolute paths. |
| `network` | a profile | no | `"none"` (the default and the meaning of absent) or `"inherit"`. |
| `unixSockets` | a profile | no | Array of absolute paths to AF_UNIX sockets. |
| `protectFromDelete` | a profile | no | Array of absolute paths. |

**Validation is `host/manifest.c`'s discipline, unchanged**
(`docs/manifest.md:180-203`): unknown keys fatal at every level with an error
naming the key and its location (`profiles["docs-readonly"].reed`), the walk
does not stop at the first bad profile, and construction begins only after the
whole document validates.

**Bounds**, sized the way `docs/manifest.md:139-154` sizes `args` — generous for
any real profile, small enough that a failure stays legible, and rejected at load
with a location-naming error: at most 64 profiles; at most 64 paths per key; at
most 4096 bytes per path.

### Every path is canonicalized at load, and must exist

Each path is required to be absolute, is resolved with `realpath(3)`, and the
**resolved** value is what the profile carries. A path that does not exist is a
load-time error.

**Why existence is required.** `realpath` on a nonexistent path either fails or
requires the loader to canonicalize the deepest existing ancestor and re-attach
the remainder — which is a *guess about which symlinks will be present when the
path is created*. Guessing is how a profile ends up granting a tree the operator
did not mean. Refusing is one message at startup.

**The cost, and why it is small.** An operator who wants a not-yet-existing file
writable names its existing parent directory. Since every path primitive is
subtree-scoped, that is the normal idiom anyway rather than a workaround.

**What canonicalization buys against symlinks.** The profile carries resolved
paths, so a symlink present at load cannot smuggle in a tree under a different
name. It does **not** freeze the filesystem: a path that becomes a symlink after
load is a live question, and the answer is the backend's rather than the
loader's. On macOS the kernel resolves a path before the sandbox check, so a
symlink inside a granted tree pointing outside it does not grant the target — a
property [S3](#s3--the-seatbelt-backend) pins with a test rather than assumes.
On Linux the bind mounts are established once at spawn, so the same question has
a different and simpler answer. Both are tested; neither is asserted.

### The library-side object

```c
typedef struct maelys_mcp_sandbox_profiles maelys_mcp_sandbox_profiles_t;

maelys_mcp_result_t maelys_mcp_sandbox_profiles_create(
    maelys_mcp_sandbox_profiles_t **out_profiles, char **out_error);

maelys_mcp_result_t maelys_mcp_sandbox_profiles_add(
    maelys_mcp_sandbox_profiles_t *profiles, const char *name, char **out_error);

maelys_mcp_result_t maelys_mcp_sandbox_profile_allow_read(
    maelys_mcp_sandbox_profiles_t *profiles, const char *name,
    const char *path, char **out_error);
/* …_allow_write, …_allow_exec, …_allow_unix_socket,
   …_protect_from_delete, …_set_network */

void maelys_mcp_sandbox_profiles_retain(maelys_mcp_sandbox_profiles_t *profiles);
void maelys_mcp_sandbox_profiles_release(maelys_mcp_sandbox_profiles_t *profiles);
```

Opaque and refcounted, for the reason
`include/maelys/mcp/process_launcher.h:420-439` gives for the launcher itself: a
structure an embedder can write is a structure an embedder can corrupt, and a
borrowed pointer with a written lifetime obligation is a use-after-free waiting
for its second caller. Canonicalization happens inside `allow_read` and friends,
so an embedder building profiles programmatically gets the same treatment the
file parser does — one implementation of the rule, not two.

## `executionProfile` × `sandboxMode` × `--allow-profile`

The complete interaction. Inputs:

- **P** — the provider's `executionProfile`: absent, `"trusted-local"`, or a
  name *X*.
- **M** — effective `sandboxMode`: `disabled` (default) or `required`
  (manifest key OR `--require-sandbox`).
- **A** — is *X* in the effective allow set (`--allow-profile` ∪
  `allowProfiles`)?
- **D** — is *X* defined in the loaded profile set?
- **B** — is a sandbox backend available (platform supported, wrapper present,
  profiles loaded)?

Every row is a defined behaviour, and every failure is a **host startup
refusal** naming the provider index and the profile.

| # | P | M | B | D | A | Behaviour |
|---|---|---|---|---|---|---|
| 1 | absent | disabled | — | — | — | POSIX launcher. Unconfined. Today's behaviour, unchanged. |
| 2 | absent | **required** | — | — | — | **Refuse to start.** `required` with no profile named is a contradiction; the host does not resolve it by guessing. |
| 3 | `trusted-local` | disabled | — | — | — | POSIX launcher. Unconfined, explicitly. Today's behaviour, unchanged. |
| 4 | `trusted-local` | **required** | — | — | — | **Refuse to start.** `trusted-local` *is* the spelling of no confinement; `required` denies it. |
| 5 | *X* | either | no | — | — | **Refuse to start.** No backend. `disabled` does not rescue it: `disabled` permits an unconfined start only where none was requested, and *X* requests one. |
| 6 | *X* | either | yes | no | — | **Refuse to start.** *X* is not defined in the profile set. |
| 7 | *X* | either | yes | yes | no | **Refuse to start.** *X* is not authorized: pass `--allow-profile X`. |
| 8 | *X* | either | yes | yes | yes | Sandbox backend bound for this provider. Confined per *X*. |
| 9 | *X* | either | yes | yes | yes | …**unless** *X* uses a primitive the backend cannot express: **refuse to start**, naming primitive and backend ([D11](#d11--a-backend-refuses-a-profile-it-cannot-express-in-full)). |

Three properties are worth reading off the table.

**`sandboxMode` never *causes* confinement; it only *forbids its absence*.**
Rows 5–9 are identical under `disabled` and `required`, because a named profile
is already a request for confinement and is already fail-closed. `sandboxMode`
changes exactly two rows — 2 and 4 — the ones where nothing was requested. That
is the whole of its job, and it is why there is no third value: `preferred`
would have to *soften* rows 5 through 7, and softening them is the silent
degradation this design refuses.

**Row 5 is the row that makes the flag worth having.** A manifest that names a
profile on a platform with no backend does not fall back. It refuses. The
deployment that most needs this is the one that copied a working macOS manifest
to a Linux host.

**There is no row where a provider starts less confined than its
`executionProfile` asked for.** Every disagreement between what was asked and
what can be delivered is a refusal.

## The v1 primitive surface

How the six primitives of
[D10](#d10--the-v1-primitive-surface-is-six-primitives) reach each backend.

| Primitive | Seatbelt (SBPL) | bubblewrap (argv) |
|---|---|---|
| `read: [P]` | `(allow file-read* (subpath (param "Rn")))` | `--ro-bind P P` |
| `write: [P]` | `(allow file-read* file-write* (subpath (param "Wn")))` | `--bind P P` |
| `exec: [P]` | `(allow process-exec* (subpath (param "Xn")))` | **refuses the profile** ([D11](#d11--a-backend-refuses-a-profile-it-cannot-express-in-full)) |
| `network: "none"` | deny-default base; no `network*` allow emitted | `--unshare-net` |
| `network: "inherit"` | `(allow network*)` | no network flag |
| `unixSockets: [S]` | `(allow network-outbound (literal (param "Un")))` | `--bind S S` |
| `protectFromDelete: [P]` | `(deny file-write-unlink (subpath (param "Dn")))`, emitted after the write allows | **refuses the profile** |

Two structural remarks.

**The two backends enforce by different means, not by a shared abstraction.**
Seatbelt evaluates ordered rules against a filesystem the child can otherwise
see; bubblewrap composes a filesystem the child cannot see past. A path absent
from a bwrap profile does not exist for that child; a path absent from an SBPL
profile exists and is denied. The observable difference is the error the child
gets — `ENOENT` versus `EPERM` — and any test asserting a specific errno is
asserting a backend detail rather than a profile property.

**`protectFromDelete` is where rule order becomes operator-visible on macOS**,
and it is emitted *after* the write allows precisely because of
[measurement 1](#what-was-measured-and-what-it-changed). Emitting it before them
would make it a no-op that reads correctly. This is the concrete case
[D14](#d14--last-match-wins-and-the-base-preamble-are-tested-invariants-not-comments)'s
first test exists to protect, and it is also the clearest illustration of why
the primitive is not expressible on bubblewrap: a bind-mounted writable
directory permits `unlink` and there is no mount option that separates the two.

## The Seatbelt backend

`src/process/seatbelt_launcher.c`. Structurally the POSIX launcher with a
different argv — which is a claim
[measurement 5](#what-was-measured-and-what-it-changed) earns rather than a hope.

**What is reused verbatim** from `src/process/posix_launcher.c`: the socketpair
and its `FD_CLOEXEC` handling (`:283-299`), both descriptor layouts including the
`ISOLATED` ordering hazard the comments at `:320-356` explain, the environment
vector built in the parent before `fork` (`:203-232`, and `:198-202` for why it
must be), `SO_NOSIGPIPE` (`:379-403`), `wait_for_child`'s deadline polling
(`:97-137`), `posix_stop` (`:463-489`) and `posix_release` (`:494-497`). The
duplication this implies is real and [S3](#s3--the-seatbelt-backend) resolves it
by extraction into a shared internal unit, not by copy.

**What changes** is the vector handed to `execve`:

```
/usr/bin/sandbox-exec
  -D R0=<path> -D R1=<path> …            one per path in the profile
  -p <generated profile text>            no operator bytes; parameters only
  <executable>                           == request argv[0], per D15
  <request argv[1..]>                    verbatim
```

**The generated profile has a fixed shape**, and the shape is the invariant:

```
(version 1)
(deny default)
<base preamble>                      dyld, the shared cache, sysctl, mach-lookup
(allow process-exec* (literal (param "SELF")))
<read allows>  <write allows>  <exec allows>  <unix-socket allows>
<protectFromDelete denies>           LAST, per measurement 1
```

`(deny default)` first, denies last, allows between: reading the generator's
output top to bottom reads the profile in evaluation order, so a reviewer
checking rule order is checking the thing that actually matters.

**Everything an operator supplied is a `-D` value**
([D12](#d12--paths-travel-as-parameters-never-as-profile-text)). The profile text
is fixed tokens and generated parameter names. Even a path containing `")`,
newlines, or arbitrary UTF-8 changes nothing about the profile's structure —
[measurement 3](#what-was-measured-and-what-it-changed) — and a generator bug
that emits a rule without its parameter is a hard spawn failure rather than a
silently weaker sandbox —
[measurement 4](#what-was-measured-and-what-it-changed).

**Escaping ships anyway**, in [S1](#s1--canonicalization-and-escaping), and is
applied to every value that reaches the profile text through any future path that
cannot take a parameter. It escapes `\` to `\\` and `"` to `\"`, refuses any byte
below `0x20` or equal to `0x7F`, refuses embedded NUL — which is
`docs/security-model.md:73`'s existing rule for protocol fields entering
length-unaware C APIs, applied here — and passes valid UTF-8 through unchanged,
because a path with an accent or a space is an ordinary path and refusing it
would be a bug reported as a security feature.

**Only `literal` and `subpath` predicates are generated. Never `regex`.** A
`regex` predicate would need a second escaping layer with different rules over
the same operator input, and two escaping functions is one more than can be kept
correct.

## The bubblewrap backend

`src/process/bubblewrap_launcher.c`. **Every behavioural claim in this section is
unverified** — see
[What was measured](#what-was-measured-and-what-it-changed) — and
[S4](#s4--the-bubblewrap-backend) begins by measuring them.

**The vector:**

```
/usr/bin/bwrap
  --die-with-parent
  --unshare-user --unshare-ipc --unshare-pid --unshare-uts --unshare-cgroup
  [--unshare-net]                        when network is "none"
  --proc /proc  --dev /dev
  --ro-bind <path> <path>  …             read
  --bind <path> <path>     …             write, unixSockets
  --
  <executable>                           == request argv[0], per D15
  <request argv[1..]>
```

**Paths are separate argv elements, so there is no injection surface** — the
structural property [D12](#d12--paths-travel-as-parameters-never-as-profile-text)
buys on macOS by other means. `execve` carries each path as its own NUL-terminated
element with no parser between the backend and `bwrap`, so no quoting exists to
get wrong. This is also why
`scripts/audit_boundaries.sh:20-23`'s prohibition on `system`, `popen` and
`sh -c` is not merely a layering rule here: shelling out would *create* the
quoting problem that building the vector directly does not have.

**The environment.** `bwrap` inherits its own environment and passes it on, and
the backend `execve`s `bwrap` with the request's environment vector, so the
child receives what the request specified. `--clearenv` is therefore *not*
passed. **(unverified)** — pinned by adapting the `environment-provider` fixture
(`tests/helpers/adversarial_provider.c:300-320`), which already exits 4 if its
`PATH` is not exactly `MAELYS_MCP_PROCESS_CHILD_PATH`
(`src/process/launcher.h:52-55`).

**Descriptors.** The protocol descriptor must survive to the child at fd 0/1
(`STDIO`) or fd 3 (`ISOLATED`). `bwrap` is not known to close inherited
descriptors, but "not known to" is not a guarantee. **(unverified, and the single
highest-risk unknown in this backend.)** The `fd-check-provider` fixture
(`tests/helpers/adversarial_provider.c:269-279`) already walks fds 3..255 looking
for sockets and exits 6 if it finds one, and it is the right instrument pointed
the other way: S4 pins both that the protocol fd *is* present where the layout
says and that nothing else is.

**Signals and exit status** are the two open questions of
[D18](#d18--exit-status-fidelity-is-exact-on-macos-and-lossy-on-linux), and S4
cannot merge without answering both.

**`--die-with-parent` is load-bearing.** It is what prevents a confined tree
outliving the runtime that started it. `include/maelys/mcp/process_launcher.h:372-377`
calls an unterminated child a *containment failure* and requires it to be
reported rather than swallowed; `--die-with-parent` makes that outcome rarer on
Linux than the POSIX launcher can make it.

## The network is not a boolean

The v1 answer is [D16](#d16--the-network-is-off-or-inherited-and-nothing-else-in-v1),
and the section exists to make the *shape* of the deferral legible rather than to
restate it.

Network confinement has at least four rungs, and only two are in v1:

| Rung | v1? | Why |
|---|---|---|
| Deny everything | **yes** | Expressible identically on both backends. The rung most providers want. |
| Inherit the host's position | **yes** | The honest name for "no restriction". |
| Loopback-only, to a proxy on the host | no | The two backends cannot mean the same thing by it — [D16](#d16--the-network-is-off-or-inherited-and-nothing-else-in-v1) |
| Domain allowlist through that proxy | no | Downstream of the rung above |

The deferred rungs are named in [Not in v1](#not-in-v1) with what each needs,
because a deferral that is not written down becomes an assumption that the
feature exists.

## Backend selection, and a platform with no backend

Restating [D17](#d17--the-backend-is-chosen-by-platform-with-no-flag-to-choose-it)
as the sequence the host actually performs, since it spans three decisions:

1. **Compile time.** Both backend files are in `LIB_SOURCES` (`Makefile:45-67`,
   beside `src/process/posix_launcher.c` at `:64`) on every platform. Each body
   is `#ifdef __APPLE__` / `#else` per the repository's only convention.
2. **`--sandbox-profiles` given?** No: no sandbox backend is created; every
   provider gets the POSIX launcher; rows 5–9 of the truth table cannot arise
   because no profile is defined; a `required` provider fails row 2 or 4.
3. **Yes.** The file is loaded and validated. The platform's backend constructor
   runs, probes its wrapper for existence and executability, and validates every
   profile for expressibility ([D11](#d11--a-backend-refuses-a-profile-it-cannot-express-in-full)).
   Any failure is fatal to startup.
4. **Per provider**, the launcher is chosen by `executionProfile`
   ([D3](#d3--executionprofile-selects-the-launcher)) after the truth table's
   startup checks pass.

A host therefore holds at most two launchers, each refcounted per provider
(`include/maelys/mcp/process_launcher.h:426-439`), and releases its own
references immediately after spawning — which the header states is correct and
ordering-independent (`:432-439`).

## Test strategy

Five groups. Groups 1 and 2 are pure and run everywhere; 3, 4 and 5 need the
platform they test.

The repository's conventions are followed rather than extended: a test is a
hand-written link rule plus an entry in `TEST_ARTIFACTS` (`Makefile:236`) plus an
invocation line in the `test:` target; `tests/test_support.h` supplies
`ASSERT_TRUE` and `maelys_run_tests`; and process-spawning suites follow the
convention `tests/test_process_launcher.c` and `tests/test_process_provider.c`
use — a standalone `main` taking fixture paths as arguments.

**Tests never fork directly.** They spawn children only through library entry
points, which reach the one fork site in `src/process/`. This is the arrangement
`tests/test_process_launcher.c:16-22` describes and
`scripts/audit_boundaries.sh:52-56` enforces for that file specifically, and the
new suites keep it: a test that forked its own child would be testing something
other than the launcher.

### 1. Injection (`tests/test_sandbox_path.c`, no processes)

The corpus, each asserted against both the canonicalizer and the escaper:

- `")` — the payload from
  [measurement 2](#what-was-measured-and-what-it-changed), and specifically the
  full working bypass, verbatim, as a fixture path;
- a bare `"`; a trailing `\`; `\"`; `\\"`;
- newline, tab, and a byte below `0x20` — **refused**, not escaped;
- an embedded NUL delivered through jansson as a `\u0000` escape — refused,
  per `docs/security-model.md:73`;
- non-ASCII: accented Latin, CJK, an emoji, a combining sequence — **passed
  through unchanged**, because these are ordinary paths;
- relative paths, `..` segments, a trailing `/`, a doubled `//` — normalized or
  refused, each asserted;
- a symlink into a granted tree and a symlink out of one — the resolved value
  asserted;
- a path exceeding the 4096-byte bound.

**A fuzz target joins `fuzz-smoke`** (`Makefile:475`), beside the six that exist:
arbitrary bytes into the canonicalizer and escaper, with the invariant that the
escaper's output, re-parsed, contains no unbalanced quote and no parenthesis the
input did not have.

### 2. Profile validation (`tests/test_sandbox_profile.c`, no processes)

Every truth-table row that can be evaluated without spawning; the name grammar,
including the reserved `trusted-local` and a leading `-`; unknown keys at every
level with the location string asserted; the bounds; the per-backend
expressibility refusal, with the message asserted to name **both** the primitive
and the backend.

### 3. SBPL generation (macOS, no processes)

The generated text is asserted directly: `(deny default)` is the first rule after
`(version 1)`; `protectFromDelete` denies come last; **the text contains no byte
from any configured path** — the mechanical statement of
[D12](#d12--paths-travel-as-parameters-never-as-profile-text), and the assertion
that fails the day someone "simplifies" a parameter into an interpolation; every
`(param "N")` has a matching `-D N=`.

### 4. Escape tests — live children that try to get out (macOS and Linux)

The instrument is a **new persona in the existing fixture**. The whole
adversarial fixture is one binary symlinked under many names, dispatching on
`argv[0]` (`tests/helpers/adversarial_provider.c:257-269`), and the
`environment-provider` persona (`:300-320`) is already the exact shape needed: it
checks a fact about its own execution context and exits with a distinct code when
the fact is wrong. `sandbox-probe-provider` does the same for reachability —
attempts a read, a write, an unlink, a connect, and reports each outcome — with
distinct exit codes chosen the way `exit-seven-provider`'s 7 was chosen
(`:283-291`): distinguishable from success, from `/usr/bin/false`, and from the
126 and 127 the launcher's own child branch uses.

The cases, each spawned under a real backend with a real profile:

1. A file outside `read` **cannot** be read.
2. A file inside `read` can be.
3. A file inside `read` but outside `write` cannot be written.
4. A file inside `write` can be.
5. `network: "none"` — an outbound connect fails.
6. A unix socket outside `unixSockets` cannot be connected to; one inside can.
7. `protectFromDelete` — a file inside a writable tree can be modified and
   **cannot** be unlinked or renamed (macOS only, per
   [D11](#d11--a-backend-refuses-a-profile-it-cannot-express-in-full)).
8. A symlink inside a granted tree pointing outside it does **not** grant the
   target.
9. **A path containing the [measurement 2](#what-was-measured-and-what-it-changed)
   payload, granted for read, does not widen anything else.** This is group 1's
   assertion made behaviourally: the same bypass, through the real backend,
   with a real child, ending in a denial.

**Case 9 is the one worth insisting on**, in the sense
`docs/launch-contract-design.md:998-1002` means: every other case tests a rule,
and case 9 tests that the rules cannot be rewritten by their own arguments.

### 5. Seam conformance under the backends

The thirteen cases of `docs/launch-contract-design.md:947-1002` are the seam's
conformance suite. The subset expressible with a real child runs against each
backend, in addition to the fake and POSIX launchers it runs against today:
exit-code preservation (`exit-seven-provider` must still report 7 through the
sandbox), signal preservation, the graceful and forced rungs, containment
failure, `release` exactly once, and the fd-layout assertions.

This group is where
[D18](#d18--exit-status-fidelity-is-exact-on-macos-and-lossy-on-linux)'s Linux
questions get their answers, and it is why they are answered by a test rather
than by a paragraph.

## CI, and the fact that this repository has never run a test on macOS

The current state, read from the workflows:

| Workflow | Jobs | Runners | Runs tests? |
|---|---|---|---|
| `ci.yml` | `check`, `check-gcc`, `sanitizers`, `fuzz`, `official-conformance` | `ubuntu-24.04` ×5 (`:23`, `:45`, `:55`, `:66`, `:76`) | Yes |
| `mutation.yml` | `sweep` (weekly cron, not required) | `ubuntu-24.04` | Mutation sweep |
| `release.yml` | `build` matrix, `publish` | `ubuntu-24.04`, `ubuntu-24.04-arm`, `macos-14` (`:20-30`) | **No** — packages tarballs |

**macOS.** [D19](#d19--the-seatbelt-backend-does-not-merge-without-a-macos-ci-job)
adds a `macos-14` job to `ci.yml` running `make check`, in
[S3](#s3--the-seatbelt-backend). Dependencies come from Homebrew, following the
pattern `release.yml` already uses for that runner (`brew install jansson
uriparser`, `PKG_CONFIG_PATH` from `brew --prefix`), so this is an established
recipe rather than a new one. `/usr/bin/sandbox-exec` is part of the OS and
needs no install — confirmed present on macOS 15.5 as a root-owned system
binary, and expected on `macos-14` for the same reason, though the job's first
run is the real confirmation.

**Linux.** `bwrap` is **not** in the runner's default package set and must be
installed (`sudo apt-get install --yes bubblewrap`), joining the `apt-get` line
every `ci.yml` job already has. Beyond installation there is a second question
with no reliable answer from here: **Ubuntu 24.04 restricts unprivileged user
namespaces via AppArmor**, which is exactly what `bwrap --unshare-user` needs.
Whether the GitHub `ubuntu-24.04` image relaxes that is **unverified**, and it is
[S4](#s4--the-bubblewrap-backend)'s first task — before any backend code — to
find out, because the answer changes the phase. If user namespaces are
unavailable, the options are, in preference order: a setuid `bwrap`; relaxing
the sysctl in the job; or running the Linux escape tests in the existing
digest-pinned Docker image (`tools/docker/asan-linux/Dockerfile`) that
`make test-asan-linux` already uses locally. **What is not an option is skipping
them**, for the same reason [D19](#d19--the-seatbelt-backend-does-not-merge-without-a-macos-ci-job)
gives.

**Skipping is not silence.** Where a backend's tests cannot run — the Seatbelt
suite on a Linux runner — the suite **reports that it skipped and why**, in the
`ok`/`not ok` stream `tests/test_support.h:20-29` already writes. A suite that
passes by doing nothing is indistinguishable from a suite that passes, and this
is the milestone where that distinction is worth a line of output.

## Phases

Each phase is self-contained: files touched, tests, exit criterion. **The phase
sections are the only authority on scope.** No phase merges with `make check`
red, and no phase depends on a later one.

Canonicalization and escaping are first, before any launcher, because
[measurement 2](#what-was-measured-and-what-it-changed) is a working bypass and
everything after it is built on top of the code that prevents it.

### S1 — Canonicalization and escaping

**Files.** `src/process/sandbox_path.c`, `src/process/sandbox_path.h` (internal),
`tests/test_sandbox_path.c`, `fuzz/fuzz_sandbox_path.c`. `Makefile`: add the
source to `LIB_SOURCES` (`:45-67`), the test to `TEST_ARTIFACTS` (`:236`) and to
`test:`, and the fuzz target beside the existing six (`:422-475`).

**What.** Canonicalize an operator path: require absolute, `realpath`, require
existence, enforce the 4096-byte bound, reject control bytes and embedded NUL.
Escape a string for an SBPL literal: `\` → `\\`, `"` → `\"`, refuse control
bytes, pass valid UTF-8 through. No launcher, no profile, no process — this
phase produces two functions and their tests.

**Tests.** Group 1 in full, plus the fuzz target in `fuzz-smoke`.

**Exit criterion.** The full injection corpus passes, **including the exact
payload from [measurement 2](#what-was-measured-and-what-it-changed) carried as a
fixture**. `make check`, `make asan` and `make tsan` green. `fuzz-smoke` includes
the new target. No file outside the four listed is modified.

### S2 — The profile set

**Files.** `include/maelys/mcp/sandbox.h` (new public header),
`src/process/sandbox_profile.c`, `tests/test_sandbox_profile.c`, `Makefile`.

**What.** The opaque refcounted `maelys_mcp_sandbox_profiles_t`, its builder, the
name grammar, the bounds, and the per-backend expressibility query the backends
will call at creation. Paths go through S1's canonicalizer on the way in. No
backend, no host wiring, no SBPL, no bwrap.

**ABI note.** New header, new entry points, no released layout changed —
`docs/abi-policy.md`'s compatible-extension idiom.
**`MAELYS_MCP_ABI_VERSION` does not move**, and the phase asserts it: bumping it
would announce a break that did not happen, which is the correction
`docs/launch-contract-design.md:30-35` records having had to make once already.

**Tests.** Group 2 in full.

**Exit criterion.** `make check`, `asan`, `tsan` green.
`scripts/audit_boundaries.sh` passes — in particular the new public header
includes no private header (`:85-88`). The version-header check
(`check-version-header`) confirms the ABI constant is unchanged.

### S3 — The Seatbelt backend

**Files.** `src/process/seatbelt_launcher.c`, the shared-substrate extraction out
of `src/process/posix_launcher.c` (socketpair, fd layouts, environment vector,
child-wait polling, stop, release) into `src/process/posix_substrate.c`/`.h`,
`include/maelys/mcp/sandbox.h` (constructor declaration),
`tests/test_sandbox_seatbelt.c`, `tests/helpers/adversarial_provider.c` (the
`sandbox-probe` persona), `Makefile`, `.github/workflows/ci.yml`.

**What.** The generator ([D12](#d12--paths-travel-as-parameters-never-as-profile-text),
[D14](#d14--last-match-wins-and-the-base-preamble-are-tested-invariants-not-comments)),
the argv construction ([D13](#d13---p-never--f-no-temporary-profile-file),
[D15](#d15--argv0-must-equal-the-executable-path-or-the-backend-refuses)), the
`ops` table, `maelys_mcp_seatbelt_launcher_create`, and the base preamble.

**The extraction is part of this phase and not a follow-up.** Copying
`posix_launcher.c`'s descriptor handling would duplicate the `ISOLATED` ordering
hazard its comments at `:320-356` exist to explain, and a duplicated hazard is
one that gets fixed once.

**Tests.** Groups 3 and 4 (macOS), group 5's expressible subset. The existing
`tests/test_process_launcher.c:1454-1459` — the POSIX launcher refusing
`"seatbelt-readonly"` — **must still pass unmodified**; it is the assertion that
[D3](#d3--executionprofile-selects-the-launcher) did not change what the POSIX
launcher does.

**Exit criterion.** `ci.yml` has a `macos-14` job running `make check`, and it is
green. Every escape case denies. The generated-text assertion that **no
configured path byte appears in the profile** passes. `make asan` and `make tsan`
green on both runners. If the base preamble derived substantially from
`sandbox-runtime`, a `NOTICE` with the Apache-2.0 attribution is in the same PR
([D14](#d14--last-match-wins-and-the-base-preamble-are-tested-invariants-not-comments)).

### S4 — The bubblewrap backend

**Files.** `src/process/bubblewrap_launcher.c`,
`include/maelys/mcp/sandbox.h`, `tests/test_sandbox_bubblewrap.c`, `Makefile`,
`.github/workflows/ci.yml`.

**Task zero, before any backend code**, because the answers change the phase:

1. Are unprivileged user namespaces available on the `ubuntu-24.04` runner?
2. Does `bwrap` preserve the child's exit code, and does it distinguish a
   signalled child? Does `--json-status-fd` recover the distinction?
3. Does `SIGTERM` to `bwrap` reach the payload? Does `SIGKILL` tear down the
   tree?
4. Do inherited descriptors survive to the payload at fd 0/1 and fd 3?
5. Does the payload receive the environment the backend `execve`d `bwrap` with?

Each answer is recorded in this document as an **As built** note, whether or not
it matched.

**What.** The argv construction, the `ops` table,
`maelys_mcp_bubblewrap_launcher_create`, the expressibility refusals for `exec`
and `protectFromDelete`, and whatever question 2's answer requires for exit
status ([D18](#d18--exit-status-fidelity-is-exact-on-macos-and-lossy-on-linux)).

**Tests.** Group 4 (Linux), group 5's expressible subset, plus a test per
question above, including the abstract-socket assertion from
[D16](#d16--the-network-is-off-or-inherited-and-nothing-else-in-v1) and the
`fd-check`-derived descriptor assertion.

**Exit criterion.** Every escape case denies on `ubuntu-24.04` in CI, not only
locally. Exit-code preservation is asserted (`exit-seven-provider` reports 7
through the sandbox) and the signal question is **answered in a test** — either
`term_signal` is exact, or the collapse is asserted along with the diagnostic
that reports it. The `exec` and `protectFromDelete` refusals name both the
primitive and the backend. `make check`, `asan`, `tsan` green.

### S5 — Host wiring and manifest v3

**Files.** `host/sandbox_profiles.c`, `host/sandbox_profiles.h`, `host/main.c`
(three flags, launcher selection, the startup checks), `host/manifest.c`,
`host/manifest.h` (v3 key tables, `sandboxMode`, `allowProfiles`),
`tests/test_manifest.c`, `tests/test_sandbox_host.c`, `Makefile`,
`scripts/test_stdio.sh`.

**What.** `--sandbox-profiles`, `--allow-profile`, `--require-sandbox`, all three
in the `usage()` string (`host/main.c:39-64`), which
`scripts/test_stdio.sh:9-10` already pins for `--allow-effect`. Manifest
v3: `manifestVersion` accepts `1`, `2` or `3`; `sandboxMode` per provider;
`allowProfiles` at top level; both are unknown keys under v1 and v2
([D8](#d8--sandboxmode-is-disabledrequired-and-nothing-else)). The nine truth-table
rows. Launcher selection per provider
([D3](#d3--executionprofile-selects-the-launcher)).

**The parser follows `host/manifest.c` exactly**: version-selected key tables
(`:63-77`), unknown keys fatal with a location string (`:126`), full-document
validation before construction.

**Tests.** Every truth-table row, each asserting the exit status and the message.
Every existing v1 and v2 fixture passes **unmodified** — the compatibility
guarantee `docs/manifest.md:13-16` has held twice and holds a third time. `v2`
documents using `sandboxMode` are rejected **by name**. `--allow-profile` and
`allowProfiles` OR-ing, mirroring the `allowEffects` test.
`scripts/test_stdio.sh` covers a bad `--allow-profile` value the way it covers a
bad `--allow-effect`.

**Exit criterion.** All nine rows tested and green. Every pre-existing manifest
fixture unmodified. `make check`, `asan`, `tsan` green on both runners. An
end-to-end run: the real `maelys-mcp` binary, a v3 manifest, a profiles file, a
confined provider that answers `tools/call` and cannot read a file outside its
profile.

### S6 — Documentation and the naming cleanup

**Files.** `docs/security-model.md`, `docs/manifest.md`,
`docs/sandbox-backends-design.md` (the **As built** notes), `README.md`,
`docs/architecture.md`, `docs/mcp-proxy.md`,
`docs/authenticated-principal-design.md`, `docs/test-parity.md`.

**What.** `docs/security-model.md`'s trust-boundary section stops saying "A later
sandbox adapter may use OS facilities or containers" (`:145-146`) and says what
shipped; `docs/manifest.md` documents v3; `README.md` documents the three flags;
[D20](#d20--process-provider-and-in-process-provider-stop-being-near-homonyms)'s
vocabulary is applied at the sites listed there. **No identifier is renamed.**

**Not in scope: `CHANGELOG.md`.** The changelog is written centrally at release
time and no phase of this milestone touches it.

**Tests.** `make check` — the version-header check and the boundary audit both
run under it.

**Exit criterion.** No occurrence of the bare phrase "process provider" survives
where an in-process provider could be meant. Every **As built** note from S1–S5
is recorded, including the ones where reality differed — this document keeps its
predictions and annotates them rather than editing them into descriptions, which
is the discipline `docs/http-transport-design.md:9-13` states and the reason
either document is evidence of anything.

## Not in v1

- **Computed profiles.** The Datalog PDP, and any profile that is a function of
  the caller, the request or repository state. Private, by construction —
  [the line this document does not cross](#the-line-this-document-does-not-cross).
- **OCI, Firecracker, Apple Container, remote `executord`.** Private. The E-track
  reaches them at E7 (`docs/launch-contract-design.md:1745`) without either side
  moving again, which is the property the opaque request was chosen for.
- **Loopback-only networking and domain allowlists.** [D16](#d16--the-network-is-off-or-inherited-and-nothing-else-in-v1).
  Needs a network shim or a pre-connected descriptor on Linux before it can mean
  one thing on both platforms.
- **`exec` on Linux.** Needs a seccomp filter and therefore a libseccomp
  dependency and a BPF program to review. Named here because
  [D11](#d11--a-backend-refuses-a-profile-it-cannot-express-in-full)'s refusal is
  the v1 answer and should not be mistaken for a permanent one.
- **`protectFromDelete` on Linux.** No bind-mount option separates write from
  unlink. A follow-up would need overlayfs or a seccomp filter.
- **Per-rule deny exceptions** (`denyRead` inside a granted `read`).
  [D10](#d10--the-v1-primitive-surface-is-six-primitives): it would export
  macOS's rule-ordering semantics into a format bubblewrap has no equivalent for.
- **Resource limits** — CPU, memory, pids, wall clock. Neither backend's natural
  mechanism (`RLIMIT_*`, cgroups) is a profile primitive, and cgroup delegation is
  a deployment question rather than a launcher one.
- **Profile inheritance, includes, or composition.** A vocabulary, not a
  language — [D4](#d4--profiles-are-static-named-and-written-by-the-operator).
- **Hot reload of the profiles file.** Read once at startup, matching
  `docs/manifest.md:258-259` for the manifest.
- **Sandboxing an in-process provider.** Impossible, not deferred —
  `docs/security-model.md:154-171`.
- **Windows.** The runtime is POSIX-only (`README.md:64`).

## Open questions

Six. The first four are questions of fact that
[S4](#s4--the-bubblewrap-backend)'s task zero answers by measurement; the last
two need a decision this document does not have the standing to take alone.

**1. Does `bwrap` preserve the signal/exit-code distinction?** If
`--json-status-fd` does not recover it, the bubblewrap backend reports a
collapsed status and says so. That is a stated fidelity loss against
`include/maelys/mcp/process_launcher.h:104-118`, and it is worth knowing before
S4 rather than after.

**2. Does `SIGTERM` to `bwrap` reach the payload?** If not, the graceful rung is
a no-op on Linux and every teardown pays the full grace budget before the forced
rung does the work. Mechanism and mitigation are S4's; the risk is named now.

**3. Are unprivileged user namespaces available on the GitHub `ubuntu-24.04`
runner?** Ubuntu 24.04 restricts them via AppArmor by default. If the runner does
not relax it, S4's CI story changes to a setuid `bwrap`, a sysctl in the job, or
the existing digest-pinned Docker image. Not skipping.

**4. Do inherited descriptors survive `bwrap` to the payload?** The highest-risk
unknown in the Linux backend: if fd 3 does not survive, the `ISOLATED` layout
cannot be implemented through `bwrap` as specified and the backend must either
refuse that layout or find another route.

**5. Should `--sandbox-profiles` be repeatable?** This document says one file
([D5](#d5--one-json-file-loaded-by---sandbox-profiles-parsed-in-host)), and a
plausible deployment has a platform team's shared vocabulary plus a
deployment-local addition. Allowing two files raises a question this document
deliberately has no answer to — what happens when both define the same name —
and every answer to it (last wins, first wins, error) is a *composition rule*,
which is the first step toward the policy language
[D4](#d4--profiles-are-static-named-and-written-by-the-operator) declines to
build. Refusing duplicate names across files would be defensible. **Not decided
here.**

**6. Should a profile be able to name the fd layout?** The `ISOLATED` layout is
derived from the child's protocol type and is deliberately not configurable
(`docs/manifest.md:263-267`, "No key is planned for it, in any version").
Confinement does not change that reasoning, and this document proposes no
change — but a sandbox profile is the first configuration object that plausibly
*wants* to, and the question deserves an explicit "still no" from someone with
the standing to give it rather than an omission.
