# Mutation testing

Every test in this repository is supposed to have been proven able to fail:
you break the line it covers by hand — canonically `if (cond)` becomes
`if (0 && cond)` — watch the named assertion go red, and put the line back.
That discipline has two holes. The proof is not written down anywhere, so it
dies with the session that produced it and nobody can re-check it a year
later. And it can be silently wrong: a break that passes because the test was
counting the wrong thing looks exactly like a break that passes because the
test is fine, and only the author noticing catches it.

`scripts/mutate.py` closes both. It injects the breaks itself, one at a time,
in a scratch copy of the tree; it runs the suite against each one; and it
writes a machine-readable report saying which breaks the suite noticed and
which it did not. A break the suite notices is a **killed** mutant. A break it
does not notice is a **survivor**, and survivors are the entire point of the
output: each one is a line the tests do not actually constrain.

## Running it

The per-pull-request mode mutates only the lines the branch changed, which is
what makes it affordable to run before opening a PR:

```sh
python3 scripts/mutate.py --diff origin/main --report mutation-report.json
```

The comparison is `merge-base(origin/main, HEAD)` against the **working
tree**, so uncommitted edits are mutated too.

Other modes:

```sh
# One file or a glob, ignoring the configured include list.
python3 scripts/mutate.py --files 'src/core/nested.c'

# Everything the config includes. Slow; this is what the weekly sweep does,
# under a budget.
python3 scripts/mutate.py --max-mutants 150 --shuffle --seed 12345

# Enumerate sites without building or running anything.
python3 scripts/mutate.py --files 'src/core/*.c' --list
```

`--list` is free and is the right first call on an unfamiliar file: it prints
every site the operators found, so you can see what is about to be tried
before spending an hour trying it.

Useful flags: `--operators` restricts the operator set for the run,
`--max-mutants` caps the budget, `--shuffle --seed S` makes a budgeted sweep a
reproducible random sample rather than always the first N sites in file order,
`--confirm-survivors` re-runs each survivor (see below), `--markdown-summary`
writes the same summary the CI job posts, `--keep-scratch` leaves the scratch
tree behind for debugging, `--verbose` echoes each build and kill command's
output, and `--fail-on-survivors` makes the exit code non-zero when anything
survived.

Sizing expectation for this repository: the kill command is `make check`,
which costs roughly 15–25 seconds per mutant once the scratch tree is warm.
`--diff` on a normal branch is a handful of minutes. A full sweep of `src/`
is hours, which is why the scheduled job takes a budgeted sample instead.

## The verdicts, and why there are five of them

| verdict | meaning | in the score? |
|---|---|---|
| `killed` | the kill command exited non-zero | yes |
| `survived` | the kill command exited zero — the suite did not notice | yes |
| `stillborn` | the mutated tree did not compile | **no** |
| `timeout` | the kill command exceeded the per-mutant timeout | **no** |
| `flaky` | survived once, killed on confirmation | **no** |

The mutation score is `killed / (killed + survived)` and nothing else.
Stillborn, timeout and flaky mutants are counted and listed but kept out of
the score, because each of them is a mutant about which the suite said nothing
and folding any of them into `killed` would manufacture confidence that was
never earned. A stillborn mutant in particular is common and harmless — a
compiler rejecting `arr[1]` where the array has one element proves nothing
about the tests — but it is not a kill.

Telling `stillborn` apart from `killed` requires a build step that is separate
from the test step, which is why the Makefile has a `test-build` target that
compiles everything `make check` runs without running any of it. The runner
uses it as `build_command`; a non-zero exit there is a stillborn, and only a
successful build proceeds to the kill command. The alternative — pattern
matching compiler errors out of a combined build-and-test log — is exactly the
kind of heuristic that produces a wrong verdict on the day it matters.

## Reading a report

The JSON report is versioned (`schema_version`, currently `1`) and stable
enough to diff between runs. Top level:

```json
{
  "schema_version": 1,
  "tool": { "name": "mutate.py", "version": "1.0.0" },
  "run": {
    "started_at": "2026-08-20T18:04:11Z",
    "finished_at": "2026-08-20T18:31:52Z",
    "duration_s": 1661.4,
    "mode": "diff",
    "listed_only": false,
    "base_commit": "6a5693a7771e...",
    "base_ref": "feat/mutation-testing",
    "diff_base": "origin/main",
    "dirty_worktree": false,
    "config_path": "mutation.json",
    "config_sha256": "...",
    "language": "c",
    "operators": ["condition_kill", "relational_swap", "logical_swap", "constant_flip"],
    "build_command": "make -j4 test-build",
    "kill_command": "make check",
    "timeout_seconds": 900,
    "scratch_strategy": "copy",
    "scratch_path": "/tmp/mutate-ab12cd/mcp-runtime",
    "max_mutants": 150,
    "shuffle": true,
    "seed": 12345,
    "confirm_survivors": true,
    "sites_found": 812,
    "baseline": { "checked": true, "build_exit": 0, "kill_exit": 0, "duration_s": 61.2 }
  },
  "totals": {
    "mutants": 150,
    "killed": 131,
    "survived": 12,
    "stillborn": 6,
    "timeout": 0,
    "flaky": 1,
    "not_run": 0,
    "scored": 143,
    "mutation_score": 0.9161,
    "mutation_score_basis": "killed / (killed + survived)"
  },
  "mutants": [
    {
      "id": "m0001",
      "fingerprint": "9f2c1a0b7d4e5f61",
      "file": "src/core/nested.c",
      "line": 148,
      "column": 9,
      "operator": "condition_kill",
      "original_token": "",
      "mutated_token": "0 && ",
      "original": "if (entry->state != NESTED_PENDING) {",
      "mutated": "if (0 && entry->state != NESTED_PENDING) {",
      "verdict": "killed",
      "duration_s": 18.4,
      "build_exit": 0,
      "kill_exit": 2,
      "runs": 1,
      "detail": ""
    }
  ],
  "survivors": [],
  "flaky": []
}
```

`survivors` and `flaky` repeat the matching entries from `mutants` so a
consumer can read the actionable part without filtering. `fingerprint` is a
digest of file, position, operator and tokens: it is stable across runs as
long as the surrounding line does not move, so it is the field to key on when
comparing this week's survivors against last week's.

`sites_found` versus `totals.mutants` is the budget: how many sites existed
against how many were actually run. `mode` records how the sites were
*selected* (`full`, `diff` or `files`) and `listed_only` records whether
anything was run at all, so an archived `--list --diff` report still answers
"which base was this against?".

## What is mutated

Four operators, all C:

| operator | mutation |
|---|---|
| `condition_kill` | `if (c)` → `if (0 && (c))` |
| `relational_swap` | `==`↔`!=`, `<`↔`<=`, `>`↔`>=` |
| `logical_swap` | `&&`↔`\|\|` |
| `constant_flip` | `0`↔`1`, only where the digit is the entire literal |

Before any operator runs, the file is passed through a C lexer state machine
that classifies every byte as code, comment, string literal, character literal
or preprocessor directive. **Only bytes classified as code are eligible.** A
`== 0` inside a comment, a `&&` inside a string, a `'0'` character literal and
everything on a `#define` line are all invisible to the operators. That is a
mask, not a regex heuristic, and it handles escaped quotes, block comments
spanning lines, and backslash-spliced directives.

On top of the mask, each operator refuses a site it cannot be sure of:

- `condition_kill` requires a real `if` keyword (word-boundary matched, so
  `notif` and `elif` are not it) followed by `(`, finds the matching `)` by
  counting only parentheses the mask calls code (so a `)` inside a string
  cannot unbalance it), skips an unbalanced condition, and skips a condition
  that already begins `0 &&`. It **parenthesises** the condition it kills:
  `&&` binds tighter than `||`, so the bare `if (0 && a || b)` form parses as
  `(0 && a) || b`, which is `if (b)` — a partial condition mutation wearing a
  condition kill's label. This repository's `-Werror
  -Wlogical-op-parentheses` rejects that text outright, so here it would have
  shown up as a stillborn rather than a wrong verdict; a repository without
  that warning would simply have been told a lie.
- `relational_swap` matches the longest operator at any offset, so `<=` is
  never seen as `<` followed by `=`. It rejects `<<`, `>>`, `<<=`, `>>=`,
  `->`, and any candidate whose neighbouring character shows there is more
  operator present than was matched.
- `logical_swap` requires an operand before the token, which excludes GCC's
  address-of-label `&&label` prefix.
- `constant_flip` requires that neither neighbour is an identifier character
  or a `.`, so `0x10`, `10`, `1.5`, `.0f`, `E1`, `buf1`, `1u` and `0UL` are
  all skipped. Suffixed literals are skipped rather than handled, on the
  principle that skipping a site loses a finding while corrupting one
  manufactures a false finding.

The bias throughout is to skip. A site the tool declines to mutate costs a
missed finding; a site it mutates wrongly costs a spurious stillborn or, far
worse, a misattributed survivor.

### Limits, stated rather than implied

- **Conditional compilation is not evaluated.** Code inside `#if 0` is
  classified as code, because the tool does not run the preprocessor. Mutating
  it produces a mutant that cannot possibly change behaviour, which shows up
  as a survivor. This is a *false survivor* — annoying, but in the safe
  direction. Exclude such regions with an `exclude` glob or `skip_line_regex`
  if they become noisy.
- **Equivalent mutants exist.** Some mutations genuinely do not change
  behaviour (`i < n` versus `i <= n - 1` reached by different arithmetic, a
  flag compared two ways). These survive by definition and no tool can
  distinguish them from real gaps automatically. Triage is manual.
- **Digraphs, trigraphs and mid-token line splices** are not decoded. Each
  makes the tool skip a site, never corrupt one.
- **One mutant at a time.** There is no higher-order mutation, and no
  detection of mutants that mask each other.
- **`logical_swap` is systematically stillborn on a mixed `&&`/`||` chain in
  this repository.** Swapping one operator in `a && b && c` yields
  `a || b && c`, which is a perfectly good mutant that `-Werror
  -Wlogical-op-parentheses` refuses to compile. The classification is honest —
  the mutant never ran, so it is stillborn and stays out of the score — but it
  does mean the operator's yield here is lower than its site count suggests.
  Emitting `a || (b && c)` instead would need real expression-boundary
  analysis, which is beyond what a mask-and-scan tool can do without risking
  corrupted sites, so the operator does not attempt it. If you want those
  mutants to run, relax just that diagnostic for mutant builds through the
  config's `environment` (for example an added `-Wno-parentheses`), accepting
  that the mutant is then built under slightly different flags than CI uses.

## The false-kill caveat

This is the failure mode that matters, so it gets its own section.

A mutant is called killed because the kill command exited non-zero. If the
suite fails for a reason that has nothing to do with the mutation — a
timing-sensitive assertion missing its deadline on a loaded machine, a port
collision, a flaky fixture spawn — the mutant is recorded as killed and the
real coverage gap is hidden. **A false kill is worse than no tool at all**,
because it converts an unknown into a wrong answer that nobody will re-check.

Three things are in place against it, and one is on you:

1. **The baseline gate.** Before any mutant runs, the runner builds and runs
   the unmutated scratch tree. If either fails, the run aborts with exit code
   2 and reports no verdicts at all. A suite that is already red would call
   every mutant killed, so refusing to score is the only honest response.
   `--skip-baseline` exists and should be treated as a debugging flag.
2. **`--confirm-survivors`.** A survivor is re-run before it is reported. If
   the second run kills it, the mutant is recorded as `flaky`, not as killed
   and not as survived, with the detail spelling out that the suite is
   nondeterministic for it. That catches the false-kill's mirror image and
   surfaces suite nondeterminism as a finding in itself. The CI sweep always
   passes it.
3. **Process-group kills on timeout.** A timed-out mutant's entire process
   group is killed, so a hung compiler or test binary cannot leave the machine
   loaded and push the *next* mutant into a spurious failure.

What is not automated: `--confirm-survivors` only re-runs survivors. It cannot
detect a mutant that was killed spuriously on its only run, because a kill is
not re-run. If a kill looks surprising — a mutation in a line you did not
think was covered — reproduce it by hand before believing it. This repository
has had exactly this class of flake (fixture spawns missing a default 5-second
describe deadline under sanitizers on a loaded machine, fixed in `553a8d7`),
and the fix does not make the caveat go away.

The configured kill command for this repository is `make check`, whose
performance assertions are deliberately loose for the same reason. If a sweep
starts producing kills you distrust, override the kill command in
`mutation.json` — for example to a single suite, or to `make check` under a
stricter environment — rather than reading the numbers more optimistically.

## Configuration contract

Everything repository-specific lives in `mutation.json` at the repository
root; `scripts/mutate.py` contains no knowledge of this repository at all.

```json
{
  "config_version": 1,
  "language": "c",
  "include": ["src/**/*.c"],
  "exclude": ["src/**/*.h"],
  "operators": ["condition_kill", "relational_swap", "logical_swap", "constant_flip"],
  "build_command": "make -j4 test-build",
  "kill_command": "make check",
  "build_timeout_seconds": 600,
  "timeout_seconds": 900,
  "scratch": {
    "strategy": "copy",
    "exclude": [".git", "build", "__pycache__", ".DS_Store", "dist", "stage"]
  }
}
```

| key | meaning |
|---|---|
| `config_version` | must equal the version the tool reads; a mismatch is refused rather than guessed at |
| `language` | selects the operator set; only `c` has one today |
| `include` / `exclude` | `**`-aware globs relative to the repository root. `*` does not cross a `/`. `--files` substitutes for `include`; `exclude` still applies, because an exclude is a statement that a file must never be mutated |
| `operators` | subset of the language's operators; an unknown name is refused |
| `build_command` | optional. Non-zero exit ⇒ `stillborn`. Omit it and every build failure becomes an indistinguishable `killed` |
| `kill_command` | required. Non-zero exit ⇒ `killed` |
| `timeout_seconds` / `build_timeout_seconds` | per-mutant caps |
| `environment` | extra environment variables for both commands |
| `skip_line_regex` | optional; any source line matching it yields no mutants |
| `scratch.strategy` | `copy` today |
| `scratch.root` | where scratch trees are made; defaults to the system temp dir |
| `scratch.exclude` | directory names not copied into the scratch tree |

Adding a language means adding an operator table and a lexical mask for it;
nothing else in the runner is language-aware. That is the promotion path: the
tool is intended to move into the private multi-language engineering toolkit
(`projectctl`) unchanged, with each repository carrying its own
`mutation.json`. The schema version and the config version are separate on
purpose, so a toolkit-side report consumer and a repository-side config can
move independently.

### What the runner assumes of your build and kill commands

The scratch tree is built once and then reused: each mutant is one file
rewritten in place followed by an *incremental* build. That is what makes a
sweep affordable, and it puts one requirement on the configured commands —
**the build must actually rebuild from the mutated source.** A build system
that decides freshness by source timestamp (make, ninja) does the right thing,
because the runner rewrites the file before every mutant and restores it
after, so the source is strictly newer than the object built from it. A build
system with a content-addressed cache that is *shared with the host tree*
could serve a stale artifact; point such a cache at the scratch tree or
disable it in `environment`.

If a rebuild were ever skipped the symptom would be conspicuous rather than
silent — every mutant in the file would survive at once, including the
`condition_kill` on a line the suite plainly covers — but it is worth knowing
what to suspect.

### Why a scratch copy rather than a `git worktree`

Both were viable. The copy won on two grounds. A worktree can only ever
contain committed state, and `--diff` mode exists specifically to be run on a
branch *before* committing — mutating a tree that does not match the one the
developer is looking at would be a subtle lie. And a worktree mutates the real
repository (`.git/worktrees` entries, a branch or a detached checkout) where a
copy touches nothing; an interrupted run leaves a stale worktree registration
behind, and this tool is expected to be interrupted. The cost is a full tree
copy per run — one `shutil.copytree` of a few megabytes, once, not per mutant,
with `build/` and `.git/` excluded — and one cold build to warm the scratch
tree. Every mutant after that is an incremental rebuild of a single
translation unit.

The real tree is never written to. One self-test asserts exactly that.

## Continuous integration

`.github/workflows/mutation.yml` runs a budgeted sweep on `workflow_dispatch`
and on a weekly cron. It is **not** a required check and does not block
anything: a random budgeted sample gives a different answer every week, so
gating on it would gate on the seed. The job runs the runner's own self-tests
first — a sweep from a broken runner produces confident verdicts nobody
checked — then sweeps 150 mutants with `--confirm-survivors`, writes the
summary and survivor table into the job summary, and uploads the JSON report
as an artifact for 90 days. The dispatch form takes a budget, a seed and an
optional glob.

## The runner's own tests

`tests/test_mutate.py`, run by `make check-sdks` alongside the other Python
suites. `make check` deliberately does not run them: it must stay buildable
with nothing but a C compiler, make and pkg-config, which is what the GCC CI
job installs.

The end-to-end tests drive the fixture project in `tests/fixtures/mutation`: a
five-line `in_range` function, a **deliberately weak** suite that only ever
probes interior points, and a **strong** suite that adds both boundaries. The
weak suite kills five of the seven mutants and lets both relational swaps
through; the strong suite kills them. The tests assert those verdicts by file,
line and operator rather than by count alone. A third fixture,
`src/guard.c`, holds a `_Static_assert` whose literal is the mutation site, so
flipping it is a guaranteed compile failure on any C11 compiler — that is the
deterministic `stillborn` case. `src/skips.c` carries `if`, `==`, `&&`, `0`
and `1` inside comments, strings, a character literal and `#define` lines,
plus a block of shapes that only look like relational operators (`<<`, `>>`,
`>>=`, `->`). The site inventory for both fixture sources is pinned exactly,
by file, line, column and operator, so a regression that stops finding a site
and one that starts inventing one both fail. That inventory is not
hypothetical: the first version of the relational scanner rejected the `>` in
`->` without advancing past it and looped forever, which is why those shapes
are in the fixture.
