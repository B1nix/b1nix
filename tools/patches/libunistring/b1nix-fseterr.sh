#!/bin/sh
# Add b1nix (musl libc) support to libunistring's fseterr.c.
#
# musl's FILE is opaque in the public headers, so the field-poking branches
# gnulib ships for glibc/BSD/etc. do not compile. But musl's struct _IO_FILE
# begins with `unsigned flags;` and marks a stream error with the F_ERR bit
# (0x20) — setting that bit via a cast to the first field is exactly what
# musl's own ferror() tests. A real error flag, not a stub.
set -eu
SRC_DIR="${1:?usage: b1nix-fseterr.sh <libunistring-src-dir>}"
FILE="$SRC_DIR/lib/fseterr.c"
[ -f "$FILE" ] || exit 0
if ! grep -q 'defined b1nix' "$FILE"; then
  tmp="$FILE.tmp"
  sed 's/#elif 0                             \/\* unknown  \*\//#elif defined b1nix\n  *(unsigned *)(void *)fp |= 32;\n#elif 0                             \/\* unknown  \*\//' "$FILE" > "$tmp"
  mv "$tmp" "$FILE"
fi
