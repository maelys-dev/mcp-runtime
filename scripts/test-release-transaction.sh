#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

git clone --quiet --no-hardlinks "$root" "$tmp/repo"
cd "$tmp/repo"

bash scripts/check-release-contract.sh
bash scripts/check-release-workflow.sh
test "$(bash scripts/release-version.sh)" = "0.10.0"

python3 - <<'PY'
from pathlib import Path

path = Path("CHANGELOG.md")
text = path.read_text(encoding="utf-8")
path.write_text(text.replace("## Unreleased\n\n", "## Unreleased\n\n- Prepare the next release.\n\n", 1), encoding="utf-8")
PY

RELEASE_DATE=2026-08-16 bash scripts/prepare-release.sh 0.10.1
test "$(bash scripts/release-version.sh)" = "0.10.1"
bash scripts/check-release-contract.sh

before=$(git diff -- Makefile CHANGELOG.md)
if RELEASE_DATE=2026-08-16 bash scripts/prepare-release.sh 0.10.1; then
  echo "duplicate release preparation unexpectedly succeeded" >&2
  exit 1
fi
after=$(git diff -- Makefile CHANGELOG.md)
test "$before" = "$after"
