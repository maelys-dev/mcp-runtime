#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

git clone --quiet --no-hardlinks "$root" "$tmp/repo"
cd "$tmp/repo"

bash scripts/check-release-contract.sh
bash scripts/check-release-workflow.sh
current_version=$(bash scripts/release-version.sh)
next_version=$(python3 - "$current_version" <<'PY'
import sys

major, minor, patch = map(int, sys.argv[1].split("."))
print(f"{major}.{minor}.{patch + 1}")
PY
)

python3 - <<'PY'
from pathlib import Path

Path("release-transaction.toml").write_text(
    """[release_transaction]
binding = "external-command"
tag_template = "v{version}"
expected_workflow = "CI"
expected_jobs = [
  "Native, SDK and provider checks",
  "ASan, UBSan and TSan",
  "Fuzzer smoke campaigns",
  "Supported official MCP scenarios",
]
metadata_paths = ["Makefile", "CHANGELOG.md"]
prepare = ["bash", "scripts/prepare-release.sh", "{version}"]
version_probe = ["bash", "scripts/release-version.sh"]
verify = ["bash", "scripts/check-release-contract.sh"]
""",
    encoding="utf-8",
)
PY
bash scripts/check-release-binding.sh release-transaction.toml

workflow=.github/workflows/release.yml
workflow_pristine="$tmp/release.yml"
cp "$workflow" "$workflow_pristine"

reject_workflow_mutation() {
  local label=$1
  local old=$2
  local new=$3
  cp "$workflow_pristine" "$workflow"
  python3 - "$workflow" "$old" "$new" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = sys.argv[2]
new = sys.argv[3]
if old not in text:
    raise SystemExit(f"mutation target missing: {old!r}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
PY
  if bash scripts/check-release-workflow.sh; then
    echo "workflow mutation was accepted: $label" >&2
    exit 1
  fi
}

reject_workflow_mutation "workflow_dispatch trigger" $'  push:\n' $'  workflow_dispatch:\n  push:\n'
reject_workflow_mutation "pull_request trigger" $'  push:\n' $'  pull_request:\n  push:\n'
reject_workflow_mutation "branch trigger" $'    tags:\n' $'    branches:\n      - main\n    tags:\n'
reject_workflow_mutation "verify write permission" $'  verify:\n' $'  verify:\n    permissions:\n      contents: write\n'
reject_workflow_mutation "unpinned action" \
  'actions/setup-node@a0853c24544627f65ddf259abe73b1d18a591444' 'actions/setup-node@v5.0.0'
cp "$workflow_pristine" "$workflow"

cp release-transaction.toml release-transaction-invalid.toml
python3 - <<'PY'
from pathlib import Path

path = Path("release-transaction-invalid.toml")
text = path.read_text(encoding="utf-8")
path.write_text(
    text.replace("Fuzzer smoke campaigns", "Unexpected CI job", 1),
    encoding="utf-8",
)
PY
if bash scripts/check-release-binding.sh release-transaction-invalid.toml; then
  echo "binding/CI divergence was accepted" >&2
  exit 1
fi

python3 - <<'PY'
from pathlib import Path

path = Path("CHANGELOG.md")
text = path.read_text(encoding="utf-8")
path.write_text(text.replace("## Unreleased\n\n", "## Unreleased\n\n- Prepare the next release.\n\n", 1), encoding="utf-8")
PY

RELEASE_DATE=2026-08-16 bash scripts/prepare-release.sh "$next_version"
test "$(bash scripts/release-version.sh)" = "$next_version"
bash scripts/check-release-contract.sh

before=$(git diff -- Makefile CHANGELOG.md)
if RELEASE_DATE=2026-08-16 bash scripts/prepare-release.sh "$next_version"; then
  echo "duplicate release preparation unexpectedly succeeded" >&2
  exit 1
fi
after=$(git diff -- Makefile CHANGELOG.md)
test "$before" = "$after"
