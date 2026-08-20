"""Self-tests for scripts/mutate.py.

The runner exists to make a claim - "the suite notices when this line breaks" -
that nothing else in the repository checks. A tool making that claim has to be
checked itself, so these tests drive it end-to-end against the deliberately
weak and deliberately strong suites in tests/fixtures/mutation and assert the
verdicts by name.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "mutate.py"
FIXTURE = ROOT / "tests" / "fixtures" / "mutation"
SPEC = importlib.util.spec_from_file_location("mutate", MODULE_PATH)
assert SPEC and SPEC.loader
mutate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mutate)

HAVE_COMPILER = bool(shutil.which("make")) and bool(
    shutil.which(os.environ.get("CC") or "cc"))
HAVE_GIT = bool(shutil.which("git"))

# The complete site inventory of the fixture sources. Pinning it exactly is
# the point: a regression that stops finding a site and a regression that
# starts inventing one are equally bad, and only an exact inventory catches
# both.
# Column is part of the key on purpose: two sites of the same operator can
# share a line, and a set keyed only on (file, line, operator) would silently
# collapse them and stop being an inventory.
EXPECTED_SITES = {
    ("src/range.c", 10, 9, "condition_kill"),
    ("src/range.c", 10, 15, "relational_swap"),
    ("src/range.c", 11, 16, "constant_flip"),
    ("src/range.c", 13, 9, "condition_kill"),
    ("src/range.c", 13, 15, "relational_swap"),
    ("src/range.c", 14, 16, "constant_flip"),
    ("src/range.c", 16, 12, "constant_flip"),
    # The one site inside `#if 0`. The runner does not run the preprocessor,
    # so it cannot know the region is discarded; the mutant is guaranteed to
    # survive and is a documented false survivor, not a false kill.
    ("src/skips.c", 10, 28, "constant_flip"),
    ("src/skips.c", 26, 9, "condition_kill"),
    ("src/skips.c", 26, 11, "relational_swap"),
    ("src/skips.c", 26, 14, "constant_flip"),
    ("src/skips.c", 26, 16, "logical_swap"),
    ("src/skips.c", 26, 21, "relational_swap"),
    ("src/skips.c", 26, 24, "constant_flip"),
    ("src/skips.c", 27, 16, "constant_flip"),
    ("src/skips.c", 29, 12, "constant_flip"),
    # `a << 1`, `v >>= 1`, `p->left <= b` and `p->right >> 1`: exactly one
    # relational site among them, and the shifts and the arrow contribute
    # none. A scanner that rejects one of those without advancing past it
    # never terminates, so this block is the regression test for that too.
    ("src/skips.c", 38, 18, "constant_flip"),
    ("src/skips.c", 39, 11, "constant_flip"),
    ("src/skips.c", 40, 25, "relational_swap"),
    ("src/skips.c", 40, 46, "constant_flip"),
}


def sites(text, operators=None):
    mask = mutate.lexical_mask(text)
    names = operators or sorted(mutate.OPERATORS)
    found = []
    for name in names:
        for start, end, original, mutated in mutate.OPERATORS[name](text, mask):
            found.append((name, start, original, mutated))
    return sorted(found, key=lambda item: (item[1], item[0]))


def run_tool(argv):
    """Run the tool in-process and return (exit code, report)."""
    with tempfile.TemporaryDirectory() as scratch:
        report_path = Path(scratch) / "report.json"
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(
                io.StringIO()):
            code = mutate.main([*argv, "--report", str(report_path)])
        report = json.loads(report_path.read_text(encoding="utf-8"))
    return code, report


def fixture_copy(destination):
    shutil.copytree(str(FIXTURE), str(destination))
    shutil.rmtree(Path(destination) / "build", ignore_errors=True)
    return Path(destination)


def write_config(root, name, data):
    base = {
        "config_version": 1,
        "language": "c",
        "include": ["src/range.c"],
        "build_command": "true",
        "kill_command": "true",
        "timeout_seconds": 120,
        "scratch": {"strategy": "copy", "exclude": [".git", "build"]},
    }
    base.update(data)
    path = Path(root) / name
    path.write_text(json.dumps(base), encoding="utf-8")
    return path


def verdicts(report):
    return {(item["file"], item["line"], item["operator"]): item["verdict"]
            for item in report["mutants"]}


class LexicalMaskTest(unittest.TestCase):
    def test_comments_strings_chars_and_directives_are_not_code(self):
        text = ('#define A 1\n'
                'int a = 0; /* if (b == 1) */\n'
                'const char *s = "x == 0";\n'
                'char c = \'0\';\n'
                '// if (d != 1)\n')
        mask = mutate.lexical_mask(text)
        self.assertTrue(all(kind == mutate.PREPROC
                            for kind in mask[:len('#define A 1')]))
        self.assertEqual(mask[text.index("int a = 0")], mutate.CODE)
        self.assertEqual(mask[text.index("if (b == 1)")], mutate.COMMENT)
        self.assertEqual(mask[text.index("x == 0")], mutate.STRING)
        self.assertEqual(mask[text.index("'0'") + 1], mutate.CHARLIT)
        self.assertEqual(mask[text.index("if (d != 1)")], mutate.COMMENT)

    def test_a_spliced_directive_stays_a_directive(self):
        text = '#define WIDE(a, b) \\\n    ((a) == (b) && (a) > 0)\nint x = 1;\n'
        mask = mutate.lexical_mask(text)
        self.assertEqual(mask[text.index("(a) == (b)")], mutate.PREPROC)
        self.assertEqual(mask[text.index("int x = 1")], mutate.CODE)
        self.assertEqual(sites(text, ["constant_flip"])[0][2], "1")

    def test_an_escaped_quote_does_not_end_the_literal(self):
        text = 'const char *s = "a \\" == 0 still string";\nint y = 0;\n'
        mask = mutate.lexical_mask(text)
        self.assertEqual(mask[text.index("== 0 still")], mutate.STRING)
        self.assertEqual([item[1] for item in sites(text, ["constant_flip"])],
                         [text.index("int y = 0") + len("int y = ")])


class OperatorTest(unittest.TestCase):
    def test_condition_kill_only_targets_an_if_keyword(self):
        text = ('int f(int n) {\n'
                '    int notif = 0;\n'
                '    if (n) { return 1; }\n'
                '    while (n) { n--; }\n'
                '    return 0;\n'
                '}\n')
        found = sites(text, ["condition_kill"])
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][3], "0 && (n)")
        self.assertEqual(text[found[0][1] - 1], "(")

    def test_condition_kill_parenthesises_the_condition_it_kills(self):
        # `0 && a || b` parses as `(0 && a) || b`, which is `b` - a partial
        # condition mutation wearing a condition kill's label, and text this
        # repository's -Werror rejects outright. The inner parentheses are
        # what make the operator mean what it is called.
        found = sites("if (!channel || b) return;\n", ["condition_kill"])
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][3], "0 && (!channel || b)")

    def test_condition_kill_spans_a_nested_call_to_the_right_paren(self):
        found = sites("if (f(a, g(b)) == 0) { }\n", ["condition_kill"])
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][3], "0 && (f(a, g(b)) == 0)")

    def test_condition_kill_ignores_a_paren_inside_a_string(self):
        text = 'if (strcmp(s, ")") == 0) { }\n'
        found = sites(text, ["condition_kill"])
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][3], '0 && (strcmp(s, ")") == 0)')

    def test_condition_kill_is_not_applied_twice(self):
        # Both this tool's own output and the hand-injected form the manual
        # red-green discipline uses.
        self.assertEqual(sites("if (0 && (n)) { }\n", ["condition_kill"]), [])
        self.assertEqual(sites("if (0 && n) { }\n", ["condition_kill"]), [])

    def test_condition_kill_skips_an_unbalanced_condition(self):
        self.assertEqual(sites("if (a && b { }\n", ["condition_kill"]), [])

    def test_relational_swap_skips_shifts_arrows_and_assignments(self):
        text = ('int f(struct s *p, int a, int b) {\n'
                '    int v = a << 2;\n'
                '    v >>= 1;\n'
                '    v += p->n;\n'
                '    v = a <= b;\n'
                '    return v;\n'
                '}\n')
        found = sites(text, ["relational_swap"])
        self.assertEqual([(item[2], item[3]) for item in found], [("<=", "<")])

    def test_relational_swap_round_trips_equality(self):
        self.assertEqual(
            [(item[2], item[3]) for item in sites("int v = (a == b) + (c != d);\n",
                                                  ["relational_swap"])],
            [("==", "!="), ("!=", "==")])

    def test_logical_swap_skips_a_prefix_double_ampersand(self):
        text = 'void *label = &&done;\nint v = a && b;\nint w = c || d;\n'
        found = sites(text, ["logical_swap"])
        self.assertEqual([(item[2], item[3]) for item in found],
                         [("&&", "||"), ("||", "&&")])

    def test_constant_flip_skips_literals_that_are_not_standalone(self):
        text = ('int v = 0x10 + 10 + 1.5 + 0.5 + E1 + buf1 + 1u + 0UL;\n'
                'int w = 1;\n'
                'int z = 0;\n')
        found = sites(text, ["constant_flip"])
        self.assertEqual([(item[2], item[3]) for item in found],
                         [("1", "0"), ("0", "1")])
        self.assertTrue(all(item[1] > text.index("int w") for item in found))


class DisplayTest(unittest.TestCase):
    def test_a_multi_line_condition_renders_as_one_bounded_line(self):
        text = ("int f(int a, int b) {\n"
                "    if (a > 0\n"
                "        && b > 0) {\n"
                "        return 1;\n"
                "    }\n"
                "    return 0;\n"
                "}\n")
        mask = mutate.lexical_mask(text)
        start, end, original, mutated = next(
            iter(mutate.op_condition_kill(text, mask)))
        mutant = mutate.Mutant("f.c", start, end, "condition_kill", original,
                               mutated, text)
        self.assertEqual(mutant.line, 2)
        self.assertNotIn("\n", mutant.original)
        self.assertNotIn("\n", mutant.mutated)
        self.assertEqual(mutant.original, "if (a > 0 && b > 0) {")
        self.assertEqual(mutant.mutated, "if (0 && (a > 0 && b > 0)) {")

    def test_a_very_long_line_is_truncated(self):
        self.assertTrue(mutate._display("x" * 500).endswith("..."))
        self.assertEqual(len(mutate._display("x" * 500)), 200)


class SiteInventoryTest(unittest.TestCase):
    def test_the_fixture_inventory_is_exactly_the_expected_sites(self):
        code, report = run_tool([
            "--config", str(FIXTURE / "mutation-sites.json"), "--list"])
        self.assertEqual(code, 0)
        found = {(item["file"], item["line"], item["column"], item["operator"])
                 for item in report["mutants"]}
        self.assertEqual(found, EXPECTED_SITES)
        self.assertEqual(len(report["mutants"]), len(EXPECTED_SITES))

    def test_no_site_lands_on_a_comment_string_or_directive(self):
        code, report = run_tool([
            "--config", str(FIXTURE / "mutation-sites.json"), "--list"])
        self.assertEqual(code, 0)
        for item in report["mutants"]:
            self.assertNotIn("NOMUTATE", item["original"],
                             "mutated a line the mask should have excluded: %r"
                             % item["original"])

    def test_a_list_run_writes_a_versioned_report_with_run_metadata(self):
        code, report = run_tool([
            "--config", str(FIXTURE / "mutation-sites.json"), "--list"])
        self.assertEqual(code, 0)
        self.assertEqual(report["schema_version"], mutate.SCHEMA_VERSION)
        self.assertEqual(report["run"]["mode"], "full")
        self.assertTrue(report["run"]["listed_only"])
        self.assertEqual(report["run"]["kill_command"], "false")
        self.assertEqual(report["run"]["config_sha256"], hashlib.sha256(
            (FIXTURE / "mutation-sites.json").read_bytes()).hexdigest())
        self.assertFalse(report["run"]["baseline"]["checked"])
        self.assertEqual(report["totals"]["not_run"], report["totals"]["mutants"])
        self.assertIsNone(report["totals"]["mutation_score"])

    def test_shuffling_is_reproducible_for_a_seed(self):
        argv = ["--config", str(FIXTURE / "mutation-sites.json"), "--list",
                "--shuffle", "--seed", "7", "--max-mutants", "5"]
        first = run_tool(argv)[1]
        second = run_tool(argv)[1]
        order = [item["fingerprint"] for item in first["mutants"]]
        self.assertEqual(order, [item["fingerprint"]
                                 for item in second["mutants"]])
        self.assertEqual(len(order), 5)
        self.assertEqual(first["run"]["sites_found"], len(EXPECTED_SITES))

    def test_shuffling_selects_from_the_same_population(self):
        base = run_tool(["--config", str(FIXTURE / "mutation-sites.json"),
                         "--list"])[1]
        shuffled = run_tool(["--config", str(FIXTURE / "mutation-sites.json"),
                             "--list", "--shuffle", "--seed", "3"])[1]
        self.assertEqual(
            sorted(item["fingerprint"] for item in base["mutants"]),
            sorted(item["fingerprint"] for item in shuffled["mutants"]))


class RunnerTest(unittest.TestCase):
    @unittest.skipUnless(HAVE_COMPILER, "needs make and a C compiler")
    def test_the_weak_suite_leaves_the_boundary_mutants_alive(self):
        code, report = run_tool([
            "--config", str(FIXTURE / "mutation-weak.json")])
        self.assertEqual(code, 0)
        self.assertEqual(report["totals"]["mutants"], 7)
        self.assertEqual(report["totals"]["killed"], 5)
        self.assertEqual(report["totals"]["survived"], 2)
        self.assertEqual(report["totals"]["stillborn"], 0)
        self.assertEqual(report["totals"]["timeout"], 0)
        self.assertEqual(report["totals"]["mutation_score"], 0.7143)
        self.assertEqual(
            {(item["file"], item["line"], item["operator"])
             for item in report["survivors"]},
            {("src/range.c", 10, "relational_swap"),
             ("src/range.c", 13, "relational_swap")})

    @unittest.skipUnless(HAVE_COMPILER, "needs make and a C compiler")
    def test_the_strong_suite_kills_what_the_weak_suite_missed(self):
        code, report = run_tool([
            "--config", str(FIXTURE / "mutation-strong.json"),
            "--operators", "relational_swap"])
        self.assertEqual(code, 0)
        self.assertEqual(report["totals"]["mutants"], 2)
        self.assertEqual(report["totals"]["killed"], 2)
        self.assertEqual(report["totals"]["survived"], 0)
        self.assertEqual(report["totals"]["mutation_score"], 1.0)
        self.assertEqual(report["survivors"], [])

    @unittest.skipUnless(HAVE_COMPILER, "needs make and a C compiler")
    def test_a_mutant_that_cannot_build_is_stillborn_not_killed(self):
        code, report = run_tool([
            "--config", str(FIXTURE / "mutation-stillborn.json")])
        self.assertEqual(code, 0)
        self.assertEqual(report["totals"]["mutants"], 1)
        self.assertEqual(report["totals"]["stillborn"], 1)
        self.assertEqual(report["totals"]["killed"], 0)
        self.assertEqual(report["totals"]["scored"], 0)
        self.assertIsNone(report["totals"]["mutation_score"])
        self.assertNotEqual(report["mutants"][0]["build_exit"], 0)

    @unittest.skipUnless(HAVE_COMPILER, "needs make and a C compiler")
    def test_the_working_tree_is_never_mutated(self):
        before = (FIXTURE / "src" / "range.c").read_bytes()
        run_tool(["--config", str(FIXTURE / "mutation-weak.json"),
                  "--max-mutants", "1"])
        self.assertEqual((FIXTURE / "src" / "range.c").read_bytes(), before)

    def test_a_failing_baseline_aborts_before_any_verdict(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = fixture_copy(Path(scratch) / "repo")
            config = write_config(root, "baseline.json", {"kill_command": "false"})
            code, report = run_tool(["--config", str(config)])
        self.assertEqual(code, 2)
        self.assertEqual(report["mutants"], [])
        self.assertEqual(report["totals"]["killed"], 0)
        self.assertIn("failed", report["run"]["baseline"])

    def test_a_baseline_that_cannot_build_aborts_too(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = fixture_copy(Path(scratch) / "repo")
            config = write_config(root, "baseline.json",
                                  {"build_command": "false"})
            code, report = run_tool(["--config", str(config)])
        self.assertEqual(code, 2)
        self.assertIn("does not build", report["run"]["baseline"]["failed"])

    def test_confirm_survivors_runs_each_survivor_a_second_time(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = fixture_copy(Path(scratch) / "repo")
            counter = Path(scratch) / "runs"
            counter.write_text("", encoding="utf-8")
            config = write_config(root, "count.json", {
                "kill_command": 'printf "x" >> "$MUTATE_COUNTER"',
                "environment": {"MUTATE_COUNTER": str(counter)},
            })
            code, report = run_tool(["--config", str(config), "--max-mutants",
                                     "2", "--confirm-survivors"])
            runs = len(counter.read_text(encoding="utf-8"))
        self.assertEqual(code, 0)
        self.assertEqual(report["totals"]["survived"], 2)
        self.assertTrue(all(item["runs"] == 2 for item in report["mutants"]))
        # One baseline plus two runs for each of the two survivors.
        self.assertEqual(runs, 5)

    def test_a_survivor_killed_on_confirmation_is_reported_flaky(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = fixture_copy(Path(scratch) / "repo")
            counter = Path(scratch) / "runs"
            counter.write_text("0", encoding="utf-8")
            config = write_config(root, "flaky.json", {
                # Exits 0 for the baseline and the first mutant run, then
                # fails: exactly the shape of a suite that is nondeterministic
                # for this mutant.
                "kill_command": ('n=$(cat "$MUTATE_COUNTER"); n=$((n+1)); '
                                 'printf "%s" "$n" > "$MUTATE_COUNTER"; '
                                 'test "$n" -le 2'),
                "environment": {"MUTATE_COUNTER": str(counter)},
            })
            code, report = run_tool(["--config", str(config), "--max-mutants",
                                     "1", "--confirm-survivors"])
        self.assertEqual(code, 0)
        self.assertEqual(report["totals"]["flaky"], 1)
        self.assertEqual(report["totals"]["survived"], 0)
        self.assertEqual(report["totals"]["killed"], 0)
        self.assertEqual(report["totals"]["scored"], 0)
        self.assertEqual(report["survivors"], [])
        self.assertIn("nondeterministic", report["flaky"][0]["detail"])

    def test_a_mutant_that_never_finishes_is_a_timeout_not_a_kill(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = fixture_copy(Path(scratch) / "repo")
            marker = Path(scratch) / "seen"
            config = write_config(root, "slow.json", {
                # The baseline must pass, so only the mutant runs hang.
                "kill_command": ('if [ -f "$MUTATE_MARKER" ]; then sleep 60; '
                                 'fi; touch "$MUTATE_MARKER"'),
                "environment": {"MUTATE_MARKER": str(marker)},
            })
            code, report = run_tool(["--config", str(config), "--max-mutants",
                                     "1", "--timeout", "2"])
        self.assertEqual(code, 0)
        self.assertEqual(report["totals"]["timeout"], 1)
        self.assertEqual(report["totals"]["killed"], 0)
        self.assertEqual(report["totals"]["scored"], 0)
        self.assertIsNone(report["mutants"][0]["kill_exit"])

    def test_fail_on_survivors_changes_only_the_exit_code(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = fixture_copy(Path(scratch) / "repo")
            config = write_config(root, "survive.json", {"kill_command": "true"})
            code, report = run_tool(["--config", str(config), "--max-mutants",
                                     "1", "--fail-on-survivors"])
        self.assertEqual(code, 1)
        self.assertEqual(report["totals"]["survived"], 1)


class SelectionTest(unittest.TestCase):
    def test_exclude_globs_do_not_cross_a_path_segment(self):
        pattern = mutate.glob_to_regex("src/*.c")
        self.assertTrue(pattern.match("src/a.c"))
        self.assertFalse(pattern.match("src/nested/a.c"))
        recursive = mutate.glob_to_regex("src/**/*.c")
        self.assertTrue(recursive.match("src/a.c"))
        self.assertTrue(recursive.match("src/nested/a.c"))

    def test_files_mode_overrides_the_configured_include_list(self):
        # mutation-weak.json includes only src/range.c.
        code, report = run_tool([
            "--config", str(FIXTURE / "mutation-weak.json"), "--list",
            "--files", "src/skips.c"])
        self.assertEqual(code, 0)
        self.assertEqual(report["run"]["mode"], "files")
        self.assertTrue(report["run"]["listed_only"])
        self.assertEqual({item["file"] for item in report["mutants"]},
                         {"src/skips.c"})

    def test_files_mode_does_not_override_the_configured_exclude_list(self):
        # mutation-sites.json excludes src/guard.c. An exclude is a statement
        # that a file must never be mutated, so naming it on the command line
        # does not reach past it - unlike the include list, which --files is
        # explicitly a substitute for.
        code, report = run_tool([
            "--config", str(FIXTURE / "mutation-sites.json"), "--list",
            "--files", "src/guard.c"])
        self.assertEqual(code, 0)
        self.assertEqual(report["mutants"], [])
        self.assertEqual(report["run"]["sites_found"], 0)

    @unittest.skipUnless(HAVE_GIT, "needs git")
    def test_diff_mode_restricts_sites_to_the_changed_lines(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = fixture_copy(Path(scratch) / "repo")
            env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                       GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
            for args in (["init", "-q", "-b", "base"], ["add", "-A"],
                         ["commit", "-qm", "base"]):
                subprocess.run(["git", *args], cwd=str(root), env=env,
                               check=True, capture_output=True)
            source = root / "src" / "range.c"
            text = source.read_text(encoding="utf-8")
            source.write_text(
                text.replace("    return 1;\n}", "    return 1 + 0;\n}"),
                encoding="utf-8")
            config = write_config(root, "diff.json", {})
            code, report = run_tool(["--config", str(config), "--list",
                                     "--diff", "base"])
        self.assertEqual(code, 0)
        self.assertEqual(report["run"]["mode"], "diff")
        self.assertEqual(report["run"]["diff_base"], "base")
        self.assertEqual({item["line"] for item in report["mutants"]}, {16})
        self.assertEqual(len(report["mutants"]), 2)


class ConfigTest(unittest.TestCase):
    def test_an_unknown_config_version_is_refused(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "mutation.json"
            path.write_text(json.dumps({"config_version": 99}),
                            encoding="utf-8")
            with self.assertRaisesRegex(mutate.ConfigError, "config_version"):
                mutate.load_config(path)

    def test_an_unknown_operator_is_refused(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = write_config(scratch, "mutation.json",
                                {"operators": ["invert_gravity"]})
            with self.assertRaisesRegex(mutate.ConfigError, "unknown operators"):
                mutate.load_config(path)

    def test_a_missing_kill_command_is_refused(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "mutation.json"
            path.write_text(json.dumps({
                "config_version": 1, "language": "c", "include": ["src/*.c"],
            }), encoding="utf-8")
            with self.assertRaisesRegex(mutate.ConfigError, "kill_command"):
                mutate.load_config(path)

    def test_the_repository_config_is_loadable_and_selects_the_runtime(self):
        config = mutate.load_config(ROOT / "mutation.json")
        self.assertEqual(config.kill_command, "make check")
        files = mutate.select_files(ROOT, config.include, config.exclude)
        self.assertIn("src/core/middleware.c", files)
        self.assertIn("src/core/nested.c", files)
        self.assertTrue(all(name.endswith(".c") for name in files))


if __name__ == "__main__":
    unittest.main()
