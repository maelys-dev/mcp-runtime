#!/usr/bin/env python3
"""Mutation testing runner: prove the suite fails when the code is broken.

The tool reads a repository-local configuration file (mutation.json by
default), enumerates mutation sites in the configured sources, applies ONE
mutation at a time inside a scratch copy of the tree, and runs the configured
build and kill commands against it. A mutant the suite notices is *killed*; one
it does not is a *survivor*, and survivors are the actionable output.

Nothing here knows anything about the repository it is running in. Everything
repository-specific - which files to mutate, how to build, what command counts
as the test suite, how long a mutant may take - comes from the configuration
file, so the same script runs against any project that can describe itself.

Design notes worth knowing before trusting a verdict are in
docs/mutation-testing.md; the short version is that a *false kill* is the
failure mode to fear, so the runner refuses to score anything unless the
unmutated baseline builds and passes first, and --confirm-survivors exists to
separate a genuine survivor from a suite that is merely nondeterministic.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Iterable, Sequence

TOOL_NAME = "mutate.py"
TOOL_VERSION = "1.0.0"

# Bumped whenever a consumer that reads an existing report could be broken by
# the change. Additive fields do not bump it; renamed, removed or
# re-interpreted fields do.
SCHEMA_VERSION = 1

CONFIG_VERSION = 1

# Verdicts. killed/survived are the score; the rest are deliberately outside
# it, because folding an unbuildable or a nondeterministic mutant into "killed"
# is exactly the false confidence this tool exists to remove.
KILLED = "killed"
SURVIVED = "survived"
STILLBORN = "stillborn"
TIMEOUT = "timeout"
FLAKY = "flaky"
NOT_RUN = "not_run"

DEFAULT_CONFIG_NAME = "mutation.json"


class ConfigError(RuntimeError):
    pass


class BaselineError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# Lexical mask
#
# Every operator below refuses to touch a byte the mask does not call CODE.
# This is a real C tokenizer state machine rather than a regex heuristic,
# because the one thing worse than missing a mutation site is corrupting a
# string literal and reporting the resulting build failure as a finding.
# --------------------------------------------------------------------------

CODE = "c"
COMMENT = "/"
STRING = '"'
CHARLIT = "'"
PREPROC = "#"


def lexical_mask(text: str) -> str:
    """Classify every character of a C translation unit.

    Returns a string of the same length as `text` where each position holds
    CODE, COMMENT, STRING, CHARLIT or PREPROC. Only CODE positions are
    eligible for mutation.

    Known limits, stated rather than papered over: digraphs and trigraphs are
    not decoded, and a line splice (backslash-newline) inside an operator
    token is masked correctly but never mutated, because the operator scanners
    match contiguous text. Both cases skip a site rather than corrupt one.
    """
    out = [CODE] * len(text)
    index = 0
    length = len(text)
    # A directive runs to the end of its logical line; a spliced line and a
    # block comment both extend that line, so the flag outlives the states.
    in_directive = False
    line_has_code = False
    while index < length:
        char = text[index]
        nxt = text[index + 1] if index + 1 < length else ""

        if char == "\n":
            out[index] = PREPROC if in_directive else CODE
            in_directive = False
            line_has_code = False
            index += 1
            continue

        if char == "\\" and nxt == "\n" and in_directive:
            out[index] = PREPROC
            out[index + 1] = PREPROC
            index += 2
            # A spliced directive keeps going on the next physical line.
            continue

        current = PREPROC if in_directive else CODE

        if char == "/" and nxt == "*":
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            fill = PREPROC if in_directive else COMMENT
            for pos in range(index, end):
                out[pos] = COMMENT if text[pos] != "\n" else fill
            # A block comment may span lines; a directive containing one still
            # ends at the first unspliced newline, which the loop above marked.
            if in_directive and "\n" in text[index:end]:
                in_directive = False
                line_has_code = False
            index = end
            continue

        if char == "/" and nxt == "/":
            end = text.find("\n", index)
            end = length if end < 0 else end
            for pos in range(index, end):
                out[pos] = COMMENT
            index = end
            continue

        if char == '"' or char == "'":
            kind = STRING if char == '"' else CHARLIT
            out[index] = PREPROC if in_directive else kind
            pos = index + 1
            while pos < length:
                out[pos] = PREPROC if in_directive else kind
                if text[pos] == "\\" and pos + 1 < length:
                    out[pos + 1] = PREPROC if in_directive else kind
                    pos += 2
                    continue
                if text[pos] == char:
                    pos += 1
                    break
                if text[pos] == "\n":
                    # Unterminated literal: bail to end of line rather than
                    # swallowing the rest of the file.
                    break
                pos += 1
            line_has_code = True
            index = pos
            continue

        if char == "#" and not line_has_code and not in_directive:
            in_directive = True
            out[index] = PREPROC
            index += 1
            continue

        out[index] = current
        if not char.isspace():
            line_has_code = True
        index += 1
    return "".join(out)


def all_code(mask: str, start: int, end: int) -> bool:
    return all(mask[pos] == CODE for pos in range(start, end))


# --------------------------------------------------------------------------
# Operators
# --------------------------------------------------------------------------

IDENT_CHARS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
# A relational operand always ends with one of these; anything else before a
# candidate operator means the match is a fragment of a longer token.
NUMERIC_TAIL = set("0123456789.")


def _char_at(text: str, index: int) -> str:
    if index < 0 or index >= len(text):
        return ""
    return text[index]


def _prev_nonspace(text: str, index: int) -> str:
    pos = index - 1
    while pos >= 0 and text[pos] in " \t\r\n":
        pos -= 1
    return _char_at(text, pos)


IF_PATTERN = re.compile(r"\bif\b")


def _matching_paren(text: str, mask: str, open_index: int) -> int:
    """Index of the `)` closing the `(` at `open_index`, or -1.

    Only parentheses the mask calls code are counted, so a `(` inside a string
    or a comment cannot unbalance the scan.
    """
    depth = 0
    for index in range(open_index, len(text)):
        if mask[index] != CODE:
            continue
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return index
    return -1


def op_condition_kill(text: str, mask: str) -> Iterable[tuple[int, int, str, str]]:
    """`if (c)` -> `if (0 && (c))`.

    This is the break the repository's manual red-green discipline already
    injects by hand; automating it is the whole point. It is also the operator
    with the highest kill expectation, which makes an unexpected survivor here
    the most interesting single finding the tool can produce.

    The inner parentheses are not cosmetic. `&&` binds tighter than `||`, so
    the bare `if (0 && a || b)` form parses as `(0 && a) || b` - which is
    `if (b)`, a *partial* condition mutation wearing a condition kill's label.
    This repository's `-Werror -Wlogical-op-parentheses` happens to reject that
    text, so the mutant would have been recorded as stillborn rather than
    mis-scored; a repository without that warning would have been told a lie.
    Parenthesising makes the operator mean what its name says everywhere.
    """
    for match in IF_PATTERN.finditer(text):
        start = match.start()
        if not all_code(mask, start, match.end()):
            continue
        pos = match.end()
        while pos < len(text) and text[pos] in " \t\r\n":
            pos += 1
        if _char_at(text, pos) != "(" or mask[pos] != CODE:
            continue
        close = _matching_paren(text, mask, pos)
        if close < 0:
            continue
        condition = text[pos + 1:close]
        # Skip a condition already killed, in this tool's form or in the
        # hand-injected `0 && cond` form the manual discipline uses.
        if not condition.strip() or condition.lstrip().startswith("0 &&"):
            continue
        yield (pos + 1, close, condition, "0 && (%s)" % condition)


def _longest_match(text: str, index: int,
                   table: Sequence[tuple[str, str]]) -> tuple[str, str] | None:
    """First table entry matching at `index`; tables are ordered longest first."""
    for token, replacement in table:
        if text.startswith(token, index):
            return (token, replacement)
    return None


RELATIONAL_SWAPS = (
    ("<=", "<"),
    (">=", ">"),
    ("==", "!="),
    ("!=", "=="),
    ("<", "<="),
    (">", ">="),
)


def _relational_ok(text: str, start: int, token: str) -> bool:
    before = _char_at(text, start - 1)
    after = _char_at(text, start + len(token))
    if after == "=":
        # `<==` is not C, but `>>=`, `<<=`, `==` seen as `=` + `=` and friends
        # all reduce to "there is more operator here than I matched".
        return False
    if token in ("<", "<="):
        if before in ("<", "=", "!", ">"):
            return False
        if token == "<" and after == "<":
            return False
    if token in (">", ">="):
        if before in (">", "=", "!", "<", "-"):
            return False
        if token == ">" and after == ">":
            return False
    if token in ("==", "!="):
        if before in "=!<>+-*/%&|^":
            return False
    return True


def op_relational_swap(text: str, mask: str) -> Iterable[tuple[int, int, str, str]]:
    """`==`<->`!=`, `<`<->`<=`, `>`<->`>=`.

    Longest token wins at any offset, so a single site yields exactly one
    relational mutant and `<=` is never mistaken for `<` followed by `=`.
    """
    index = 0
    length = len(text)
    while index < length:
        if mask[index] != CODE:
            index += 1
            continue
        match = _longest_match(text, index, RELATIONAL_SWAPS)
        if match is None:
            index += 1
            continue
        token, replacement = match
        if (all_code(mask, index, index + len(token))
                and _relational_ok(text, index, token)):
            yield (index, index + len(token), token, replacement)
            index += len(token)
        else:
            # Advance one byte, never zero: a rejected `>` in `->` that did not
            # move the cursor would spin here forever, and `>>=` needs the
            # second `>` reconsidered on its own.
            index += 1


LOGICAL_SWAPS = (("&&", "||"), ("||", "&&"))
# A binary `&&`/`||` always has an operand to its left. Anything in this set
# immediately before the token means the `&&` is GCC's address-of-label
# prefix, or otherwise not the operator we think it is.
PREFIX_CONTEXT = set("=(,{;?:")


def op_logical_swap(text: str, mask: str) -> Iterable[tuple[int, int, str, str]]:
    """`&&` <-> `||`, with the address-of-label extension excluded."""
    index = 0
    length = len(text)
    while index < length:
        if mask[index] != CODE:
            index += 1
            continue
        match = _longest_match(text, index, LOGICAL_SWAPS)
        if match is None:
            index += 1
            continue
        token, _ = match
        previous = _prev_nonspace(text, index)
        if (all_code(mask, index, index + 2)
                and _char_at(text, index - 1) != token[0]
                and _char_at(text, index + 2) != token[0]
                and previous != ""
                and previous not in PREFIX_CONTEXT):
            yield (index, index + 2, token, match[1])
            index += 2
        else:
            index += 1


CONSTANT_FLIPS = {"0": "1", "1": "0"}


def op_constant_flip(text: str, mask: str) -> Iterable[tuple[int, int, str, str]]:
    """`0` <-> `1`, only where the digit is the whole literal.

    Rejects anything with an identifier character or a `.` on either side, so
    `0x10`, `10`, `1.5`, `.0f`, `E1` and `buf1` are all left alone. Suffixed
    literals (`1u`, `0UL`) are skipped too: the suffix is an identifier
    character, and skipping a site is always the safe direction.
    """
    for index, char in enumerate(text):
        if char not in CONSTANT_FLIPS:
            continue
        if mask[index] != CODE:
            continue
        before = _char_at(text, index - 1)
        after = _char_at(text, index + 1)
        if before in IDENT_CHARS or before == ".":
            continue
        if after in IDENT_CHARS or after == ".":
            continue
        yield (index, index + 1, char, CONSTANT_FLIPS[char])


OPERATORS = {
    "condition_kill": op_condition_kill,
    "relational_swap": op_relational_swap,
    "logical_swap": op_logical_swap,
    "constant_flip": op_constant_flip,
}


# --------------------------------------------------------------------------
# Site enumeration
# --------------------------------------------------------------------------

def _display(text: str, limit: int = 200) -> str:
    """One-line, bounded rendering of a source span for the report.

    Whitespace runs collapse to a single space so a multi-line condition stays
    one JSON string and one Markdown table cell.
    """
    collapsed = " ".join(text.split())
    if len(collapsed) <= limit:
        return collapsed
    return collapsed[:limit - 3] + "..."


class Mutant:
    def __init__(self, path: str, start: int, end: int, operator: str,
                 original_token: str, mutated_token: str, text: str) -> None:
        self.path = path
        self.start = start
        self.end = end
        self.operator = operator
        self.original_token = original_token
        self.mutated_token = mutated_token
        line_start = text.rfind("\n", 0, start) + 1
        self.line = text.count("\n", 0, start) + 1
        self.column = start - line_start + 1
        # The display window runs to the end of the line the mutated span
        # ends on, not the line it starts on: a condition spanning three lines
        # would otherwise render an `original` and a `mutated` that describe
        # different amounts of source.
        window_end = text.find("\n", max(end, line_start))
        window_end = len(text) if window_end < 0 else window_end
        self.original = _display(text[line_start:window_end])
        self.mutated = _display(text[line_start:start] + mutated_token
                                + text[end:window_end])
        self.verdict = NOT_RUN
        self.duration_s = 0.0
        self.build_exit: int | None = None
        self.kill_exit: int | None = None
        self.runs = 0
        self.detail = ""
        self.identifier = ""

    def fingerprint(self) -> str:
        digest = hashlib.sha256()
        digest.update("\x00".join([
            self.path, str(self.line), str(self.column), self.operator,
            self.original_token, self.mutated_token,
        ]).encode("utf-8"))
        return digest.hexdigest()[:16]

    def apply(self, text: str) -> str:
        return text[:self.start] + self.mutated_token + text[self.end:]

    def to_json(self) -> dict[str, Any]:
        return {
            "id": self.identifier,
            "fingerprint": self.fingerprint(),
            "file": self.path,
            "line": self.line,
            "column": self.column,
            "operator": self.operator,
            "original_token": self.original_token,
            "mutated_token": self.mutated_token,
            "original": self.original,
            "mutated": self.mutated,
            "verdict": self.verdict,
            "duration_s": round(self.duration_s, 3),
            "build_exit": self.build_exit,
            "kill_exit": self.kill_exit,
            "runs": self.runs,
            "detail": self.detail,
        }


def enumerate_mutants(root: Path, relpath: str, operators: Sequence[str],
                      lines: set[int] | None, skip_regex: re.Pattern[str] | None
                      ) -> list[Mutant]:
    text = (root / relpath).read_text(encoding="utf-8", errors="replace")
    mask = lexical_mask(text)
    found: list[Mutant] = []
    for name in operators:
        for start, end, original_token, mutated_token in OPERATORS[name](text, mask):
            mutant = Mutant(relpath, start, end, name, original_token,
                            mutated_token, text)
            if mutant.mutated == mutant.original:
                continue
            if lines is not None and mutant.line not in lines:
                continue
            if skip_regex is not None and skip_regex.search(mutant.original):
                continue
            found.append(mutant)
    found.sort(key=lambda item: (item.path, item.start, item.operator))
    return found


# --------------------------------------------------------------------------
# File selection
# --------------------------------------------------------------------------

def glob_to_regex(pattern: str) -> re.Pattern[str]:
    """Translate a `**`-aware glob into a regex over a posix relative path.

    `fnmatch`'s `*` crosses `/`, which makes `src/*.c` match `src/a/b.c`; this
    keeps `*` within one path segment and gives `**` the recursive meaning
    every other tool gives it.
    """
    parts: list[str] = []
    index = 0
    while index < len(pattern):
        char = pattern[index]
        if pattern.startswith("**/", index):
            parts.append("(?:.*/)?")
            index += 3
        elif pattern.startswith("**", index):
            parts.append(".*")
            index += 2
        elif char == "*":
            parts.append("[^/]*")
            index += 1
        elif char == "?":
            parts.append("[^/]")
            index += 1
        elif char == "[":
            end = pattern.find("]", index)
            if end < 0:
                parts.append(re.escape(char))
                index += 1
            else:
                parts.append(pattern[index:end + 1])
                index = end + 1
        else:
            parts.append(re.escape(char))
            index += 1
    return re.compile("^" + "".join(parts) + "$")


def matches_any(relpath: str, patterns: Sequence[re.Pattern[str]]) -> bool:
    return any(pattern.match(relpath) for pattern in patterns)


def select_files(root: Path, includes: Sequence[str], excludes: Sequence[str]
                 ) -> list[str]:
    exclude_res = [glob_to_regex(pattern) for pattern in excludes]
    selected: set[str] = set()
    for pattern in includes:
        for hit in glob.iglob(pattern, root_dir=str(root), recursive=True):
            relpath = Path(hit).as_posix()
            if not (root / relpath).is_file():
                continue
            if matches_any(relpath, exclude_res):
                continue
            selected.add(relpath)
    return sorted(selected)


DIFF_FILE = re.compile(r"^\+\+\+ b/(.*)$")
DIFF_HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def changed_lines(root: Path, base: str) -> dict[str, set[int]]:
    """Map each changed file to the set of lines it gained relative to `base`.

    The comparison is `merge-base(base, HEAD)` against the *working tree*, not
    against HEAD, so a developer running this before committing gets their
    uncommitted lines mutated too - which is the point of the per-PR mode.
    """
    merge_base = base
    probe = subprocess.run(["git", "merge-base", base, "HEAD"], cwd=str(root),
                           capture_output=True, text=True)
    if probe.returncode == 0 and probe.stdout.strip():
        merge_base = probe.stdout.strip()
    diff = subprocess.run(
        ["git", "diff", "--unified=0", "--no-color", "--no-ext-diff", merge_base],
        cwd=str(root), capture_output=True, text=True)
    if diff.returncode != 0:
        raise ConfigError("git diff against %s failed: %s"
                          % (base, diff.stderr.strip()))
    result: dict[str, set[int]] = {}
    current: str | None = None
    for line in diff.stdout.splitlines():
        file_match = DIFF_FILE.match(line)
        if file_match:
            name = file_match.group(1)
            current = None if name == "/dev/null" else name
            continue
        if current is None:
            continue
        hunk = DIFF_HUNK.match(line)
        if hunk:
            start = int(hunk.group(1))
            count = int(hunk.group(2)) if hunk.group(2) is not None else 1
            if count == 0:
                continue
            result.setdefault(current, set()).update(
                range(start, start + count))
    return result


# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

DEFAULT_SCRATCH_EXCLUDES = [".git", "build", "__pycache__", ".DS_Store"]


class Config:
    def __init__(self, path: Path, data: dict[str, Any], raw: bytes) -> None:
        self.path = path
        self.data = data
        self.sha256 = hashlib.sha256(raw).hexdigest()
        version = data.get("config_version")
        if version != CONFIG_VERSION:
            raise ConfigError(
                "%s: config_version %r is not supported (this tool reads %d)"
                % (path, version, CONFIG_VERSION))
        self.language = str(data.get("language", "c"))
        if self.language != "c":
            raise ConfigError("%s: language %r has no operator set in this "
                              "version" % (path, self.language))
        self.include = list(data.get("include") or [])
        if not self.include:
            raise ConfigError("%s: include must list at least one glob" % path)
        self.exclude = list(data.get("exclude") or [])
        self.operators = list(data.get("operators") or sorted(OPERATORS))
        unknown = [name for name in self.operators if name not in OPERATORS]
        if unknown:
            raise ConfigError("%s: unknown operators %s (known: %s)"
                              % (path, ", ".join(unknown),
                                 ", ".join(sorted(OPERATORS))))
        self.kill_command = data.get("kill_command")
        if not self.kill_command:
            raise ConfigError("%s: kill_command is required" % path)
        self.build_command = data.get("build_command") or ""
        self.timeout_seconds = float(data.get("timeout_seconds", 900))
        self.build_timeout_seconds = float(
            data.get("build_timeout_seconds", self.timeout_seconds))
        self.environment = dict(data.get("environment") or {})
        skip_pattern = data.get("skip_line_regex")
        self.skip_line_regex = re.compile(skip_pattern) if skip_pattern else None
        scratch = dict(data.get("scratch") or {})
        self.scratch_strategy = str(scratch.get("strategy", "copy"))
        if self.scratch_strategy != "copy":
            raise ConfigError("%s: scratch.strategy %r is not supported (this "
                              "version implements 'copy')"
                              % (path, self.scratch_strategy))
        self.scratch_root = scratch.get("root")
        self.scratch_exclude = list(
            scratch.get("exclude") or DEFAULT_SCRATCH_EXCLUDES)


def load_config(path: Path) -> Config:
    if not path.is_file():
        raise ConfigError("no mutation configuration at %s" % path)
    raw = path.read_bytes()
    try:
        data = json.loads(raw.decode("utf-8"))
    except ValueError as error:
        raise ConfigError("%s: %s" % (path, error)) from error
    if not isinstance(data, dict):
        raise ConfigError("%s: top level must be an object" % path)
    return Config(path, data, raw)


# --------------------------------------------------------------------------
# Command execution
# --------------------------------------------------------------------------

class CommandResult:
    def __init__(self, exit_code: int | None, output: str, timed_out: bool,
                 duration_s: float) -> None:
        self.exit_code = exit_code
        self.output = output
        self.timed_out = timed_out
        self.duration_s = duration_s


def run_command(command: str, cwd: Path, timeout: float,
                environment: dict[str, str]) -> CommandResult:
    """Run one shell command, killing its whole process group on timeout.

    `make` spawns compilers and test binaries; terminating only the shell
    would leave those running and let the next mutant inherit a busy machine,
    which is one more way to manufacture a false verdict.
    """
    env = os.environ.copy()
    env.update(environment)
    started = time.monotonic()
    proc = subprocess.Popen(
        command, shell=True, cwd=str(cwd), stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, errors="replace", env=env,
        start_new_session=True)
    try:
        output, _ = proc.communicate(timeout=timeout)
        return CommandResult(proc.returncode, output, False,
                             time.monotonic() - started)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            proc.kill()
        output, _ = proc.communicate()
        return CommandResult(None, output or "", True,
                             time.monotonic() - started)


def tail(output: str, limit: int = 2000) -> str:
    text = output.strip()
    if len(text) <= limit:
        return text
    return "..." + text[-limit:]


# --------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------

class Runner:
    def __init__(self, repo: Path, config: Config, scratch: Path,
                 timeout: float, confirm_survivors: bool, verbose: bool) -> None:
        self.repo = repo
        self.config = config
        self.scratch = scratch
        self.timeout = timeout
        self.confirm_survivors = confirm_survivors
        self.verbose = verbose

    def _log(self, label: str, result: CommandResult | None) -> None:
        if not self.verbose or result is None:
            return
        print("--- %s: exit %s in %.1fs ---\n%s"
              % (label, result.exit_code, result.duration_s,
                 result.output.rstrip()), file=sys.stderr)

    def _build_and_kill(self) -> tuple[CommandResult | None, CommandResult | None]:
        build: CommandResult | None = None
        if self.config.build_command:
            build = run_command(self.config.build_command, self.scratch,
                                min(self.config.build_timeout_seconds,
                                    self.timeout),
                                self.config.environment)
            self._log("build", build)
            if build.timed_out or build.exit_code != 0:
                return build, None
        kill = run_command(self.config.kill_command, self.scratch, self.timeout,
                           self.config.environment)
        self._log("kill", kill)
        return build, kill

    def baseline(self) -> dict[str, Any]:
        started = time.monotonic()
        build, kill = self._build_and_kill()
        duration = time.monotonic() - started
        record = {
            "checked": True,
            "build_exit": None if build is None else build.exit_code,
            "kill_exit": None if kill is None else kill.exit_code,
            "duration_s": round(duration, 3),
        }
        if build is not None and (build.timed_out or build.exit_code != 0):
            raise BaselineError(
                "the unmutated tree does not build in the scratch copy; every "
                "verdict would be meaningless. Build output:\n%s"
                % tail(build.output))
        if kill is None or kill.timed_out or kill.exit_code != 0:
            raise BaselineError(
                "the unmutated tree does not pass the kill command; every "
                "mutant would look killed. Kill output:\n%s"
                % tail(kill.output if kill else ""))
        return record

    def _one_pass(self, mutant: Mutant, target: Path, original: str
                  ) -> tuple[str, CommandResult | None, CommandResult | None]:
        target.write_text(mutant.apply(original), encoding="utf-8")
        build, kill = self._build_and_kill()
        if build is not None and build.timed_out:
            return TIMEOUT, build, kill
        if build is not None and build.exit_code != 0:
            return STILLBORN, build, kill
        if kill is None:
            return STILLBORN, build, kill
        if kill.timed_out:
            return TIMEOUT, build, kill
        return (SURVIVED if kill.exit_code == 0 else KILLED), build, kill

    def evaluate(self, mutant: Mutant) -> None:
        target = self.scratch / mutant.path
        original = target.read_text(encoding="utf-8")
        started = time.monotonic()
        try:
            verdict, build, kill = self._one_pass(mutant, target, original)
            mutant.runs = 1
            if verdict == SURVIVED and self.confirm_survivors:
                second, build2, kill2 = self._one_pass(mutant, target, original)
                mutant.runs = 2
                if second != SURVIVED:
                    verdict = FLAKY
                    mutant.detail = (
                        "survived the first run and was %s on confirmation; "
                        "the suite is nondeterministic for this mutant, so "
                        "neither verdict is trustworthy" % second)
                    build, kill = build2, kill2
            mutant.verdict = verdict
            mutant.build_exit = None if build is None else build.exit_code
            mutant.kill_exit = None if kill is None else kill.exit_code
            if verdict == STILLBORN and not mutant.detail:
                mutant.detail = tail(
                    (build.output if build else "") or "", 600)
            if verdict == TIMEOUT and not mutant.detail:
                mutant.detail = ("exceeded the %.0fs per-mutant timeout"
                                 % self.timeout)
        finally:
            target.write_text(original, encoding="utf-8")
            mutant.duration_s = time.monotonic() - started


def make_scratch(repo: Path, config: Config, keep: bool) -> Path:
    root = Path(config.scratch_root) if config.scratch_root else Path(
        tempfile.gettempdir())
    root.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="mutate-", dir=str(root)))
    destination = scratch / repo.name
    excludes = set(config.scratch_exclude)

    def ignore(directory: str, names: list[str]) -> set[str]:
        return {name for name in names if name in excludes}

    shutil.copytree(str(repo), str(destination), ignore=ignore,
                    symlinks=True)
    if keep:
        print("scratch: %s" % destination, file=sys.stderr)
    return destination


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------

def git_output(repo: Path, args: Sequence[str]) -> str:
    proc = subprocess.run(["git", *args], cwd=str(repo), capture_output=True,
                          text=True)
    return proc.stdout.strip() if proc.returncode == 0 else ""


def summarize(mutants: Sequence[Mutant]) -> dict[str, Any]:
    counts = {name: 0 for name in
              (KILLED, SURVIVED, STILLBORN, TIMEOUT, FLAKY, NOT_RUN)}
    for mutant in mutants:
        counts[mutant.verdict] = counts.get(mutant.verdict, 0) + 1
    scored = counts[KILLED] + counts[SURVIVED]
    return {
        "mutants": len(mutants),
        "killed": counts[KILLED],
        "survived": counts[SURVIVED],
        "stillborn": counts[STILLBORN],
        "timeout": counts[TIMEOUT],
        "flaky": counts[FLAKY],
        "not_run": counts[NOT_RUN],
        "scored": scored,
        "mutation_score": (round(counts[KILLED] / scored, 4)
                           if scored else None),
        "mutation_score_basis": "killed / (killed + survived)",
    }


def build_report(repo: Path, config: Config, args: argparse.Namespace,
                 mutants: Sequence[Mutant], sites_found: int,
                 baseline: dict[str, Any], started_at: float,
                 scratch: Path | None, mode: str) -> dict[str, Any]:
    finished = time.time()
    survivors = [mutant for mutant in mutants if mutant.verdict == SURVIVED]
    flaky = [mutant for mutant in mutants if mutant.verdict == FLAKY]
    return {
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": TOOL_NAME, "version": TOOL_VERSION},
        "run": {
            "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                        time.gmtime(started_at)),
            "finished_at": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                         time.gmtime(finished)),
            "duration_s": round(finished - started_at, 3),
            # How the sites were selected, and separately whether anything was
            # actually run. Folding --list into `mode` would erase the answer
            # to "was this a diff run?", which is the question a reader of an
            # archived report asks first.
            "mode": mode,
            "listed_only": bool(args.list),
            "base_commit": git_output(repo, ["rev-parse", "HEAD"]),
            "base_ref": git_output(repo, ["rev-parse", "--abbrev-ref", "HEAD"]),
            "diff_base": args.diff,
            "dirty_worktree": bool(git_output(repo, ["status", "--porcelain"])),
            "config_path": str(config.path.relative_to(repo))
            if config.path.is_relative_to(repo) else str(config.path),
            "config_sha256": config.sha256,
            "language": config.language,
            "operators": list(config.operators),
            "build_command": config.build_command,
            "kill_command": config.kill_command,
            "timeout_seconds": args.timeout,
            "scratch_strategy": config.scratch_strategy,
            "scratch_path": str(scratch) if scratch else None,
            "max_mutants": args.max_mutants,
            "shuffle": bool(args.shuffle),
            "seed": args.seed,
            "confirm_survivors": bool(args.confirm_survivors),
            "sites_found": sites_found,
            "baseline": baseline,
        },
        "totals": summarize(mutants),
        "mutants": [mutant.to_json() for mutant in mutants],
        "survivors": [mutant.to_json() for mutant in survivors],
        "flaky": [mutant.to_json() for mutant in flaky],
    }


def render_markdown(report: dict[str, Any]) -> str:
    totals = report["totals"]
    run = report["run"]
    score = totals["mutation_score"]
    lines = [
        "## Mutation testing",
        "",
        "| metric | value |",
        "| --- | --- |",
        "| mode | `%s` |" % run["mode"],
        "| base commit | `%s` |" % (run["base_commit"][:12] or "unknown"),
        "| kill command | `%s` |" % run["kill_command"],
        "| sites found | %d |" % run["sites_found"],
        "| mutants run | %d |" % totals["mutants"],
        "| killed | %d |" % totals["killed"],
        "| **survived** | **%d** |" % totals["survived"],
        "| stillborn (excluded from score) | %d |" % totals["stillborn"],
        "| timeout (excluded from score) | %d |" % totals["timeout"],
        "| flaky (excluded from score) | %d |" % totals["flaky"],
        "| mutation score | %s |" % (
            "%.1f%%" % (score * 100) if score is not None else "n/a"),
        "",
    ]
    if report["survivors"]:
        lines += [
            "### Survivors",
            "",
            "| file:line | operator | mutation |",
            "| --- | --- | --- |",
        ]
        for mutant in report["survivors"]:
            lines.append("| `%s:%d` | `%s` | `%s` -> `%s` |" % (
                mutant["file"], mutant["line"], mutant["operator"],
                mutant["original"].replace("|", "\\|"),
                mutant["mutated"].replace("|", "\\|")))
        lines.append("")
    else:
        lines += ["No survivors.", ""]
    if report["flaky"]:
        lines += [
            "### Nondeterministic (survived once, killed on confirmation)",
            "",
        ]
        for mutant in report["flaky"]:
            lines.append("- `%s:%d` %s" % (mutant["file"], mutant["line"],
                                           mutant["operator"]))
        lines.append("")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------

def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog=TOOL_NAME,
        description="Run mutation testing against a configured repository.")
    parser.add_argument("--config", default=None,
                        help="path to the mutation configuration "
                             "(default: %s at the repository root)"
                             % DEFAULT_CONFIG_NAME)
    parser.add_argument("--repo", default=None,
                        help="repository root (default: the config's directory)")
    parser.add_argument("--diff", metavar="BASE", default=None,
                        help="mutate only lines changed against BASE "
                             "(merge-base of BASE and HEAD, compared with the "
                             "working tree)")
    parser.add_argument("--files", metavar="GLOB", action="append", default=[],
                        help="mutate these globs instead of the configured "
                             "include list; repeatable")
    parser.add_argument("--list", action="store_true",
                        help="enumerate mutation sites and exit without "
                             "building or running anything")
    parser.add_argument("--max-mutants", type=int, default=None,
                        help="stop after this many mutants")
    parser.add_argument("--shuffle", action="store_true",
                        help="shuffle sites before applying --max-mutants")
    parser.add_argument("--seed", type=int, default=0,
                        help="seed for --shuffle (default: 0)")
    parser.add_argument("--operators", default=None,
                        help="comma-separated subset of the configured "
                             "operators")
    parser.add_argument("--timeout", type=float, default=None,
                        help="per-mutant timeout in seconds "
                             "(default: the config's timeout_seconds)")
    parser.add_argument("--confirm-survivors", action="store_true",
                        help="re-run every survivor once; a mutant that is "
                             "killed on the second run is reported as flaky "
                             "rather than as either verdict")
    parser.add_argument("--report", default=None,
                        help="write the JSON report here (default: stdout "
                             "summary only)")
    parser.add_argument("--markdown-summary", default=None,
                        help="write a Markdown summary here, for a CI job "
                             "summary")
    parser.add_argument("--keep-scratch", action="store_true",
                        help="do not delete the scratch copy on exit")
    parser.add_argument("--skip-baseline", action="store_true",
                        help="do not verify that the unmutated tree passes "
                             "first (unsafe: every verdict becomes suspect)")
    parser.add_argument("--fail-on-survivors", action="store_true",
                        help="exit non-zero when any mutant survives")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="print each command's output")
    return parser.parse_args(argv)


def resolve_paths(args: argparse.Namespace) -> tuple[Path, Path]:
    if args.config:
        config_path = Path(args.config).resolve()
        repo = Path(args.repo).resolve() if args.repo else config_path.parent
        return repo, config_path
    repo = Path(args.repo).resolve() if args.repo else Path.cwd()
    return repo, repo / DEFAULT_CONFIG_NAME


def collect(repo: Path, config: Config, args: argparse.Namespace
            ) -> tuple[list[Mutant], int, str]:
    operators = config.operators
    if args.operators:
        requested = [name.strip() for name in args.operators.split(",")
                     if name.strip()]
        unknown = [name for name in requested if name not in OPERATORS]
        if unknown:
            raise ConfigError("unknown operators: %s" % ", ".join(unknown))
        operators = requested

    line_filter: dict[str, set[int]] | None = None
    if args.diff:
        mode = "diff"
        line_filter = changed_lines(repo, args.diff)
        includes = list(config.include)
    elif args.files:
        mode = "files"
        includes = list(args.files)
    else:
        mode = "full"
        includes = list(config.include)

    files = select_files(repo, includes, config.exclude)
    if line_filter is not None:
        files = [name for name in files if name in line_filter]

    mutants: list[Mutant] = []
    for relpath in files:
        lines = line_filter.get(relpath) if line_filter is not None else None
        mutants.extend(enumerate_mutants(repo, relpath, operators, lines,
                                         config.skip_line_regex))
    sites_found = len(mutants)

    if args.shuffle:
        random.Random(args.seed).shuffle(mutants)
    if args.max_mutants is not None and args.max_mutants >= 0:
        mutants = mutants[:args.max_mutants]
    for index, mutant in enumerate(mutants, start=1):
        mutant.identifier = "m%04d" % index
    return mutants, sites_found, mode


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    started_at = time.time()
    repo, config_path = resolve_paths(args)
    try:
        config = load_config(config_path)
        mutants, sites_found, mode = collect(repo, config, args)
    except ConfigError as error:
        print("mutate: %s" % error, file=sys.stderr)
        return 2

    if args.timeout is None:
        args.timeout = config.timeout_seconds

    if args.list:
        for mutant in mutants:
            print("%s %s:%d:%d %s  %s -> %s" % (
                mutant.identifier, mutant.path, mutant.line, mutant.column,
                mutant.operator, mutant.original, mutant.mutated))
        print("%d site(s) found, %d listed" % (sites_found, len(mutants)),
              file=sys.stderr)
        report = build_report(repo, config, args, mutants, sites_found,
                              {"checked": False}, started_at, None, mode)
        write_outputs(args, report)
        return 0

    if not mutants:
        print("mutate: no mutation sites selected", file=sys.stderr)
        report = build_report(repo, config, args, mutants, sites_found,
                              {"checked": False}, started_at, None, mode)
        write_outputs(args, report)
        return 0

    scratch = make_scratch(repo, config, args.keep_scratch)
    runner = Runner(repo, config, scratch, args.timeout,
                    args.confirm_survivors, args.verbose)
    baseline: dict[str, Any] = {"checked": False}
    status = 0
    try:
        if not args.skip_baseline:
            print("mutate: verifying the unmutated baseline ...",
                  file=sys.stderr)
            try:
                baseline = runner.baseline()
            except BaselineError as error:
                print("mutate: %s" % error, file=sys.stderr)
                report = build_report(repo, config, args, [], sites_found,
                                      {"checked": True, "failed": str(error)},
                                      started_at, scratch, mode)
                write_outputs(args, report)
                return 2
            print("mutate: baseline green in %.1fs"
                  % baseline["duration_s"], file=sys.stderr)
        total = len(mutants)
        for index, mutant in enumerate(mutants, start=1):
            runner.evaluate(mutant)
            print("[%d/%d] %-9s %s:%d %s  %s -> %s (%.1fs)" % (
                index, total, mutant.verdict, mutant.path, mutant.line,
                mutant.operator, mutant.original, mutant.mutated,
                mutant.duration_s), file=sys.stderr)
    except KeyboardInterrupt:
        print("mutate: interrupted; writing a partial report", file=sys.stderr)
        status = 130
    finally:
        if not args.keep_scratch:
            shutil.rmtree(scratch.parent, ignore_errors=True)

    report = build_report(repo, config, args, mutants, sites_found, baseline,
                          started_at, scratch, mode)
    write_outputs(args, report)
    print(render_markdown(report), file=sys.stderr)
    if args.fail_on_survivors and report["totals"]["survived"]:
        return 1
    return status


def write_outputs(args: argparse.Namespace, report: dict[str, Any]) -> None:
    if args.report:
        path = Path(args.report)
        if path.parent != Path(""):
            path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.markdown_summary:
        Path(args.markdown_summary).write_text(render_markdown(report),
                                               encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
