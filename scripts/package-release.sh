#!/usr/bin/env bash
#
# Build and package the release tarballs for the current platform:
#   - dynamic : jansson/uriparser linked from the system
#   - static  : jansson/uriparser built from pinned, checksum-verified source
#               and linked as .a (system C library stays dynamic)
#
# One command, used both locally and by .github/workflows/release.yml.
# Assumes a C toolchain, make, cmake, curl and pkg-config are present, plus the
# jansson/uriparser dev packages for the dynamic variant. Outputs to dist/.
#
# Usage: scripts/package-release.sh [target-label]
#   target-label defaults to <os>-<arch> auto-detected from uname
#   (e.g. linux-x86_64, linux-arm64, macos-arm64, macos-x86_64).
set -euo pipefail

# Pinned dependency sources for the static variant. Update deliberately.
JANSSON_URL="https://github.com/akheron/jansson/releases/download/v2.14/jansson-2.14.tar.gz"
JANSSON_SHA="5798d010e41cf8d76b66236cfb2f2543c8d082181d16bc3085ab49538d4b9929"
URIPARSER_URL="https://github.com/uriparser/uriparser/releases/download/uriparser-1.0.2/uriparser-1.0.2.tar.gz"
URIPARSER_SHA="963554c32d40fb6cba5644f1ba63e6dd7a182b2948bd71ee448c532f53b07f1e"

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

# Portable SHA-256 (Linux: sha256sum, macOS: shasum -a 256).
sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

version="$(cat VERSION)"

target="${1:-}"
if [ -z "$target" ]; then
  case "$(uname -s)" in
    Linux)  os_label=linux ;;
    Darwin) os_label=macos ;;
    *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64)  arch_label=x86_64 ;;
    arm64|aarch64) arch_label=arm64 ;;
    *) echo "unsupported arch: $(uname -m)" >&2; exit 1 ;;
  esac
  target="${os_label}-${arch_label}"
fi

dist="$root/dist"
mkdir -p "$dist"

package() {  # $1 = variant label
  local name="mcp-runtime-${version}-${target}-$1"
  rm -rf stage && mkdir -p stage
  make install DESTDIR="$root/stage" PREFIX=/usr/local
  tar -czf "$dist/${name}.tar.gz" -C stage .
  ( cd "$dist" && sha256 "${name}.tar.gz" > "${name}.tar.gz.sha256" )
  echo "packaged ${name}.tar.gz"
}

echo "==> dynamic variant"
make clean
make all
make check
package dynamic

echo "==> static variant (jansson + uriparser from pinned source)"
deps="$root/.deps-static"
rm -rf "$deps" srcdeps && mkdir -p srcdeps
(
  cd srcdeps
  curl -fsSL -o jansson.tar.gz "$JANSSON_URL"
  echo "${JANSSON_SHA}  jansson.tar.gz" | sha256 -c -
  curl -fsSL -o uriparser.tar.gz "$URIPARSER_URL"
  echo "${URIPARSER_SHA}  uriparser.tar.gz" | sha256 -c -
  tar xzf jansson.tar.gz && tar xzf uriparser.tar.gz
  cmake -S jansson-2.14 -B jb -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED_LIBS=OFF -DJANSSON_BUILD_SHARED_LIBS=OFF \
    -DJANSSON_BUILD_DOCS=OFF -DJANSSON_WITHOUT_TESTS=ON -DJANSSON_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_LIBDIR=lib
  cmake --build jb --target install
  cmake -S uriparser-1.0.2 -B ub -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED_LIBS=OFF -DURIPARSER_BUILD_DOCS=OFF \
    -DURIPARSER_BUILD_TESTS=OFF -DURIPARSER_BUILD_TOOLS=OFF \
    -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_LIBDIR=lib
  cmake --build ub --target install
)
make clean
make all \
  MCP_DEP_CFLAGS="-I${deps}/include" \
  MCP_DEP_LIBS="${deps}/lib/libjansson.a ${deps}/lib/liburiparser.a"
./build/release/bin/maelys-mcp --help >/dev/null   # smoke: the static binary runs
package static

echo "==> artifacts in dist/:"
ls -1 "$dist"
