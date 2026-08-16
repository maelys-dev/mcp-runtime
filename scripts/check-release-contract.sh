#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
version=$(bash "$root/scripts/release-version.sh")
changelog="$root/CHANGELOG.md"

first_heading=$(sed -n 's/^## //p' "$changelog" | sed -n '1p')
release_heading=$(sed -n 's/^## //p' "$changelog" | sed -n '2p')

if [[ $first_heading != "Unreleased" || -z $release_heading ]]; then
  echo "CHANGELOG.md must begin with an Unreleased section" >&2
  exit 1
fi

if [[ ! $release_heading =~ ^${version}[[:space:]]-[[:space:]][0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
  echo "CHANGELOG.md must place $version directly after Unreleased" >&2
  exit 1
fi

release_count=$(sed -n 's/^## //p' "$changelog" | grep -Fxc "$release_heading")
if [[ $release_count -ne 1 ]]; then
  echo "CHANGELOG.md must contain exactly one $version release heading" >&2
  exit 1
fi
