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

required = {
    "release name": r"^name: Release$",
    "tag trigger": r'^\s+tags:\n\s+- "v\*"$',
    "version check": r'test "\$GITHUB_REF_NAME" = "v\$version"',
    "contract check": r"bash scripts/check-release-contract\.sh",
    "native verification": r"make check-all CC=clang ANALYZER=clang",
    "publish dependency": r"(?ms)^  publish:\n.*?^    needs: verify$",
    "release creation": r'gh release create "\$GITHUB_REF_NAME" .*--verify-tag',
}
for label, pattern in required.items():
    if not re.search(pattern, text, re.MULTILINE):
        raise SystemExit(f"release workflow missing {label}")

if re.search(r"^\s+branches:", text, re.MULTILINE):
    raise SystemExit("release workflow must not run on branch pushes")
if not re.search(r"(?ms)^permissions:\n  contents: read$", text):
    raise SystemExit("release workflow must default to contents: read")
if not re.search(r"(?ms)^  publish:\n.*?^    permissions:\n      contents: write$", text):
    raise SystemExit("only the publish job may receive contents: write")
if not re.search(r"uses: actions/checkout@[0-9a-f]{40}", text):
    raise SystemExit("release workflow must pin actions/checkout by SHA")
if not re.search(r"uses: actions/setup-node@[0-9a-f]{40}", text):
    raise SystemExit("release workflow must pin actions/setup-node by SHA")
PY
