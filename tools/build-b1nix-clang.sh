#!/usr/bin/env bash
# Compatibility entry point for the real b1nix-native Clang build.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "$ROOT/tools/build-native-clang.sh" --b1nix-elf "$@"
