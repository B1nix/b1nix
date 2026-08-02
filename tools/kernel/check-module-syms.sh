#!/bin/sh
# check-module-syms.sh <module.ko>...
#
# Every undefined symbol of a built module must be resolvable at insmod time,
# either from the kernel's EXPORT_SYMBOL table (kernel/module/ksyms.c) or from
# another module's exports. Catching that here turns "the module fails to load
# at boot" into "the build fails", which is where an ABI mistake belongs.
set -eu

[ $# -gt 0 ] || exit 0

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NM="${NM:-$(command -v llvm-nm 2>/dev/null || command -v nm)}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Kernel exports, straight from the single source of truth.
sed -n 's/^EXPORT_SYMBOL(\([A-Za-z_][A-Za-z0-9_]*\));.*/\1/p' \
    "$ROOT/kernel/module/ksyms.c" | sort -u > "$TMP/known"

# Plus everything the modules themselves export.
for ko in "$@"; do
    "$NM" "$ko" | sed -n 's/.*__ksymname_//p' >> "$TMP/known"
done
sort -u "$TMP/known" -o "$TMP/known"

rc=0
for ko in "$@"; do
    "$NM" -u "$ko" | awk '{print $NF}' | sort -u > "$TMP/undef"
    missing="$(comm -23 "$TMP/undef" "$TMP/known")"
    if [ -n "$missing" ]; then
        echo "check-module-syms: $ko has unresolvable symbols:" >&2
        echo "$missing" | sed 's/^/    /' >&2
        echo "  add EXPORT_SYMBOL() for them in kernel/module/ksyms.c" >&2
        rc=1
    fi
done
exit $rc
