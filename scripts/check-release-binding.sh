#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <engineering-projectctl.toml>" >&2
  exit 2
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

python3 - "$1" "$root/.github/workflows/ci.yml" <<'PY'
from __future__ import annotations

import re
import sys
from pathlib import Path

config_path = Path(sys.argv[1])
ci_path = Path(sys.argv[2])

config_text = config_path.read_text(encoding="utf-8")
section = re.search(
    r"(?ms)^\[release_transaction\]\n(?P<body>.*?)(?=^\[|\Z)",
    config_text,
)
if not section:
    raise SystemExit("engineering configuration has no release_transaction binding")
binding_text = section.group("body")


def quoted_value(key: str) -> str:
    matches = re.findall(
        rf'(?m)^{re.escape(key)} = "([^"]+)"\s*(?:#.*)?$',
        binding_text,
    )
    if len(matches) != 1:
        raise SystemExit(f"release_transaction.{key} must occur exactly once")
    return matches[0]


def string_array(key: str) -> list[str]:
    matches = re.findall(
        rf"(?ms)^{re.escape(key)} = \[(.*?)\]\s*(?:#.*)?$",
        binding_text,
    )
    if len(matches) != 1:
        raise SystemExit(f"release_transaction.{key} must occur exactly once")
    body = matches[0]
    values = re.findall(r'"([^"]+)"', body)
    without_values = re.sub(r'"[^"]+"|,|\s|#.*', "", body)
    if without_values:
        raise SystemExit(f"release_transaction.{key} must be a string array")
    return values

expected = {
    "binding": "external-command",
    "tag_template": "v{version}",
    "metadata_paths": ["Makefile", "CHANGELOG.md"],
    "prepare": ["bash", "scripts/prepare-release.sh", "{version}"],
    "version_probe": ["bash", "scripts/release-version.sh"],
    "verify": ["bash", "scripts/check-release-contract.sh"],
}
for key, value in expected.items():
    actual = string_array(key) if isinstance(value, list) else quoted_value(key)
    if actual != value:
        raise SystemExit(f"release_transaction.{key} does not match the sealed contract")

ci_text = ci_path.read_text(encoding="utf-8")
if not re.search(r"(?m)^name: CI$", ci_text):
    raise SystemExit("ci.yml must define workflow CI")
ci_job_names = re.findall(r"(?m)^    name: (.+)$", ci_text)
if quoted_value("expected_workflow") != "CI":
    raise SystemExit("release_transaction.expected_workflow must be CI")
if string_array("expected_jobs") != ci_job_names:
    raise SystemExit("release_transaction.expected_jobs diverges from ci.yml")
PY
