#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <version>" >&2
  exit 2
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
release_date=${RELEASE_DATE:-$(date -u +%F)}

python3 - "$root/Makefile" "$root/CHANGELOG.md" "$1" "$release_date" <<'PY'
from __future__ import annotations

import re
import sys
from pathlib import Path

makefile = Path(sys.argv[1])
changelog = Path(sys.argv[2])
new_version = sys.argv[3]
release_date = sys.argv[4]
semver = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")

if not semver.fullmatch(new_version):
    raise SystemExit(f"release version must be SemVer without a prerelease: {new_version}")
if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", release_date):
    raise SystemExit(f"RELEASE_DATE must use YYYY-MM-DD: {release_date}")

makefile_text = makefile.read_text(encoding="utf-8")
changelog_text = changelog.read_text(encoding="utf-8")
version_matches = list(re.finditer(r"^VERSION := ([^\n]+)$", makefile_text, re.MULTILINE))
if len(version_matches) != 1:
    raise SystemExit("expected exactly one VERSION := entry in Makefile")
current_version = version_matches[0].group(1)
if not semver.fullmatch(current_version):
    raise SystemExit(f"current VERSION is not release SemVer: {current_version}")
if tuple(map(int, new_version.split("."))) <= tuple(map(int, current_version.split("."))):
    raise SystemExit(f"release version must exceed {current_version}: {new_version}")
if re.search(rf"^## {re.escape(new_version)} - ", changelog_text, re.MULTILINE):
    raise SystemExit(f"CHANGELOG.md already contains release {new_version}")

unreleased = re.match(r"^# Changelog\n\n## Unreleased\n(?P<body>.*?)(?=^## )", changelog_text, re.DOTALL | re.MULTILINE)
if not unreleased:
    raise SystemExit("CHANGELOG.md must begin with an Unreleased section")
notes = unreleased.group("body").strip()
if not notes:
    raise SystemExit("Unreleased must contain release notes before preparation")

new_makefile = makefile_text[:version_matches[0].start(1)] + new_version + makefile_text[version_matches[0].end(1):]
new_changelog = changelog_text[:unreleased.start()] + (
    "# Changelog\n\n## Unreleased\n\n"
    f"## {new_version} - {release_date}\n\n{notes}\n\n"
) + changelog_text[unreleased.end():]

makefile.write_text(new_makefile, encoding="utf-8")
changelog.write_text(new_changelog, encoding="utf-8")
PY
