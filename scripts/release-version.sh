#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
makefile="$root/Makefile"

version=$(awk '
  /^VERSION := / {
    if (seen++) exit 1
    print $3
  }
  END {
    if (seen != 1) exit 1
  }
' "$makefile") || {
  echo "expected exactly one VERSION := entry in Makefile" >&2
  exit 1
}

if [[ ! $version =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
  echo "VERSION must be a release SemVer value, got: $version" >&2
  exit 1
fi

printf '%s\n' "$version"
