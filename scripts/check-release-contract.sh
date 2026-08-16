#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
version=$(bash "$root/scripts/release-version.sh")
changelog="$root/CHANGELOG.md"

mapfile -t headings < <(sed -n 's/^## //p' "$changelog")

if [[ ${#headings[@]} -lt 2 || ${headings[0]} != "Unreleased" ]]; then
  echo "CHANGELOG.md must begin with an Unreleased section" >&2
  exit 1
fi

if [[ ! ${headings[1]} =~ ^${version}[[:space:]]-[[:space:]][0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
  echo "CHANGELOG.md must place $version directly after Unreleased" >&2
  exit 1
fi

release_count=$(printf '%s\n' "${headings[@]}" | grep -Fxc "$version - ${headings[1]#"$version - "}")
if [[ $release_count -ne 1 ]]; then
  echo "CHANGELOG.md must contain exactly one $version release heading" >&2
  exit 1
fi
