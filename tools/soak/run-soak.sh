#!/bin/sh
# One soak boot: build the cmdline into a small ISO, run it, grade the log.
#
# Plain QEMU, no VFIO, no privileges. The root filesystem is attached as a
# virtio disk with snapshot=on rather than carried inside the ISO, for two
# reasons: the boot no longer copies half a gigabyte into memory before the
# kernel starts, and a run cannot leave anything behind in the shared image —
# which is what makes it safe to run these back to back all night.
#
# Usage:
#   sh tools/soak/run-soak.sh <spec> [name]
#
# Environment:
#   SMP           guest CPUs                    (default 6)
#   MEM_MB        guest RAM                     (default 2048)
#   TIMEOUT       seconds before QEMU is killed (default 180)
#   MACHINE       QEMU machine model            (default pc)
#   SOAK_SECONDS  per-workload budget, guest side (default 20)
#   SOAK_SCALE    work multiplier, percent      (default 100)
#   SOAK_THREADS  threads per workload          (default: one per guest CPU)
#   OUT_DIR       where logs go                 (default smoke_run/soak)
set -u

SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
ROOT_DIR="$(cd "$(dirname "$SELF")/../.." && pwd)"
ARCH="${B1NIX_ARCH:-x86_64}"
BUILD="$ROOT_DIR/build/$ARCH"

SPEC="${1:-all}"
NAME="${2:-$SPEC}"
SMP="${SMP:-6}"
MEM_MB="${MEM_MB:-2048}"
TIMEOUT="${TIMEOUT:-180}"
SOAK_SECONDS="${SOAK_SECONDS:-20}"
SOAK_SCALE="${SOAK_SCALE:-100}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/smoke_run/soak}"
mkdir -p "$OUT_DIR"

STAMP="$(date +%H%M%S)"
LOG="$OUT_DIR/soak-$NAME-smp$SMP-$STAMP.log"

EXTRA="b1nix.soak-seconds=$SOAK_SECONDS b1nix.soak-scale=$SOAK_SCALE"

# A display for the workloads that want one.
#
# The compositor workload on the headless backend has no output to pace itself
# against, and wlroots then repaints as fast as it can — which is not what a
# desktop does and not what is worth measuring. A virtio-gpu gives it a real
# mode, a real page flip and a frame interval, which is also the shape a
# windowed application runs in.
SOAK_GPU_ARGS="${SOAK_GPU_ARGS-}"
if [ -z "${SOAK_GPU_ARGS+set}" ] || [ "${SOAK_GPU_ARGS}" = "" ]; then
	case "$SPEC" in
	*gfx*|all) SOAK_GPU_ARGS="${SOAK_NO_GPU:+}${SOAK_NO_GPU:-"-device virtio-gpu-pci"}" ;;
	esac
fi
[ -n "${SOAK_THREADS:-}" ] && EXTRA="$EXTRA b1nix.soak-threads=$SOAK_THREADS"
# Anything else the run wants on the kernel cmdline — diagnostic flags, mostly.
[ -n "${SOAK_CMDLINE_EXTRA:-}" ] && EXTRA="$EXTRA $SOAK_CMDLINE_EXTRA"

# The ISO carries the cmdline, so it is rebuilt whenever the spec changes. With
# no rootfs module to stage that is a couple of seconds; `iso-soak-quick` skips
# re-walking the staging tree, which the loop has already done once.
#
# Frozen mode. An overnight loop and a debugging session cannot share one build
# tree: the loop's next run would boot whatever kernel happened to be half
# written at that moment, and its results would describe no particular system.
# SOAK_FROZEN_DIR points at a snapshot made by tools/soak/freeze.sh — one ISO
# per workload plus the root image — and a run in that mode builds nothing.
if [ -n "${SOAK_FROZEN_DIR:-}" ] && [ -f "$SOAK_FROZEN_DIR/b1nix-soak-$SPEC.iso" ]; then
	ISO="$SOAK_FROZEN_DIR/b1nix-soak-$SPEC.iso"
	ROOT_IMG="$SOAK_FROZEN_DIR/root.ext4"
else
	BUILD_TARGET="${SOAK_BUILD_TARGET:-iso-soak-quick}"
	if ! make -C "$ROOT_DIR" --no-print-directory "$BUILD_TARGET" \
	        SOAK_SPEC="$SPEC" SOAK_EXTRA="$EXTRA" > "$OUT_DIR/.mkiso.log" 2>&1; then
		echo "soak: ISO build failed; see $OUT_DIR/.mkiso.log" >&2
		tail -20 "$OUT_DIR/.mkiso.log" >&2
		exit 2
	fi
	ISO="$BUILD/b1nix-soak.iso"
	ROOT_IMG="$BUILD/root.ext4"
	# Under a name of its own.
	#
	# The overnight loop clears strays with `pkill -f b1nix-soak`, and a probe
	# run booting an image of that name is killed by it mid-measurement — which
	# reads as a hang and is nothing of the sort. A probe image is a probe
	# image; the loop leaves it alone.
	if [ -n "${SOAK_PROBE:-}" ]; then
		mkdir -p "$OUT_DIR/iso"
		cp "$ISO" "$OUT_DIR/iso/b1nix-probe.iso"
		ISO="$OUT_DIR/iso/b1nix-probe.iso"
	fi
fi
[ -f "$ISO" ] || { echo "soak: no ISO at $ISO" >&2; exit 2; }
[ -f "$ROOT_IMG" ] || { echo "soak: no root image at $ROOT_IMG" >&2; exit 2; }

# snapshot=on: QEMU keeps every write in a temporary overlay it discards on
# exit. Without it a diskstress run would write into the image the next run
# boots from, and two concurrent runs would corrupt it outright.
#
# i440fx by default, and q35 works too. On q35 the ISO is attached to the ich9
# AHCI controller as an ATAPI device; the driver used to send it an IDENTIFY it
# cannot answer and stop dead after "port 2 ready", never reaching userspace.
# It now reads the port signature, identifies a packet device with the command
# meant for one, and bounds the probe — so MACHINE=q35 boots. i440fx remains the
# default because it is what the overnight matrix has been measured on.
#
# -cpu host,+invtsc — the guest needs a counter it can trust, or clock_gettime
# falls back to the 100 Hz tick and every measurement in these stressors gains
# a 10 ms granularity.
# A monitor socket, always. A guest that stops printing has not stopped
# running, and the only question worth asking it is where each CPU is. Killing
# it first throws that away, so the hang path below asks before it kills.
MON="$OUT_DIR/mon-$NAME-$STAMP.sock"
rm -f "$MON"

start=$(date +%s)
timeout "$TIMEOUT" qemu-system-x86_64 \
	-machine "${MACHINE:-pc}",accel=kvm \
	-monitor "unix:$MON,server,nowait" \
	-cpu host,+invtsc \
	-m "$MEM_MB" \
	-smp "$SMP" \
	-cdrom "$ISO" \
	-boot d \
	-drive file="$ROOT_IMG",format=raw,if=virtio,snapshot=on \
	-netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
	$SOAK_GPU_ARGS \
	-vga none -display none \
	-serial stdio \
	-no-reboot \
	> "$LOG" 2>&1 &
qemu_pid=$!

# Stop as soon as the guest says it is finished. Holding the machine to the
# timeout after that costs the whole budget per iteration and adds nothing.
waited=0
while kill -0 "$qemu_pid" 2>/dev/null && [ "$waited" -lt "$TIMEOUT" ]; do
	if grep -aq "SOAK: done" "$LOG" 2>/dev/null; then
		break
	fi
	if grep -aqE "KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then
		break
	fi
	sleep 1
	waited=$((waited + 1))
done

# ── where was it, if it was nowhere ──
# No verdict in the log and the machine still up means the guest is wedged.
# Ask the monitor for every vCPU's registers and turn each RIP into a symbol:
# a lockup is only actionable once you know which loop it is in.
if kill -0 "$qemu_pid" 2>/dev/null \
   && ! grep -aqE "SOAK: done|SOAK: FAILED|KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null \
   && [ -S "$MON" ]; then
	REGS="${LOG%.log}.regs"
	{
		printf 'info registers -a\ninfo cpus\ninfo mem\n'
		sleep 2
	} | socat - "UNIX-CONNECT:$MON" > "$REGS" 2>&1
	KELF="$BUILD/kernel.elf"
	if [ -s "$REGS" ] && [ -f "$KELF" ]; then
		echo "--- wedged: RIP per vCPU ---" >> "$REGS"
		grep -oE 'RIP=[0-9a-f]+' "$REGS" | sort -u | while read -r r; do
			a="0x${r#RIP=}"
			sym=$(llvm-addr2line -f -e "$KELF" "$a" 2>/dev/null | tr '\n' ' ')
			echo "  $a  $sym" >> "$REGS"
		done
	fi
	echo "soak $NAME: wedged, registers in $REGS"
fi
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
elapsed=$(( $(date +%s) - start ))

# ── grading ──
# A run is good only if the guest said so. Silence is a failure with a
# different name, not a pass: the marker is emitted last, so a log without it
# is a boot that died somewhere.
verdict="unknown"
detail=""
if grep -aqE "KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then
	verdict="panic"
	detail=$(grep -aE "KERNEL PANIC|\[PANIC\]" "$LOG" | head -1)
elif grep -aq "SOAK: FAILED" "$LOG" 2>/dev/null; then
	verdict="fail"
	detail=$(grep -a "SOAK: FAILED" "$LOG" | head -1)
elif grep -aq "SOAK: PASSED" "$LOG" 2>/dev/null; then
	verdict="pass"
	detail=$(grep -a "SOAK: PASSED" "$LOG" | head -1)
elif grep -aq "SOAK: start" "$LOG" 2>/dev/null; then
	verdict="hang"
	# The last `begin` with no matching verdict names the workload it died in.
	detail="last marker: $(grep -aE '^SOAK(-[A-Z]+)?: ' "$LOG" | tail -1)"
else
	verdict="noboot"
	detail="last line: $(tail -1 "$LOG" 2>/dev/null | tr -d '\r')"
fi

printf '%s\t%s\t%s\tsmp=%s\tscale=%s\tsecs=%s\t%ss\t%s\t%s\n' \
	"$(date +%Y-%m-%dT%H:%M:%S)" "$NAME" "$verdict" "$SMP" "$SOAK_SCALE" \
	"$SOAK_SECONDS" "$elapsed" "$(basename "$LOG")" "$detail" \
	>> "$OUT_DIR/results.tsv"

echo "soak $NAME smp=$SMP: $verdict in ${elapsed}s -> $LOG"
[ "$verdict" = "pass" ] && exit 0
exit 1
