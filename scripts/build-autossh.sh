#!/usr/bin/env bash
# Build the vendored autossh fork as a static library for ssh2ng.

set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -d "vendor/autossh" ] || [ ! -f "vendor/autossh/Cargo.toml" ]; then
  echo "vendor/autossh is missing — run 'git submodule update --init --recursive' first" >&2
  exit 1
fi

if [ -f "vendor/autossh/target/release/libautossh.a" ] && [ "${SSH2NG_FORCE_REBUILD:-}" != "1" ]; then
  echo "vendor/autossh/target/release/libautossh.a already built — skipping. SSH2NG_FORCE_REBUILD=1 to force."
  exit 0
fi

cargo build --release --manifest-path vendor/autossh/Cargo.toml -p ssh-stuck-detector
cargo build --release --manifest-path vendor/autossh/Cargo.toml -p autossh

echo "Built vendor/autossh/target/release/libautossh.a ($(stat -c %s vendor/autossh/target/release/libautossh.a) bytes)"
