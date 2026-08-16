#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <file>" >&2
  exit 2
fi

if ! command -v rg >/dev/null 2>&1; then
  echo "release secret heuristic requires ripgrep" >&2
  exit 127
fi

pattern='(gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|BEGIN [A-Z ]*PRIVATE KEY|xox[bp]-[A-Za-z0-9-]{10,}|xapp-[A-Za-z0-9-]{10,}|npm_[A-Za-z0-9]{36}|pypi-AgEI[A-Za-z0-9]{20,}|(^|[+[:space:]])(password|secret|api[_-]?key|token)[[:space:]]*[:=]|hooks\.slack\.com/services/)'

if rg -i -n "$pattern" "$1"; then
  echo "release secret heuristic matched" >&2
  exit 1
fi
