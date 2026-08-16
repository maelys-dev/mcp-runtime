#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

git clone --quiet --no-hardlinks "$root" "$tmp/repo"
cp "$root/.github/workflows/ci.yml" "$tmp/repo/.github/workflows/ci.yml"
cp "$root/.github/workflows/release.yml" "$tmp/repo/.github/workflows/release.yml"
cp "$root/.github/workflows/workflow-boundary.yml" "$tmp/repo/.github/workflows/workflow-boundary.yml"
cp "$root/Makefile" "$tmp/repo/Makefile"
for script in check-release-binding.sh check-release-contract.sh check-release-secrets.sh check-release-workflow.sh check-workflow-boundary.sh test-release-transaction.sh; do
  cp "$root/scripts/$script" "$tmp/repo/scripts/$script"
done
cd "$tmp/repo"

# The workflow-boundary checker reads candidate Git objects. Make the copied
# implementation the baseline commit before creating isolated mutations.
git config user.email "tests@mcp-runtime.invalid"
git config user.name "mcp-runtime tests"
git add Makefile .github/workflows scripts
if ! git diff --cached --quiet; then
  git commit --quiet -m "test workflow boundary baseline"
fi
boundary_baseline=$(git rev-parse HEAD)
boundary_checker="$tmp/check-workflow-boundary.sh"
cp scripts/check-workflow-boundary.sh "$boundary_checker"

bash scripts/check-release-contract.sh
/bin/bash scripts/check-release-contract.sh
bash scripts/check-release-workflow.sh

cat > release-transaction.toml <<'TOML'
[release_transaction]
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
TOML
bash scripts/check-release-binding.sh release-transaction.toml

workflow=.github/workflows/release.yml
workflow_pristine="$tmp/release.yml"
cp "$workflow" "$workflow_pristine"

reject_release_mutation() {
  local label=$1
  local old=$2
  local new=$3
  cp "$workflow_pristine" "$workflow"
  python3 - "$workflow" "$old" "$new" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old, new = sys.argv[2:]
if old not in text:
    raise SystemExit(f"mutation target missing: {old!r}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
PY
  if bash scripts/check-release-workflow.sh >/dev/null 2>&1; then
    echo "release workflow mutation was accepted: $label" >&2
    exit 1
  fi
}

reject_release_mutation "workflow_dispatch trigger" $'  push:\n' $'  workflow_dispatch:\n  push:\n'
reject_release_mutation "flow-style step with conditional" \
  '      - run: make check-all CC=clang ANALYZER=clang' \
  '      - {if: false, run: make check-all CC=clang ANALYZER=clang}'
reject_release_mutation "conditional publish" $'    needs: verify\n' $'    needs: verify\n    if: always()\n'
reject_release_mutation "ignored verification failure" \
  '      - run: make check-all CC=clang ANALYZER=clang' \
  $'      - run: make check-all CC=clang ANALYZER=clang\n        continue-on-error: true'
reject_release_mutation "masked contract with true" \
  '          bash scripts/check-release-contract.sh' \
  '          bash scripts/check-release-contract.sh || true'
reject_release_mutation "masked contract with exit zero" \
  '          bash scripts/check-release-contract.sh' \
  '          bash scripts/check-release-contract.sh || exit 0'
reject_release_mutation "masked contract with colon" \
  '          bash scripts/check-release-contract.sh' \
  '          bash scripts/check-release-contract.sh || :'
reject_release_mutation "masked contract with echo" \
  '          bash scripts/check-release-contract.sh' \
  '          bash scripts/check-release-contract.sh || echo masked'
reject_release_mutation "disabled shell errexit" \
  '          bash scripts/check-release-contract.sh' \
  $'          set +e\n          bash scripts/check-release-contract.sh'
reject_release_mutation "shell override" \
  '      - run: make check-all CC=clang ANALYZER=clang' \
  $'      - run: make check-all CC=clang ANALYZER=clang\n        shell: bash'
reject_release_mutation "release creation in verify without flags" \
  '      - run: make check-all CC=clang ANALYZER=clang' \
  $'      - run: make check-all CC=clang ANALYZER=clang\n      - run: gh release create "$GITHUB_REF_NAME"'
reject_release_mutation "second release creation in publish" \
  '      - name: Create GitHub release for the pushed tag' \
  $'      - run: gh release create "$GITHUB_REF_NAME"\n      - name: Create GitHub release for the pushed tag'
reject_release_mutation "additional publish write permission" \
  '      contents: write' \
  $'      contents: write\n      packages: write'
reject_release_mutation "self-hosted publish runner" \
  $'    needs: verify\n    runs-on: ubuntu-24.04' \
  $'    needs: verify\n    runs-on: self-hosted'
reject_release_mutation "duplicate top-level key" \
  $'name: Release\n' \
  $'name: Release\nname: Duplicate\n'
cp "$workflow_pristine" "$workflow"

ci=.github/workflows/ci.yml
ci_pristine="$tmp/ci.yml"
cp "$ci" "$ci_pristine"

reject_ci_mutation() {
  local label=$1
  local old=$2
  local new=$3
  cp "$ci_pristine" "$ci"
  python3 - "$ci" "$old" "$new" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old, new = sys.argv[2:]
if old not in text:
    raise SystemExit(f"mutation target missing: {old!r}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
PY
  if bash scripts/check-release-binding.sh release-transaction.toml >/dev/null 2>&1; then
    echo "CI mutation was accepted: $label" >&2
    exit 1
  fi
}

reject_ci_mutation "extra unnamed CI job" $'  official-conformance:\n' $'  unbound:\n    runs-on: ubuntu-24.04\n    steps: []\n\n  official-conformance:\n'
reject_ci_mutation "flow-style CI step" \
  '      - run: make test-release-contract' \
  '      - {run: make test-release-contract}'
reject_ci_mutation "missing release guard wiring" \
  $'      - run: make test-release-contract\n' \
  ''
cp "$ci_pristine" "$ci"

secret_fixture="$tmp/secrets.txt"
assert_secret_rejected() {
  local label=$1
  local value=$2
  printf '%s\n' "$value" > "$secret_fixture"
  if bash scripts/check-release-secrets.sh "$secret_fixture" >/dev/null 2>&1; then
    echo "secret heuristic accepted: $label" >&2
    exit 1
  fi
}

assert_secret_rejected "Slack app token" "SLACK_APP_TOKEN=xapp-1234567890abcdef"
assert_secret_rejected "Slack bot token" "SLACK_BOT_TOKEN=xoxb-1234567890abcdef"
assert_secret_rejected "npm token" "NPM_TOKEN=npm_123456789012345678901234567890123456"
assert_secret_rejected "PyPI token" "PYPI_API_TOKEN=pypi-AgEI12345678901234567890"
printf '%s\n' 'GH_TOKEN=${{ github.token }}' > "$secret_fixture"
bash scripts/check-release-secrets.sh "$secret_fixture"

current_version=$(bash scripts/release-version.sh)
next_version=$(python3 - "$current_version" <<'PY'
import sys

major, minor, patch = map(int, sys.argv[1].split("."))
print(f"{major}.{minor}.{patch + 1}")
PY
)
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
if RELEASE_DATE=2026-08-16 bash scripts/prepare-release.sh "$next_version" >/dev/null 2>&1; then
  echo "duplicate release preparation unexpectedly succeeded" >&2
  exit 1
fi
after=$(git diff -- Makefile CHANGELOG.md)
test "$before" = "$after"

assert_boundary_rejected() {
  local label=$1
  local candidate
  candidate=$(git rev-parse HEAD)
  # The real pull_request_target checkout remains on the base while the
  # candidate is fetched into a separate ref. Recreate that topology here.
  git reset --hard --quiet "$boundary_baseline"
  if bash "$boundary_checker" "$candidate" >/dev/null 2>&1; then
    echo "workflow boundary mutation was accepted: $label" >&2
    exit 1
  fi
}

commit_boundary_mutation() {
  local label=$1
  git add -A .github/workflows scripts
  git commit --quiet -m "test workflow boundary mutation: $label"
  assert_boundary_rejected "$label"
}

git reset --hard --quiet "$boundary_baseline"
bash "$boundary_checker" "$boundary_baseline"

# A candidate may contain arbitrary source, but the checker reads blobs only.
# The sentinel would create this marker if any candidate script were executed.
sentinel_marker="$tmp/candidate-sentinel-ran"
cat > scripts/candidate-sentinel.sh <<SH
#!/usr/bin/env bash
touch "$sentinel_marker"
SH
chmod +x scripts/candidate-sentinel.sh
git add scripts/candidate-sentinel.sh
git commit --quiet -m "test candidate sentinel"
sentinel_candidate=$(git rev-parse HEAD)
git reset --hard --quiet "$boundary_baseline"
bash "$boundary_checker" "$sentinel_candidate"
test ! -e "$sentinel_marker"

python3 - .github/workflows/ci.yml <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = "      - run: make test-release-contract\n"
path.write_text(text.replace(old, "      - if: false\n        run: make test-release-contract\n", 1), encoding="utf-8")
PY
commit_boundary_mutation "conditional CI release guard"

python3 - .github/workflows/ci.yml <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = "  check:\n    name: Native, SDK and provider checks\n"
path.write_text(text.replace(old, "  check:\n    if: false\n    name: Native, SDK and provider checks\n", 1), encoding="utf-8")
PY
commit_boundary_mutation "conditional CI check job"

python3 - .github/workflows/ci.yml <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = "      - run: make test-release-contract\n"
path.write_text(text.replace(old, "      - run: make test-release-contract\n        continue-on-error: true\n", 1), encoding="utf-8")
PY
commit_boundary_mutation "ignored CI release guard failure"

python3 - .github/workflows/ci.yml <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
path.write_text(text.replace("    runs-on: ubuntu-24.04\n", "    runs-on: self-hosted\n", 1), encoding="utf-8")
PY
commit_boundary_mutation "self-hosted CI runner"

python3 - .github/workflows/workflow-boundary.yml <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
path.write_text(text.replace("  pull_request_target:\n", "  pull_request:\n", 1), encoding="utf-8")
PY
commit_boundary_mutation "boundary trigger replacement"

python3 - scripts/check-workflow-boundary.sh <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
path.write_text(path.read_text(encoding="utf-8") + "\n# candidate tampering\n", encoding="utf-8")
PY
commit_boundary_mutation "boundary checker tampering"

printf 'name: extra workflow\n' > .github/workflows/extra.yml
commit_boundary_mutation "additional workflow"

rm .github/workflows/release.yml
ln -s ci.yml .github/workflows/release.yml
commit_boundary_mutation "workflow symlink"

if bash "$boundary_checker" HEAD >/dev/null 2>&1; then
  echo "workflow boundary accepted a non-SHA reference" >&2
  exit 1
fi
if bash "$boundary_checker" 0000000000000000000000000000000000000000 >/dev/null 2>&1; then
  echo "workflow boundary accepted an absent commit" >&2
  exit 1
fi
git reset --hard --quiet "$boundary_baseline"
