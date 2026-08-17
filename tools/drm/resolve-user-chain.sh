#!/bin/sh
# Turn the fault handler's "user call chain (rbp):" addresses into module+symbol.
#
# The kernel prints raw run-time addresses; the modules they belong to are in
# the same log, in the memory map the same report dumps. This reads that map,
# finds the module each address falls in, subtracts its load base, and asks the
# on-disk file what lives at that offset. Nothing here guesses: an address
# outside every mapped range is reported as unresolved rather than attributed
# to the nearest module.
#
# Usage: sh tools/drm/resolve-user-chain.sh smoke_run/i915-passthrough.log
set -eu

LOG="${1:-smoke_run/i915-passthrough.log}"
[ -f "$LOG" ] || { echo "no such log: $LOG" >&2; exit 1; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RFS="$ROOT/build/x86_64/rootfs"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# The map lines the fault report prints: "  0xSTART-0xEND perm name".
tr -d '\r' < "$LOG" |
	grep -aoE '0x[0-9a-f]{16}-0x[0-9a-f]{16} [rwxps-]+ [^ ]+$' |
	sort -u > "$TMP/map" || true

resolve() {
	addr="$1"
	a=$(printf '%d' "$addr" 2>/dev/null) || { echo "$addr ?"; return; }
	while read -r range perm name; do
		start="${range%%-*}"; end="${range##*-}"
		s=$(printf '%d' "$start"); e=$(printf '%d' "$end")
		[ "$a" -ge "$s" ] && [ "$a" -lt "$e" ] || continue
		off=$((a - s))
		# Find the module's own base: its lowest mapped range.
		base=$(awk -v n="$name" '$3==n {print $1}' "$TMP/map" |
			sed 's/-.*//' | sort | head -1)
		b=$(printf '%d' "$base")
		modoff=$((a - b))
		file=$(find "$RFS" -name "$name" -type f 2>/dev/null | head -1)
		sym=""
		if [ -n "$file" ]; then
			sym=$(llvm-symbolizer --obj="$file" "$modoff" 2>/dev/null |
				head -1 | grep -v '^??$' || true)
		fi
		printf '%s  %s+0x%x %s\n' "$addr" "$name" "$modoff" "$sym"
		return
	done < "$TMP/map"
	printf '%s  <unmapped>\n' "$addr"
}

tr -d '\r' < "$LOG" | grep -a 'user call chain' | sort -u | while read -r line; do
	echo "=== $line" | cut -c1-40
	echo "$line" | grep -oE '0x[0-9a-f]+' | while read -r a; do
		resolve "$a" | sed 's/^/  /'
	done
done
