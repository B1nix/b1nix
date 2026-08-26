#!/bin/sh
# The overnight loop: run the soak matrix, one boot at a time, until a deadline.
#
# Sequential on purpose. Two guests competing for the same eight host cores make
# every timing measurement inside them meaningless, and a hang under contention
# cannot be told from a hang caused by it. One at a time also keeps the arithmetic
# honest: "one failure in forty runs" means forty comparable runs.
#
# Usage:
#   sh tools/soak/soak-loop.sh [HH:MM]     # default 08:00 tomorrow-or-today
#
# Environment:
#   SOAK_SMPS      CPU counts to cycle through   (default "6 1 2 4 8")
#   SOAK_SCALES    work multipliers to cycle     (default "100 200")
#   SOAK_SECONDS   per-workload budget           (default 15)
#   SMOKE_EVERY    run the full smoke suite every N cycles (default 6; 0 = never)
#   OUT_DIR        where logs and the tally go   (default smoke_run/soak)
set -u

SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
ROOT_DIR="$(cd "$(dirname "$SELF")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/smoke_run/soak}"
mkdir -p "$OUT_DIR"

DEADLINE_HHMM="${1:-08:00}"
# The deadline is the next time that clock reading occurs.
DEADLINE=$(date -d "today $DEADLINE_HHMM" +%s 2>/dev/null)
[ -n "$DEADLINE" ] || { echo "cannot parse deadline '$DEADLINE_HHMM'" >&2; exit 2; }
if [ "$DEADLINE" -le "$(date +%s)" ]; then
	DEADLINE=$(date -d "tomorrow $DEADLINE_HHMM" +%s)
fi

SMPS="${SOAK_SMPS:-6 1 2 4 8}"
SCALES="${SOAK_SCALES:-100 200}"
SECS="${SOAK_SECONDS:-15}"
# The smoke suite builds from the tree, so it is off by default in frozen mode.
SMOKE_EVERY="${SMOKE_EVERY:-6}"
[ -n "${SOAK_FROZEN_DIR:-}" ] && [ -z "${SMOKE_EVERY_SET:-}" ] && SMOKE_EVERY=0

SUMMARY="$OUT_DIR/summary.txt"
TALLY="$OUT_DIR/results.tsv"

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$SUMMARY"; }

# Nothing else may be using the host's cores while a run is being timed.
stray_qemu() {
	pgrep -f 'qemu-system-x86_64.*b1nix-soak' >/dev/null 2>&1
}
kill_strays() {
	pkill -f 'qemu-system-x86_64.*b1nix-soak' 2>/dev/null
	sleep 1
}

log "soak loop starts, deadline $(date -d "@$DEADLINE" '+%Y-%m-%d %H:%M'), smps=[$SMPS] scales=[$SCALES] secs=$SECS"

# Images: frozen if a snapshot was named, built once otherwise.
#
# Frozen is the right default for an overnight run that shares the machine with
# somebody editing the tree — see tools/soak/freeze.sh. Every run then boots the
# same kernel, and the night's tally describes one system rather than a moving
# average of several.
if [ -n "${SOAK_FROZEN_DIR:-}" ]; then
	[ -f "$SOAK_FROZEN_DIR/root.ext4" ] ||
		{ log "no frozen snapshot at $SOAK_FROZEN_DIR"; exit 2; }
	export SOAK_FROZEN_DIR
	log "using frozen images at $SOAK_FROZEN_DIR ($(cat "$SOAK_FROZEN_DIR/VERSION" 2>/dev/null))"
elif ! make -C "$ROOT_DIR" --no-print-directory -j6 iso-soak SOAK_SPEC=mem > "$OUT_DIR/.build.log" 2>&1; then
	log "initial build FAILED; see $OUT_DIR/.build.log"
	tail -20 "$OUT_DIR/.build.log" | tee -a "$SUMMARY"
	exit 2
else
	log "image built"
fi

# Each workload in its own boot, so a hang names the subsystem it hung in, plus
# one combined boot where they interfere with each other — which is the shape
# the compositor failure appears in.
WORKLOADS="${SOAK_WORKLOADS:-mem vm cpu fd shm spawn disk net all}"
# The compositor run costs 150 s and fails the same way every time once its
# defect is known; SOAK_SKIP_GFX=1 spends that budget on the workloads that
# still carry information.
SKIP_GFX="${SOAK_SKIP_GFX:-0}"

cycle=0
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
	cycle=$((cycle + 1))
	for smp in $SMPS; do
		[ "$(date +%s)" -lt "$DEADLINE" ] || break
		for scale in $SCALES; do
			[ "$(date +%s)" -lt "$DEADLINE" ] || break
			for w in $WORKLOADS; do
				[ "$(date +%s)" -lt "$DEADLINE" ] || break
				kill_strays
				SMP="$smp" SOAK_SCALE="$scale" SOAK_SECONDS="$SECS" \
				TIMEOUT=240 OUT_DIR="$OUT_DIR" \
					sh "$ROOT_DIR/tools/soak/run-soak.sh" "$w" "c${cycle}-$w" \
					>> "$SUMMARY" 2>&1
			done
			# The compositor, on its own and with a bound of its own: it is the
			# one workload that has hung the whole guest, and a hang there must
			# not eat the budget the other eight are sharing.
			[ "$SKIP_GFX" = "1" ] && continue
			kill_strays
			SMP="$smp" SOAK_SCALE="$scale" SOAK_SECONDS="$SECS" \
			TIMEOUT=150 OUT_DIR="$OUT_DIR" \
				sh "$ROOT_DIR/tools/soak/run-soak.sh" gfx "c${cycle}-gfx" \
				>> "$SUMMARY" 2>&1
		done
	done

	if [ "$SMOKE_EVERY" != "0" ] && [ $((cycle % SMOKE_EVERY)) = 0 ] &&
	   [ "$(date +%s)" -lt "$DEADLINE" ]; then
		log "cycle $cycle: full smoke suite"
		kill_strays
		start=$(date +%s)
		if (cd "$ROOT_DIR" && sh tests/smoke.sh > "$OUT_DIR/smoke-c$cycle.log" 2>&1); then
			v=pass
		else
			v=fail
		fi
		printf '%s\tsmoke\t%s\tcycle=%s\t\t\t%ss\tsmoke-c%s.log\t%s\n' \
			"$(date +%Y-%m-%dT%H:%M:%S)" "$v" "$cycle" "$(( $(date +%s) - start ))" \
			"$cycle" "$(grep -aoE '[0-9]+ passed, [0-9]+ failed[^,]*' "$OUT_DIR/smoke-c$cycle.log" | tail -1)" \
			>> "$TALLY"
		log "cycle $cycle: smoke $v"
	fi

	# A running count, so the night can be read without parsing the tally.
	log "cycle $cycle complete: $(awk -F'\t' '$3=="pass"{p++} $3=="fail"{f++} $3=="panic"{x++} $3=="hang"{h++} $3=="noboot"{n++} END{printf "%d pass, %d fail, %d panic, %d hang, %d noboot", p, f, x, h, n}' "$TALLY" 2>/dev/null)"
done

kill_strays
log "soak loop done after $cycle cycles"
log "$(awk -F'\t' '$3!="pass" && $3!="" {print}' "$TALLY" 2>/dev/null | wc -l) non-pass results in $TALLY"
