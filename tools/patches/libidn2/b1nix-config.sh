#!/bin/sh
# Teach libidn2's config.sub about b1nix.
set -eu

SRC_DIR="${1:?usage: b1nix-config.sh <libidn2-src-dir>}"
CFG="$SRC_DIR/build-aux/config.sub"

if ! grep -q 'b1nix\*' "$CFG"; then
  tmp="$CFG.tmp"
  sed 's/twizzler\*/twizzler\* | b1nix\*/' "$CFG" > "$tmp"
  mv "$tmp" "$CFG"
  chmod +x "$CFG"
fi
