#!/usr/bin/env bash
#
# One-command release. From a clean, up-to-date main, this:
#   1. writes VERSION — the single source of truth — and regenerates
#      include/maelys/mcp/version.h from it (scripts/generate-version-header.sh);
#   2. runs `make check` LOCALLY, then `make check CC=gcc` in a Linux
#      container — the gate that catches a broken bump, on both compilers
#      this codebase actually ships on, before it ever reaches CI or a tag;
#   3. opens a release PR and waits for the required checks;
#   4. merges it, then creates and pushes the annotated vX.Y.Z tag.
#
# The tag then triggers .github/workflows/release.yml, which builds the
# platform tarballs and waits for your approval on the `release` environment.
#
# Usage: scripts/cut-release.sh X.Y.Z
#   Write the CHANGELOG entry (`## X.Y.Z - <date>`) before running.
#
# v0.13.0 shipped a GCC-only -Werror bug that every clang-based check (this
# script's own local `make check`, plus ci.yml before check-gcc existed)
# missed - it only surfaced on the tag-triggered release build's Ubuntu
# runners, after the tag was already public. ci.yml's check-gcc job (#28)
# now catches this class of bug on every PR; the Docker step below catches
# it here too, before a release branch is even opened. Belt and suspenders,
# deliberately - CI is the enforced backstop, this is the fast local one.
set -euo pipefail

repo_slug="maelys-dev/mcp-runtime"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

ver="${1:-}"
if ! [[ "$ver" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  echo "usage: $0 X.Y.Z  (SemVer, no prerelease)" >&2
  exit 1
fi
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

# --- VERSION is the single source of truth; regenerate version.h from it ---
printf '%s\n' "$ver" > VERSION
bash scripts/generate-version-header.sh

# --- local gate: never tag something make check rejects ---
echo "==> make check (local gate)"
make clean >/dev/null
make check

# --- second local gate: the release build's actual compiler. CC ?= cc in
# the Makefile resolves to GCC on the release workflow's Ubuntu runners;
# make check above just ran with whatever `cc` this machine has (clang, on
# macOS), which never exercises GCC's stricter -Werror findings (see the
# header comment). Reproduce it in a throwaway Linux container instead -
# same pinned image and platform as RELEASING.md's manual reproduction
# recipe, just wired in here instead of staying documentation nobody runs
# before tagging. Architecture doesn't matter for a compiler-diagnostic
# bug, so this only reproduces one of the release matrix's three targets -
# fast, and sufficient for what this gate is actually checking. ---
if [ -n "${SKIP_DOCKER_CHECK:-}" ]; then
  echo "==> SKIP_DOCKER_CHECK set - skipping the GCC/Linux gate. Only do this if you have a real reason." >&2
elif ! command -v docker >/dev/null 2>&1; then
  echo "docker not found - required for the GCC/Linux gate (see RELEASING.md)." >&2
  echo "Install/start Docker, or set SKIP_DOCKER_CHECK=1 to bypass (not recommended)." >&2
  exit 1
else
  echo "==> make check CC=gcc, in a Linux container (second local gate)"
  docker run --rm --platform linux/arm64 -v "$root:/source:ro" ubuntu:24.04 bash -c '
    set -e
    apt-get update -qq && apt-get install -y -qq --no-install-recommends \
      gcc libc6-dev make pkg-config libjansson-dev liburiparser-dev
    cp -a /source /work && cd /work && rm -rf build
    make check CC=gcc
  '
fi

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

# --- Homebrew tap: only needs the tag (builds from the source archive), so
# it doesn't wait on the binary release workflow or its approval gate. ---
echo "==> updating the Homebrew tap"
bash scripts/update-tap-formula.sh "$tag"
