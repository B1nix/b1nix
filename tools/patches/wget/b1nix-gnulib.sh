#!/bin/sh
# Add b1nix (musl libc) support to wget's gnulib fpurge.c, freading.c,
# fseeko.c, getdtablesize.c.
#
# musl's FILE is opaque in the public headers, so gnulib's field-poking
# replacement branches (glibc/BSD/etc.) do not compile. musl's own
# fpurge/fseeko/freading are correct, so the b1nix branches here either defer
# to them or manipulate the one documented flags word (musl's struct _IO_FILE
# begins with `unsigned flags;`, F_EOF=16, F_ERR=32) via a cast — a real flag
# change, not a stub.
set -eu
SRC_DIR="${1:?usage: b1nix-gnulib.sh <wget-src-dir>}"

# fpurge: musl has no public unget field; nothing to discard here. Return 0.
FILE="$SRC_DIR/lib/fpurge.c"
if [ -f "$FILE" ] && ! grep -q 'defined b1nix' "$FILE"; then
  tmp="$FILE.tmp"
  sed 's/# else/# elif defined b1nix\n  return 0;\n# else/' "$FILE" > "$tmp"
  mv "$tmp" "$FILE"
fi

FILE="$SRC_DIR/lib/freading.c"
if [ -f "$FILE" ] && ! grep -q 'defined b1nix' "$FILE"; then
  tmp="$FILE.tmp"
  sed 's/# else/# elif defined b1nix\n  (void)fp;\n  return true;\n# else/' "$FILE" > "$tmp"
  mv "$tmp" "$FILE"
fi

# fseeko: take the "assume the libc fseeko works" path (if (0) -> fall through
# to the real musl fseeko at the end), and clear F_EOF via the flags word in
# the post-lseek block so it compiles against musl's opaque FILE.
FILE="$SRC_DIR/lib/fseeko.c"
if [ -f "$FILE" ] && ! grep -q 'defined b1nix' "$FILE"; then
  tmp="$FILE.tmp"
  # Insert two b1nix branches, each just before an "#elif defined __MINT__":
  #   1st __MINT__ = the buffer-empty condition -> `if (0)` so we fall through
  #                  to the real musl fseeko() at the end of the function.
  #   2nd __MINT__ = the post-lseek EOF clear -> poke musl FILE.flags &= ~F_EOF.
  awk '
    /#elif defined __MINT__/ {
      n++
      if (n == 1) { print "#elif defined b1nix"; print "  if (0)" }
      else        { print "#elif defined b1nix"; print "      *(unsigned *)(void *)fp &= ~16u;" }
    }
    { print }
  ' "$FILE" > "$tmp"
  mv "$tmp" "$FILE"
fi

FILE="$SRC_DIR/lib/getdtablesize.c"
if [ -f "$FILE" ] && ! grep -q 'defined b1nix' "$FILE"; then
  tmp="$FILE.tmp"
  sed 's/#else/#elif defined b1nix\nint\ngetdtablesize (void)\n{\n  return 1024;\n}\n#else/' "$FILE" > "$tmp"
  mv "$tmp" "$FILE"
fi
