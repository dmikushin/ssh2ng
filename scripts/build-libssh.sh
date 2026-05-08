#!/usr/bin/env bash
# Build libssh as a static library (libssh.a) inside vendor/build.
# This produces a self-contained archive that ssh2ng.node can link
# statically — consumers don't need libssh installed at runtime.
#
# We deliberately keep libcrypto and libz as runtime DSOs (every distro
# has them), and turn off everything libssh provides that we don't use.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

if [ ! -d "vendor/libssh" ] || [ ! -f "vendor/libssh/CMakeLists.txt" ]; then
  echo "vendor/libssh is missing — run 'git submodule update --init --recursive' first" >&2
  exit 1
fi

BUILD_DIR="vendor/build"
INSTALL_DIR="vendor/install"

# Reuse a previous build if the static lib is already there.
if [ -f "$INSTALL_DIR/lib/libssh.a" ] && [ "${SSH2NG_FORCE_REBUILD:-}" != "1" ]; then
  echo "vendor/install/lib/libssh.a already built — skipping. SSH2NG_FORCE_REBUILD=1 to force."
  exit 0
fi

mkdir -p "$BUILD_DIR"

cmake -S vendor/libssh -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$ROOT/$INSTALL_DIR" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_STATIC_LIB=ON \
  -DWITH_EXAMPLES=OFF \
  -DUNIT_TESTING=OFF \
  -DCLIENT_TESTING=OFF \
  -DSERVER_TESTING=OFF \
  -DWITH_GSSAPI=OFF \
  -DWITH_NACL=OFF \
  -DWITH_BLOWFISH_CIPHER=OFF \
  -DWITH_ZLIB=ON \
  -DWITH_SERVER=OFF \
  -DWITH_PCAP=OFF \
  -DWITH_DEBUG_CRYPTO=OFF \
  -DWITH_DEBUG_PACKET=OFF \
  -DWITH_DEBUG_CALLTRACE=OFF \
  -DWITH_GCRYPT=OFF \
  -DWITH_MBEDTLS=OFF

cmake --build "$BUILD_DIR" --target ssh-static --parallel

# `cmake --install` would also lay out headers, but we just need the .a.
mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"
# Static archive lives under build/src in libssh's CMake layout.
cp "$BUILD_DIR/src/libssh.a" "$INSTALL_DIR/lib/libssh.a"
cp -r vendor/libssh/include/libssh "$INSTALL_DIR/include/"
cp "$BUILD_DIR/include/libssh/libssh_version.h" "$INSTALL_DIR/include/libssh/" || true

echo "Built $INSTALL_DIR/lib/libssh.a ($(stat -c %s "$INSTALL_DIR/lib/libssh.a") bytes)"
