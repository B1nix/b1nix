#!/bin/sh
# Add b1nix support to libunistring's fseterr.c (fp->error = 1).
set -eu
SRC_DIR="${1:?usage: b1nix-fseterr.sh <libunistring-src-dir>}"
FILE="$SRC_DIR/lib/fseterr.c"
[ -f "$FILE" ] || exit 0
if ! grep -q 'defined b1nix' "$FILE"; then
  tmp="$FILE.tmp"
  sed 's/#elif 0                             \/\* unknown  \*\//#elif defined b1nix\n  fp->error = 1;\n#elif 0                             \/\* unknown  \*\//' "$FILE" > "$tmp"
  mv "$tmp" "$FILE"
fi
