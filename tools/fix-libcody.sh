#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
. "$PROJECT_DIR/tools/toolchain-env.sh"
REAL="$TOOLCHAIN_BUILD_HOME"
LIBCODY="$REAL/src/gcc-13.2.0/libcody"

echo "--- Patching libcody: removing u8 string prefix ---"
# Find all .cc and .hh files and do an in-place replacement of u8"..." -> "..."
# This removes the char8_t prefix that breaks S2C() on C++20-default compilers (GCC 14+)
for f in "$LIBCODY"/*.cc "$LIBCODY"/*.hh; do
    [ -f "$f" ] || continue
    # Replace both u8" and u8' (literal form in source on disk)
    perl -i -pe 's/\bu8"/"/g' "$f"
    echo "  patched: $(basename $f)"
done

echo ""
echo "Verifying (should be empty):"
remaining=$(grep -rn 'u8"' "$LIBCODY" 2>/dev/null | wc -l)
if [ "$remaining" -gt 0 ]; then
    echo "WARNING: $remaining occurrences still remain:"
    grep -rn 'u8"' "$LIBCODY" | head -10
else
    echo "OK: no u8 string literals remain in libcody"
fi

echo ""
echo "--- Removing stale build-gcc ---"
rm -rf "$REAL/src/build-gcc"
echo "Done. Run g2 again."
