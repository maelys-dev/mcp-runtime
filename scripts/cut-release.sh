#!/usr/bin/env bash
#
# One-command release. From a clean, up-to-date main, this:
#   1. bumps the two version sources together (VERSION file + version.h);
#   2. runs `make check` LOCALLY — the gate that catches a broken bump before
#      it ever reaches CI or a tag;
#   3. opens a release PR and waits for the required checks;
#   4. merges it, then creates and pushes the annotated vX.Y.Z tag.
#
# The tag then triggers .github/workflows/release.yml, which builds the
# platform tarballs and waits for your approval on the `release` environment.
#
# Usage: scripts/cut-release.sh X.Y.Z
#   Write the CHANGELOG entry (`## X.Y.Z - <date>`) before running.
set -euo pipefail

repo_slug="maelys-dev/mcp-runtime"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

ver="${1:-}"
if ! [[ "$ver" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  echo "usage: $0 X.Y.Z  (SemVer, no prerelease)" >&2
  exit 1
fi
major="${BASH_REMATCH[1]}"; minor="${BASH_REMATCH[2]}"; patch="${BASH_REMATCH[3]}"
tag="v${ver}"

# --- preconditions: clean, on main, up to date, tag free, notes written ---
[ -z "$(git status --porcelain)" ] || { echo "working tree not clean" >&2; exit 1; }
git fetch origin --quiet
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] \
  || { echo "HEAD must equal origin/main — 'git switch main && git pull' first" >&2; exit 1; }
if git ls-remote --exit-code --tags origin "$tag" >/dev/null 2>&1; then
  echo "tag $tag already exists on origin" >&2; exit 1
fi
grep -q "^## ${ver} " CHANGELOG.md \
  || { echo "CHANGELOG.md has no '## ${ver} - <date>' entry — write the notes first" >&2; exit 1; }

# --- bump the two version sources together so they can never desync ---
printf '%s\n' "$ver" > VERSION
sed -i.bak -E \
  -e "s/^#define MAELYS_MCP_VERSION_MAJOR .*/#define MAELYS_MCP_VERSION_MAJOR ${major}u/" \
  -e "s/^#define MAELYS_MCP_VERSION_MINOR .*/#define MAELYS_MCP_VERSION_MINOR ${minor}u/" \
  -e "s/^#define MAELYS_MCP_VERSION_PATCH .*/#define MAELYS_MCP_VERSION_PATCH ${patch}u/" \
  include/maelys/mcp/version.h
rm -f include/maelys/mcp/version.h.bak

# --- local gate: never tag something make check rejects ---
echo "==> make check (local gate)"
make clean >/dev/null
make check

# --- branch, commit, PR ---
branch="release/${tag}"
git switch -c "$branch"
git add VERSION include/maelys/mcp/version.h CHANGELOG.md
git commit -q -m "release: ${ver}"
git push -u origin "$branch"
gh pr create --repo "$repo_slug" --base main --head "$branch" \
  --title "release: ${ver}" --body "Automated version bump for ${tag}."

# --- wait for required checks (aborts on red), then merge ---
echo "==> waiting for required checks…"
gh pr checks --repo "$repo_slug" "$branch" --watch
gh pr merge --repo "$repo_slug" "$branch" --squash --delete-branch

# --- tag the merged main and push ---
git switch main
git fetch origin --quiet
git tag -a "$tag" origin/main -m "mcp-runtime ${ver}"
git push origin "$tag"

echo "==> ${tag} pushed."
echo "    The Release workflow is building; approve the 'publish' job when it"
echo "    shows 'Review required' to attach the tarballs to the GitHub Release."
