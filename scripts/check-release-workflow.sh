#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
workflow="$root/.github/workflows/release.yml"

python3 - "$workflow" <<'PY'
from __future__ import annotations

import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")


def top_level_block(key: str) -> str:
    match = re.search(rf"(?m)^{re.escape(key)}:\n", text)
    if not match:
        raise SystemExit(f"release workflow missing top-level {key}")
    following = re.search(r"(?m)^[A-Za-z][A-Za-z0-9_-]*:\s*$", text[match.end():])
    end = match.end() + following.start() if following else len(text)
    return text[match.end():end]


def direct_keys(block: str, indent: int) -> list[str]:
    return re.findall(rf"(?m)^{' ' * indent}([A-Za-z][A-Za-z0-9_-]*):", block)


def nested_block(block: str, key: str, indent: int) -> str:
    match = re.search(rf"(?m)^{' ' * indent}{re.escape(key)}:\n", block)
    if not match:
        raise SystemExit(f"release workflow missing {key}")
    following = re.search(rf"(?m)^{' ' * indent}[A-Za-z][A-Za-z0-9_-]*:", block[match.end():])
    end = match.end() + following.start() if following else len(block)
    return block[match.end():end]


if direct_keys(text, 0) != ["name", "on", "permissions", "jobs"]:
    raise SystemExit("release workflow must expose only name, on, permissions and jobs")
if not re.search(r"(?m)^name: Release$", text):
    raise SystemExit("release workflow name must be Release")

on_block = top_level_block("on")
if direct_keys(on_block, 2) != ["push"]:
    raise SystemExit("release workflow must have only the push trigger")
push_block = nested_block(on_block, "push", 2)
if direct_keys(push_block, 4) != ["tags"]:
    raise SystemExit("release workflow push trigger must have only tags")
tag_block = nested_block(push_block, "tags", 4)
if [line.strip() for line in tag_block.splitlines() if line.strip()] != ['- "v*"']:
    raise SystemExit('release workflow tags must be exactly ["v*"]')

permissions_block = top_level_block("permissions")
if [line.strip() for line in permissions_block.splitlines() if line.strip()] != ["contents: read"]:
    raise SystemExit("release workflow must default to contents: read")

jobs_block = top_level_block("jobs")
if direct_keys(jobs_block, 2) != ["verify", "publish"]:
    raise SystemExit("release workflow must define only verify and publish jobs")
verify_block = nested_block(jobs_block, "verify", 2)
publish_block = nested_block(jobs_block, "publish", 2)
if "permissions" in direct_keys(verify_block, 4):
    raise SystemExit("verify must not override permissions")
if not re.search(r"(?m)^    needs: verify$", publish_block):
    raise SystemExit("publish must depend on verify")
if len(re.findall(r"(?m)^\s*contents:\s*write\s*$", text)) != 1:
    raise SystemExit("release workflow must contain exactly one contents: write permission")
if not re.search(r"(?ms)^    permissions:\n      contents: write$", publish_block):
    raise SystemExit("only publish may receive contents: write")

required = {
    "version check": r'test "\$GITHUB_REF_NAME" = "v\$version"',
    "contract check": r"bash scripts/check-release-contract\.sh",
    "native verification": r"make check-all CC=clang ANALYZER=clang",
    "release creation": r'gh release create "\$GITHUB_REF_NAME" .*--verify-tag',
}
for label, pattern in required.items():
    if not re.search(pattern, text):
        raise SystemExit(f"release workflow missing {label}")
if not re.search(required["release creation"], publish_block):
    raise SystemExit("only publish may create the GitHub release")

uses_lines = [line for line in text.splitlines() if "uses:" in line]
if not uses_lines:
    raise SystemExit("release workflow must use pinned actions")
for line in uses_lines:
    if not re.fullmatch(
        r"\s*- uses: [A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}(?:\s+#.*)?",
        line,
    ):
        raise SystemExit(f"release workflow has an unpinned action: {line.strip()}")
PY
