# Mutation triage

A survivor is a finding, not a bug and not a task. The failure mode this file
exists to end is the third option: a survivor nobody killed and nobody wrote
down, which next week's sweep reports again and next month's reader assumes
somebody already looked at.

So every survivor ends up in exactly one of three places. It is killed by a
test; it is recorded here with the reason it was not; or it is argued here to
be an *equivalent mutant*, which is a claim about the program rather than
about the tests and therefore has to carry its argument with it.

The tables below cover the first real sweep — `src/core/nested.c` and
`src/core/middleware.c`, whose 128 survivors were published as findings in #38.

## Reading a `condition_kill` reason

`condition_kill` rewrites `if (c) stmt` as `if (0 && (c)) stmt`, so **the
guarded statement never runs**. On a guard whose body is `return`, that means
execution falls through into code the guard existed to protect. On a guard
whose body is a `json_decref`, it means the release never happens and the
mutant is a leak and nothing else. The two read very differently, and most of
the reasons below turn on which one a given line is.

## Buckets

| bucket | meaning |
|---|---|
| **equivalent** | the mutated program cannot behave differently from the original in any state the code can reach. No test can kill it; each entry carries its argument |
| **leak-only** | the mutant changes nothing but a reference count. Invisible to `make check`; visible to a build with leak detection, which here means Linux, because the Darwin ASan profile runs with `detect_leaks=0` |
| **allocation-failure** | on a path reached only when `malloc`, `calloc` or a jansson constructor fails. Needs heap-fault injection, which this repository does not have |
| **unreachable** | guards a state the reachable code cannot produce — `clock_gettime` failing, `pthread_cond_timedwait` returning something other than `0` or `ETIMEDOUT` |
| **undocumented guard** | a defensive check on an argument no caller passes and no header promises anything about. Killing it would pin a return code that is not part of any contract, so the next honest change to it becomes a test failure |
| **open gap** | a real gap: the behaviour is worth constraining and a test could constrain it. Named here with what such a test would have to stage |

`undocumented guard` is the bucket to be suspicious of, and the rule applied
here is deliberately narrow: **a NULL-argument guard is killed only when a
public header states the behaviour.** `maelys_mcp_provider_request_client`'s
"A NULL relay returns `MAELYS_MCP_ERR_STATE`" is such a statement, and that
mutant is now dead. `maelys_mcp_chain_resolve`'s `if (!runtime || !request)`
is not: it is internal, no caller can pass NULL, and a test calling it with
NULL would be inventing a contract rather than checking one.

## The iteration-bounds class, and why the two files needed different things

Eleven `index <` to `index <=` mutants — seven in `middleware.c`, one per hook
loop, and four in `nested.c`. They are the most interesting group in the
sweep: today's code is right, and a regression would be silent. What it takes
to kill them is not the same in the two files, and the difference is the
useful part.

**`middleware.c` needed a test, not a sanitizer.** The chain is a fixed array
of `MAELYS_MCP_MAX_MIDDLEWARE` inside the runtime, and every test in the suite
registered two or three middlewares. Below capacity, the slot one past the
last one is a zeroed slot whose hook pointers are all NULL, so the extra
iteration reads NULL, skips, and is *by construction* unobservable — the
mutant is genuinely equivalent for every chain the suite ever built. Only a
chain filled to capacity puts that index outside the array at all.
`a full chain runs every slot` fills it, and six of the seven die.

**`nested.c` needed a test for two of them and a sanitizer for the rest.**
Those loops walk a `calloc`ed array, so one index too far is a real
heap over-read that AddressSanitizer reports — but only if a test drives the
loop to its end. Three of the four break at a match, so they run past the end
only on a scan that matches nothing, which nothing in the suite did.

## The ASan kill command: what it actually bought

`mutation-asan.json` is the same configuration with `make asan-build` and
`make asan` in place of `make -j4 test-build` and `make check`:

```sh
python3 scripts/mutate.py --config mutation-asan.json \
    --files 'src/core/nested.c' --operators relational_swap \
    --confirm-survivors --report mutation-asan-report.json
```

It is deliberately **not** the default: `make asan` costs several times what
`make check` costs per mutant, and it earns that only on files whose survivors
are of a shape a sanitizer can see. Measured, mutant by mutant, rather than
assumed:

| mutant | `make check` | `make asan` (Darwin) | `make asan` (Linux, `detect_leaks=1`) |
|---|---|---|---|
| `nested.c:126` `<=` | survives | **killed** — heap-buffer-overflow | killed |
| `nested.c:151` `<=` | survives | **killed** once a test drives the scan past its last entry | killed |
| `nested.c:38` skipped `json_decref` | survives | survives | **killed** — 480 bytes in 20 allocations |
| `middleware.c:532` skipped `free` | survives | survives | **killed** — 24 bytes in 1 allocation |
| `nested.c:103` `<=` | survives | survives | survives |
| `middleware.c:270` `<=` | survives | survives | survives |
| `nested.c:45` skipped release | survives | survives | survives |

Three conclusions, in decreasing order of how obvious they were beforehand.

**It sees heap over-reads, and it sees leaks — but the leaks only on Linux.**
The whole `leak-only` bucket is invisible to `make asan` on a Mac and visible
to `make test-asan-linux`. That is a property of this repository's Makefile
(`ASAN_OPTIONS_VALUE` is split by `uname`), not of the sanitizer.

**It does not see an over-read that stays inside one allocation.**
`runtime->middleware[16]` is still within the runtime's own heap block, so
ASan has nothing to report. The natural expectation is that UBSan's
`array-bounds` covers the gap, since `middleware` is a member array of
statically known size and `-fsanitize=undefined` includes `array-bounds`. It
does not, and the reason is worth writing down because it will mislead the
next person too: **clang instruments a subscript expression but not the
address-of form these loops use.** Checked directly rather than inferred —

```c
struct e { long v; };
struct s { struct e a[16]; unsigned long count; };
static struct s box = {.count = 16};
/* &p->a[i] with i == 16: no diagnostic, exits cleanly */
for (unsigned long i = 0; i <= p->count; ++i) { const struct e *m = &p->a[i]; total += m->v; }
```

compiled with `-fsanitize=address,undefined` and `UBSAN_OPTIONS=halt_on_error=1`
runs to completion, while the same out-of-range index written as a direct load
(`v.a[i]`) reports `index 16 out of bounds for type 'long[16]'` and aborts.
Every loop in `middleware.c` takes the address. So a sanitizer will never
catch this class here, and the six that do die under `make check` die by
dereferencing a `size_t` hook counter as a function pointer — a crash, not a
diagnostic. The test is what makes them reachable; the crash is incidental.

**Where the suite never reaches the boundary, it reports exactly what
`make check` reports.** `nested.c:103` survives under every profile, because
`maelys_mcp_channel_nested_fail_id` stops at its match and nothing drives it
past one.

The honest summary is that the sanitizer profile is a second, *differently
blind* observer rather than a stronger one. Run it on a file whose survivors
are heap- or refcount-shaped. It is not a substitute for a test that reaches
the boundary.

## Scores

The baseline is #38's shipped reports. `git log 17076cf..HEAD --
src/core/nested.c src/core/middleware.c` is empty and the two files are
byte-identical to that sweep's base commit, so the mutation *sites* are
unchanged and the published survivor list still identifies real lines.

The suite has moved on since, so the *verdicts* needed corroboration rather
than assumption. Two independent checks supply it. A local sweep on `e0f603b`
— an ancestor of this branch, with the same two files — was stopped part way
through, but the 25 mutants that had run all matched #38's published verdicts.
And every mutant in the kill table below was re-verified individually after
this branch was rebased onto `main`, reproducing the same verdict and the same
killing test each time.

| file | baseline (#38) | killed here | projected |
|---|---|---|---|
| `src/core/nested.c` | 45 / 78 of 123 scored — **36.6%** | 27 under `make check`, 3 more under `mutation-asan.json` | **58.5%** (`make check`), 61.0% with the ASan variant |
| `src/core/middleware.c` | 71 / 50 of 121 scored — **58.7%** | 12 under `make check`, 1 more under `mutation-asan.json` | **68.6%** (`make check`), 69.4% with the ASan variant |

These are **projections from 47 individually verified mutants**, not a fresh
sweep: each was applied to the tree by hand, observed to be killed by a named
test, and restored. That is per-mutant evidence rather than an aggregate, and
it is a *lower bound* — a new test may kill survivors nobody hand-checked.
The official numbers are refreshed by the scheduled sweep in
`.github/workflows/mutation.yml`; treat the column above as the claim to check
against it, not as a measured result.

## Killed: mutant to killing test

Every row was verified by applying the mutant, watching the named test go red,
and restoring. `crash` means the mutant faulted the suite rather than failing
an assertion — a kill either way, but worth distinguishing.

### `src/core/nested.c`

| line | operator | killed by |
|---|---|---|
| 27 | `condition_kill` | an unconfigured table still admits a request |
| 129 | `condition_kill` | cancelling one call leaves the other nested wait |
| 142 | `condition_kill` | nested suite (crash) |
| 147 | `constant_flip` | a nested table of one admits exactly one |
| 149 | `constant_flip` | a nested table of one admits exactly one |
| 154 | `condition_kill` | a nested table of one admits exactly one |
| 159 | `constant_flip` | a nested table of one admits exactly one |
| 174 | `condition_kill`, `relational_swap`, `constant_flip` | every nested method reaches its own capability |
| 175 | `condition_kill`, `relational_swap`, `constant_flip` | every nested method reaches its own capability |
| 180 | `condition_kill` | nested suite (crash) — the NULL `out_error` call in the argument battery |
| 189 | `constant_flip`, `relational_swap` | a nested table of one admits exactly one |
| 191 | `condition_kill` | a nested table of one admits exactly one |
| 238 | `condition_kill` | nested suite (crash) — documented NULL-relay contract |
| 242 | `condition_kill` | the nested argument surface holds |
| 247 | `condition_kill` | every nested method reaches its own capability |
| 277 | `condition_kill` | nested suite (crash) — the capacity-reached path |
| 346 | `relational_swap` | every nested failure names itself |
| 348 | `condition_kill`, `relational_swap` | every nested failure names itself |
| 351 | `logical_swap` | every nested failure names itself |
| 359 | `relational_swap` | every nested failure names itself |
| 360 | `relational_swap` | every nested failure names itself |
| 38 | `condition_kill` | `mutation-asan.json` on Linux (LeakSanitizer) |
| 126 | `relational_swap` | `mutation-asan.json` — heap-buffer-overflow, existing tests |
| 151 | `relational_swap` | `mutation-asan.json` — needs the stray-reply scan |

### `src/core/middleware.c`

| line | operator | killed by |
|---|---|---|
| 7 | `condition_kill` | registration rules (crash) — the NULL `out_error` registration |
| 41 | `condition_kill` | a full chain runs every slot |
| 101, 152, 193, 228, 252, 377 | `relational_swap` | a full chain runs every slot (crash) |
| 282 | `condition_kill` | a passive list hook leaves the catalog |
| 349 | `condition_kill` | a second completion is refused |
| 379 | `condition_kill` | wrap_sink defaults to pass through (crash) |
| 515 | `logical_swap` | the compat policy accepts either callback alone |
| 532 | `condition_kill` | `mutation-asan.json` on Linux (LeakSanitizer) |

## Accepted survivors

### `src/core/nested.c` — 48

| line | operator | bucket | reason |
|---|---|---|---|
| 29 | `condition_kill` | allocation-failure | the `calloc` for the table failing |
| 39 | `condition_kill` | leak-only | skips `json_decref(entry->payload)`; killable by the Linux ASan variant, same shape as line 38 |
| 44 | `condition_kill` | equivalent | the early `return` is skipped, but `nested_capacity` is 0 whenever `nested_requests` is NULL, so the loop body never runs and `free(NULL)` is a no-op. `channel` is never NULL at the only call site |
| 44 | `logical_swap` | equivalent | `&&` only narrows a condition already false for a non-NULL channel; same argument |
| 45 | `constant_flip` | equivalent | skipping slot 0 in `nested_clear` leaks nothing: every entry is released and zeroed by its own waiter before the channel is destroyed, so slot 0 is all-zero and `release_entry_locked` on it is a no-op. Verified — survives Linux ASan with `detect_leaks=1` |
| 62 | `condition_kill` | open gap | settles an entry that is already settled, overwriting its status and payload. Needs a client reply racing a cancel on one entry |
| 62 | `logical_swap` | open gap | same double-settle window |
| 63 | `condition_kill` | leak-only | the payload delivered to a dead entry is never released; needs the same race *and* leak detection |
| 68 | `condition_kill` | leak-only | the replaced payload on a double settle |
| 75 | `condition_kill`, `logical_swap` | equivalent | as line 44 |
| 76 | `constant_flip` | equivalent | `woke = 1` only adds a `pthread_cond_broadcast` with no state change, which a waiter that re-checks `entry->settled` cannot observe |
| 79 | `condition_kill` | equivalent | the `continue` is skipped, so `settle_entry_locked` is called on dead entries — where its own guard at line 62 returns early. The only difference is the unobservable broadcast above |
| 79 | `logical_swap` | equivalent | same: live entries still settle, dead ones still early-return at line 62 |
| 83 | `logical_swap` | equivalent | broadcasting with `woke == 0` is harmless, and `nested_ready_initialized` is false only before channel setup completes, which no caller can reach |
| 91 | `condition_kill` | undocumented guard | internal; no caller passes a NULL channel |
| 101 | `condition_kill` | undocumented guard | internal; no caller passes a NULL or empty nested id |
| 103 | `relational_swap` | open gap | `fail_id` stops at its match, so it runs past the end only on an id that matches nothing — which only the process provider's death race produces. Survives `make asan` too |
| 105 | `condition_kill`, `logical_swap` | equivalent | the `continue` is skipped, but the `strcmp` on the next line rejects a zeroed entry, whose id is `""`, and an empty nested id is already refused at line 101 |
| 106 | `condition_kill` | open gap | makes `fail_id` settle the first live entry whatever its id. Needs two live entries and a targeted fail, which only the process provider issues |
| 123 | `condition_kill`, `logical_swap` | undocumented guard | internal NULL guards on `cancel_outer` |
| 125 | `constant_flip` | equivalent | as line 76 |
| 128 | `condition_kill` | equivalent | the `continue` is skipped, but `json_equal` on the next line is false for a zeroed entry's NULL `outer_id` |
| 140 | `condition_kill`, `logical_swap` | unreachable | `channel` is never NULL and the transport rejects non-object frames before `accept` sees them |
| 140 | `constant_flip` | open gap | returning 1 for a non-object frame would swallow it; the transport never delivers one |
| 142 | `logical_swap` | open gap | needs a response-shaped frame whose id is a string containing an embedded NUL |
| 147 | `condition_kill` | open gap | needs one frame carrying both a `method` and an id matching a live nested entry |
| 147 | `logical_swap` | open gap | needs one frame carrying both `result` and `error` |
| 153 | `condition_kill`, `logical_swap` | equivalent | as line 105 — a zeroed entry's id never matches a nested id, which always carries the `maelys/nested/` prefix |
| 221, 222 | `condition_kill` | allocation-failure | the failure path of `nested_request_message`, reached only when a jansson constructor fails; leak-only even then |
| 235 | `condition_kill` | undocumented guard | the header says where the result goes, not what a NULL `out_result` returns |
| 237 | `condition_kill`, `logical_swap` | undocumented guard | the header documents which methods are legal, not the verdict for an absent or empty one |
| 270 | `condition_kill` | open gap | a nested request opened on a channel that is already closing; staging it needs the claim to land between abort and teardown |
| 289 | `condition_kill` | allocation-failure | `nested_request_message` returning NULL |
| 299 | `condition_kill` | leak-only | the frame is not released when the sink refuses it; needs a sink that fails to emit |
| 301 | `condition_kill` | open gap | the emit-failure path is skipped entirely, so the call waits for a request that was never delivered. Needs a failing sink |
| 322 | `condition_kill` | unreachable | `maelys_mcp_monotonic_deadline` fails only if `clock_gettime` does |
| 332 | `condition_kill`, `relational_swap`, `constant_flip` | unreachable | `pthread_cond_timedwait` returning something other than `0` or `ETIMEDOUT` |
| 342 | `condition_kill` | open gap | the *unbind* after the wait is skipped, leaving a stale waiter binding. In-process relays set no `waiter_bind` at all, so only a process provider reaches it, and observing the staleness needs a second call in flight when the provider dies |
| 357 | `condition_kill` | equivalent | every status that reaches this line — timeout, closed, io, memory — was settled with a NULL payload, so the skipped `json_decref` had nothing to release |

### `src/core/middleware.c` — 37

| line | operator | bucket | reason |
|---|---|---|---|
| 59, 63, 67, 71, 75, 83 | `logical_swap` | equivalent | `runtime && count` becomes `runtime \|\| count`, so the predicate answers "yes" for a non-NULL runtime with no such hook. The caller then builds a hook context and traverses a chain containing no matching hook, which produces the same answer by a slower route. A cost regression, not a behavioural one; no caller passes a NULL runtime |
| 96, 151, 191, 227, 250, 268, 365, 414, 428, 514 | `condition_kill` | undocumented guard | internal NULL-argument guards on the chain entry points; no caller can pass NULL and no header promises a verdict |
| 227, 250 | `logical_swap` | undocumented guard | the same guards, narrowed rather than removed |
| 109, 128, 137 | `condition_kill` | leak-only | skipped `json_decref` on hook 1's failure and replacement paths |
| 159, 211 | `logical_swap` | leak-only | `out_error \|\| !*out_error` overwrites a message that was already set, leaking the first one. Killable by the Linux ASan variant plus a test that pre-sets `*out_error` |
| 270 | `relational_swap` | open gap (sanitizer-blind) | the slot past a full chain aliases the runtime's own counter fields. Six sibling loops die because the aliased word is a non-zero hook counter called as a function pointer; `on_list` aliases the first word of the nested config, which the middleware suite leaves zero, so the extra iteration reads a NULL hook and skips. Killing it would mean asserting on a value that exists only by struct-layout accident, and no sanitizer sees it (see above) |
| 277, 278, 283 | `condition_kill` | leak-only | skipped `json_decref` on hook 7's failure and replacement paths |
| 299, 306 | `condition_kill` | undocumented guard | `maelys_mcp_sink_emit` / `_complete` are public, but the header documents the ownership convention, not the verdict for a NULL sink |
| 312 | `condition_kill`, `logical_swap` | undocumented guard | as above for `_cancelled` |
| 312 | `constant_flip` | undocumented guard | "no sink is a closed one" is stated in the source, not in the header. Killing it would pin an undocumented return value |
| 332 | `condition_kill` | open gap | `link_cancelled` forwarding when the wrapper sets no `cancelled`. Needs something to query cancellation through a passive wrapper, which no test does |
| 465 | `condition_kill` | equivalent | `compat_on_authorize` is installed only when `authorize` is non-NULL, so `!policy->authorize` is always false inside it |
| 488 | `condition_kill` | equivalent | the same for `compat_on_audit` and `audit` |
| 515 | `condition_kill` | equivalent | with the guard skipped, a both-NULL compat policy is allocated and handed to `maelys_mcp_runtime_add_middleware`, which refuses a middleware implementing no hook with the same `MAELYS_MCP_ERR_ARGUMENT`, and the shim is freed on that failure. The observable result — return code and ownership — is identical |
| 517 | `condition_kill` | allocation-failure | the `calloc` for the shim failing |
