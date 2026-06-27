#!/bin/sh
# Teach wget's build-aux/config.sub about b1nix.
set -eu
SRC_DIR="${1:?usage: b1nix-config-sub.sh <wget-src-dir>}"
CFG="$SRC_DIR/build-aux/config.sub"
[ -f "$CFG" ] || exit 0
if ! grep -q 'b1nix\*' "$CFG"; then
  tmp="$CFG.tmp"
  sed 's/twizzler\*/twizzler* | b1nix*/' "$CFG" > "$tmp"
  mv "$tmp" "$CFG"
  chmod +x "$CFG"
fi
