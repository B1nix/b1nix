#!/bin/sh
# boot-timeline.sh — where a boot's seconds go, read off a serial log.
#
# Every line the kernel prints carries its uptime, so a log already contains
# the answer to "what took so long"; what it lacks is somewhere to look. This
# prints the milestones in order with the gap since the previous one, so the
# expensive step is the one with the large number beside it rather than
# something to be found by scrolling.
#
# Milestones are matched as fixed strings against the log. Anything absent is
# reported as missing rather than skipped silently: a stage that never ran is
# the most interesting result this can produce.
#
# Usage: sh tools/boot-timeline.sh [log ...]
#        sh tools/boot-timeline.sh smoke_run/kde9.log
set -u

# label<TAB>pattern. Order is the order they should occur in.
MILESTONES='
kernel entry	b1nix
physical memory	pmm:
kernel heap	kheap
PCI scan	pci:
filesystem	vfs:
block devices	blk:
graphics node	/dev/dri/card0 ready
init started	init: starting pid
session script	KDE: start
compositor up	KDE: ok host-compositor
message bus	KDE: ok session-bus
compositor socket	KDE: ok nested-socket
compositor alive	KDE: ok nested-alive
activity manager	KDE: ok activity-manager
desktop shell	KDE: ok plasmashell-alive
screenshot	KDE: SHOT
'

timeline() {
	log=$1
	[ -f "$log" ] || { echo "$log: no such log" >&2; return 1; }
	echo "=== $log ==="
	printf '%-22s %10s %10s\n' "milestone" "at (s)" "+since"
	prev=""
	echo "$MILESTONES" | while IFS='	' read -r label pat; do
		[ -z "${label:-}" ] && continue
		# The FIRST occurrence: a milestone is when a thing first happened.
		line=$(grep -a -m1 -F "$pat" "$log" 2>/dev/null)
		if [ -z "$line" ]; then
			printf '%-22s %10s %10s\n' "$label" "-" "(absent)"
			continue
		fi
		# Two timestamp shapes appear in one log: the kernel's "[   12.34]"
		# and the session script's own "t=12.34". Take whichever is there.
		t=$(printf '%s\n' "$line" | sed -n 's/^\[ *\([0-9][0-9]*\.[0-9]*\)\].*/\1/p')
		[ -z "$t" ] && t=$(printf '%s\n' "$line" | sed -n 's/.*t=\([0-9][0-9]*\.[0-9]*\).*/\1/p')
		[ -z "$t" ] && t="?"
		if [ "$t" = "?" ]; then
			printf '%-22s %10s %10s\n' "$label" "?" "-"
		elif [ -z "$prev" ]; then
			printf '%-22s %10s %10s\n' "$label" "$t" "-"
			prev=$t
		else
			d=$(awk -v a="$t" -v b="$prev" 'BEGIN{printf "%.2f", a-b}')
			printf '%-22s %10s %10s\n' "$label" "$t" "$d"
			prev=$t
		fi
	done
	echo
	# The frame instrument, if the run carried it.
	if grep -aq "gfx-prof:" "$log" 2>/dev/null; then
		echo "frame cost (b1nix.gfx-prof):"
		grep -a "gfx-prof:" "$log" | tail -3 | sed 's/^/  /'
	elif grep -aq "b1nix.gfx-prof" "$log" 2>/dev/null; then
		# Enabled and silent is a different fact from never asked for, and the
		# difference matters: it means frames were presented too rarely to
		# reach the instrument's reporting interval, or none were presented.
		echo "frame cost: instrument enabled but reported nothing"
		echo "  (presents are damage-driven; a quiet desktop may draw very few)"
	else
		echo "frame cost: not measured (boot without b1nix.gfx-prof)"
	fi
}

if [ $# -eq 0 ]; then
	echo "usage: sh tools/boot-timeline.sh <serial log> [...]" >&2
	exit 2
fi
for f in "$@"; do
	timeline "$f"
done
