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
import tomllib
from pathlib import Path

config_path = Path(sys.argv[1])
ci_path = Path(sys.argv[2])

with config_path.open("rb") as stream:
    config = tomllib.load(stream)
binding = config.get("release_transaction")
if not isinstance(binding, dict):
    raise SystemExit("engineering configuration has no release_transaction binding")

expected = {
    "binding": "external-command",
    "tag_template": "v{version}",
    "metadata_paths": ["Makefile", "CHANGELOG.md"],
    "prepare": ["bash", "scripts/prepare-release.sh", "{version}"],
    "version_probe": ["bash", "scripts/release-version.sh"],
    "verify": ["bash", "scripts/check-release-contract.sh"],
}
for key, value in expected.items():
    if binding.get(key) != value:
        raise SystemExit(f"release_transaction.{key} does not match the sealed contract")

ci_text = ci_path.read_text(encoding="utf-8")
if not re.search(r"(?m)^name: CI$", ci_text):
    raise SystemExit("ci.yml must define workflow CI")
ci_job_names = re.findall(r"(?m)^    name: (.+)$", ci_text)
if binding.get("expected_workflow") != "CI":
    raise SystemExit("release_transaction.expected_workflow must be CI")
if binding.get("expected_jobs") != ci_job_names:
    raise SystemExit("release_transaction.expected_jobs diverges from ci.yml")
PY
