# Releasing

Releases are produced by `.github/workflows/release.yml`, triggered only by an
annotated `v*` tag. Pushing the tag is the authorization ceremony; nothing
publishes without it.

## Cutting a release

1. Update `VERSION` (a plain SemVer data file) and `CHANGELOG.md`, on a reviewed
   PR merged to `main`. Wait for `CI` to be green on the merge commit.
2. Tag the exact green commit and push:
   ```sh
   git tag -a vX.Y.Z -m "mcp-runtime X.Y.Z" <commit>
   git push origin vX.Y.Z
   ```
3. Approve the `release` environment when the `publish` job requests it.

The tag triggers a matrix build (Linux x86_64/arm64, macOS arm64/x86_64), then a
separate `publish` job attaches the artifacts to the GitHub Release. The build
job has no write access and no secrets; the publish job compiles nothing and only
verifies checksums before uploading.

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

## Before the first binary release (one-time setup)

- **Configure the `release` environment** (Settings → Environments) with a
  required reviewer — otherwise the approval gate is a no-op — and enable branch
  protection on `main` plus restricted creation of `v*` tags.
- The first tag will surface any per-platform build fixes; the cross-OS matrix is
  not testable locally (macOS arm64 and Linux arm64 are verified; the x86_64
  targets are exercised only on the first CI run).

Workflow actions are already pinned to full SHA; update them via a dedicated PR
by re-resolving the desired version tag.
