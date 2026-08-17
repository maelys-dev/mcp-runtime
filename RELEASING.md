# Releasing

Releases are produced by `.github/workflows/release.yml`, triggered only by an
annotated `v*` tag. Pushing the tag is the authorization ceremony; nothing
publishes without it.

## Cutting a release

From a clean, up-to-date `main`, write the `## X.Y.Z - <date>` entry in
`CHANGELOG.md`, then run one command:

```sh
scripts/cut-release.sh X.Y.Z
```

`VERSION` is the single source of truth for the version; it writes that file and
regenerates `include/maelys/mcp/version.h` from it
(`scripts/generate-version-header.sh`) — nothing hand-edits both files. It then
runs `make check` **locally** to reject a broken bump before it ever reaches CI,
opens a release PR, waits for the required checks, merges it, and pushes the
annotated `vX.Y.Z` tag. Then approve the `release` environment when the
`publish` job requests it.

`version.h` stays a normal committed header — so `#include`-ing it works from a
plain checkout with no build step — but `make check-version-header` verifies it
was produced by the generator from `VERSION`; any manual edit that lets the two
drift fails the build immediately, not after a tag is already public.

### Manual equivalent

1. Bump `VERSION`, run `scripts/generate-version-header.sh` to regenerate
   `include/maelys/mcp/version.h`, plus `CHANGELOG.md`, on a PR merged to `main`
   after `CI` is green.
2. `git tag -a vX.Y.Z -m "mcp-runtime X.Y.Z" <merge-commit> && git push origin vX.Y.Z`
3. Approve the `release` environment.

The tag triggers a matrix build (Linux x86_64/arm64, macOS arm64), then a
separate `publish` job attaches the artifacts to the GitHub Release. The build
job has no write access and no secrets; the publish job compiles nothing and only
verifies checksums before uploading.

Finally, update the Homebrew formula (see [Homebrew tap](#homebrew-tap) below) —
two lines, so `brew install` serves the new version.

## Building the artifacts locally

The CI workflow and a local build share one script — a single source of truth:

```sh
scripts/package-release.sh            # target auto-detected from uname
scripts/package-release.sh linux-arm64  # or force a label
```

It produces both tarballs (+ `.sha256`) for the current platform in `dist/`,
building jansson/uriparser from pinned, checksum-verified source for the static
variant. It assumes a C toolchain, `make`, `cmake`, `curl`, `pkg-config` and the
jansson/uriparser dev packages are installed.

To reproduce a **Linux** build from macOS (or to check a clean environment), run
it in the pinned Ubuntu image — the container works on a copy, never the host
tree:

```sh
docker run --rm --platform linux/arm64 -v "$PWD:/source:ro" ubuntu:24.04 bash -c '
  apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config curl ca-certificates \
    libjansson-dev liburiparser-dev
  cp -a /source /work && cd /work && rm -rf build dist .deps-static srcdeps stage
  scripts/package-release.sh'
```

## Artifacts

Each platform ships **two tarballs**, both carrying a `.sha256` and a build
provenance attestation:

- `mcp-runtime-X.Y.Z-<target>-dynamic.tar.gz` — links jansson and uriparser from
  the system. Smaller; requires those libraries installed at runtime
  (`apt install libjansson4 liburiparser1`, or `brew install jansson uriparser`).
- `mcp-runtime-X.Y.Z-<target>-static.tar.gz` — bundles jansson and uriparser
  (built from pinned source, statically linked). **Standalone**: depends only on
  the system C library. Prefer this for "download and run".

Note on "static": a fully static binary is impossible on macOS (libSystem is
always dynamic) and fragile with glibc, so the static variant statically links
only the third-party deps; the system C library stays dynamic. On Linux the
binary therefore needs a glibc at least as new as the build runner's
(Ubuntu 24.04).

Windows is not supported: the runtime is POSIX-only (`fork`, `socketpair`,
`pthread`). A Windows target requires porting the runtime first.

## Verifying provenance

```sh
gh attestation verify mcp-runtime-X.Y.Z-<target>-static.tar.gz \
  --repo maelys-dev/mcp-runtime
```

This proves the artifact was built by this repository's workflow from a specific
commit. It does not prove the artifact is free of vulnerabilities — provenance is
integrity of the process, not of the source.

## Homebrew tap

Users install with `brew install maelys-dev/tap/mcp-runtime`. The formula lives
in the separate [`maelys-dev/homebrew-tap`](https://github.com/maelys-dev/homebrew-tap)
repo (`Formula/mcp-runtime.rb`) and builds from the release source tarball.

After each release, update two lines of the formula — the `url` (new tag) and its
`sha256`:

```sh
url="https://github.com/maelys-dev/mcp-runtime/archive/refs/tags/vX.Y.Z.tar.gz"
curl -fsSL "$url" | shasum -a 256      # paste both into Formula/mcp-runtime.rb
```

Homebrew works on macOS and Linux, so the one tap covers both. This is a
convenience layer on top of the release tarballs, not a replacement for them.

## Before the first binary release (one-time setup)

- **Configure the `release` environment** (Settings → Environments) with a
  required reviewer — otherwise the approval gate is a no-op — and enable branch
  protection on `main` plus restricted creation of `v*` tags.
- All three targets (Linux x86_64/arm64, macOS arm64) built successfully in the
  v0.11.0 release run. macOS Intel (x86_64) is intentionally not shipped.

Workflow actions are already pinned to full SHA; update them via a dedicated PR
by re-resolving the desired version tag.
