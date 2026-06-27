#!/bin/sh
# Add b1nix support to wget's gnulib fpurge.c, freading.c, fseeko.c, getdtablesize.c.
set -eu
SRC_DIR="${1:?usage: b1nix-gnulib.sh <wget-src-dir>}"

FILE="$SRC_DIR/lib/fpurge.c"
if [ -f "$FILE" ] && ! grep -q 'defined b1nix' "$FILE"; then
  tmp="$FILE.tmp"
  sed 's/# else/# elif defined b1nix\n  fp->has_unget = 0;\n  return 0;\n# else/' "$FILE" > "$tmp"
  mv "$tmp" "$FILE"
fi

FILE="$SRC_DIR/lib/freading.c"
if [ -f "$FILE" ] && ! grep -q 'defined b1nix' "$FILE"; then
  tmp="$FILE.tmp"
  sed 's/# else/# elif defined b1nix\n  (void)fp;\n  return true;\n# else/' "$FILE" > "$tmp"
  mv "$tmp" "$FILE"
fi

FILE="$SRC_DIR/lib/fseeko.c"
if [ -f "$FILE" ] && ! grep -q 'defined b1nix' "$FILE"; then
  tmp="$FILE.tmp"
  sed -e 's/#elif defined EPLAN9.*/#elif defined b1nix\n  if (!fp->has_unget)\n#elif defined EPLAN9/' \
      -e 's/#elif defined __MINT__/#elif defined b1nix\n      fp->eof = 0;\n#elif defined __MINT__/' \
      "$FILE" > "$tmp"
  mv "$tmp" "$FILE"
fi

FILE="$SRC_DIR/lib/getdtablesize.c"
if [ -f "$FILE" ] && ! grep -q 'defined b1nix' "$FILE"; then
  tmp="$FILE.tmp"
  sed 's/#else/#elif defined b1nix\nint\ngetdtablesize (void)\n{\n  return 1024;\n}\n#else/' "$FILE" > "$tmp"
  mv "$tmp" "$FILE"
fi
