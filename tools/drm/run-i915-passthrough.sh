#!/bin/sh
# Run b1nix with a real Intel GPU handed to it through VFIO.
#
# This is how the imported i915 is proved against hardware rather than against
# a model of it: QEMU maps the device's BARs and config space straight into the
# guest, so the driver in b1nix talks to the same silicon the host driver would.
#
# What this script does NOT do is take the GPU away from the host. Unbinding a
# device and binding it to vfio-pci needs root and is the one step that can
# disturb a running desktop, so it is left to the operator — `--preflight`
# prints exactly which commands to run, for this machine's GPU, and checks the
# result. Everything after that is unprivileged.
#
# Usage:
#   sh tools/run-i915-passthrough.sh --preflight     # what is missing, and how
#   sh tools/run-i915-passthrough.sh                 # run, once bound
#
# What the guest proves is decided by the cmdline baked into the ISO:
#   make B1NIX_I915=1 KERNEL_CMDLINE="b1nix.i915-gt-probe" iso
# adds the GT report — engines, submission method, GGTT/PPGTT, and one empty
# request per engine taken to retirement. That answers "did the GT come up"
# without inferring it from register dumps, and it is the step before iris:
# when a userspace driver fails, this says whether it failed on a GT that was
# already unable to retire an empty request.
#
# Environment:
#   IGD_BDF     PCI address of the GPU        (default: autodetected)
#   MEM_MB      guest RAM                     (default: 2048)
#   TIMEOUT     seconds before QEMU is killed (default: 120)
#   MACHINE     legacy | q35                  (default: legacy)
set -eu

SELF="$(readlink -f "$0" 2>/dev/null || readlink "$0" 2>/dev/null || echo "$0")"
SCRIPT_DIR="$(cd "$(dirname "$SELF")" && pwd)"
ROOT_DIR=""
cur="$SCRIPT_DIR"
while [ "$cur" != "/" ]; do
  if [ -f "$cur/Makefile" ] && [ -d "$cur/kernel" ]; then
    ROOT_DIR="$cur"
    break
  fi
  cur="$(dirname "$cur")"
done
[ -n "$ROOT_DIR" ] || ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARCH="${B1NIX_ARCH:-x86_64}"
ISO="${ISO:-$ROOT_DIR/build/$ARCH/b1nix.iso}"
OUT_DIR="$ROOT_DIR/smoke_run"
LOG="$OUT_DIR/i915-passthrough.log"
MEM_MB="${MEM_MB:-2048}"
TIMEOUT="${TIMEOUT:-120}"
MACHINE="${MACHINE:-legacy}"

# The Intel display controller, by class rather than by id: 0300 is a VGA
# controller and 0380 is a secondary display controller, and an iGPU that is not
# driving the console reports the latter.
detect_igd() {
	lspci -Dn | awk '$2 ~ /^03[08]0:/ && $3 ~ /^8086:/ { print $1; exit }'
}

IGD_BDF="${IGD_BDF:-$(detect_igd || true)}"
if [ -z "$IGD_BDF" ]; then
	echo "no Intel display controller found; set IGD_BDF=0000:00:02.0" >&2
	exit 1
fi

SYS="/sys/bus/pci/devices/$IGD_BDF"
[ -d "$SYS" ] || { echo "no such PCI device: $IGD_BDF" >&2; exit 1; }

current_driver() {
	if [ -e "$SYS/driver" ]; then
		basename "$(readlink -f "$SYS/driver")"
	else
		echo "(none)"
	fi
}

iommu_group() {
	[ -e "$SYS/iommu_group" ] && basename "$(readlink -f "$SYS/iommu_group")" || echo ""
}

vendor_device() {
	printf '%s %s\n' \
		"$(cut -c3- < "$SYS/vendor")" "$(cut -c3- < "$SYS/device")"
}

preflight() {
	group="$(iommu_group)"
	drv="$(current_driver)"
	echo "device      : $IGD_BDF  $(lspci -s "$IGD_BDF" | cut -d' ' -f2-)"
	echo "driver      : $drv"
	echo "iommu group : ${group:-none — is intel_iommu=on set?}"

	if [ -z "$group" ]; then
		echo
		echo "The IOMMU is not on. Add intel_iommu=on to the host kernel cmdline"
		echo "and reboot; without it VFIO cannot isolate the device and QEMU will"
		echo "refuse to map it."
		return 1
	fi

	# Every device in the group moves together — that is what a group means.
	echo "group members:"
	for d in /sys/kernel/iommu_groups/"$group"/devices/*; do
		bdf="$(basename "$d")"
		echo "  $bdf  $(lspci -s "$bdf" | cut -d' ' -f2- | cut -c1-60)"
	done

	# A display on this GPU means unbinding it blanks a screen.
	attached=""
	for st in /sys/class/drm/*/status; do
		[ -e "$st" ] || continue
		card="$(basename "$(dirname "$st")")"
		case "$card" in *-*) ;; *) continue ;; esac
		devlink="/sys/class/drm/${card%%-*}/device"
		[ -e "$devlink" ] || continue
		[ "$(basename "$(readlink -f "$devlink")")" = "$IGD_BDF" ] || continue
		[ "$(cat "$st")" = "connected" ] && attached="$attached $card"
	done
	if [ -n "$attached" ]; then
		echo
		echo "WARNING: displays are connected to this GPU:$attached"
		echo "Unbinding it will blank them. Move the console to another card first."
	else
		echo "displays    : none connected (safe to unbind)"
	fi

	# Anything holding the render node keeps the driver busy.
	if command -v fuser >/dev/null 2>&1; then
		holders="$(fuser /dev/dri/by-path/pci-$IGD_BDF-render 2>/dev/null || true)"
		if [ -n "$holders" ]; then
			echo
			echo "In use by PID(s):$holders — close them, or the unbind fails:"
			for p in $holders; do
				echo "  $p  $(ps -o comm= -p "$p" 2>/dev/null || true)"
			done
		fi
	fi

	if [ "$drv" = "vfio-pci" ]; then
		echo
		echo "Already bound to vfio-pci."
		node="/dev/vfio/$group"
		if [ -r "$node" ] && [ -w "$node" ]; then
			echo "$node is readable and writable. Ready."
		else
			echo "$node is not accessible to $(id -un). Run:"
			echo "  sudo setfacl -m u:$(id -un):rw $node"
		fi
		# And whether the image it will boot carries a userspace driver.
		# Without one the run can still prove the kernel side — modeset,
		# GT bring-up, `b1nix.i915-gt-probe` — but nothing in the guest
		# can render through the GPU, and that reads as a driver failure
		# when it is a missing package.
		iris="$ROOT_DIR/build/$ARCH/rootfs/usr/lib/xorg/modules/dri/iris_dri.so"
		if [ -e "$iris" ]; then
			echo "iris is in the image (userspace rendering available)."
		else
			echo "No iris in the image — kernel-side proof only. To add it:"
			echo "  make B1NIX_GPU_DRV=1 iso"
			echo "(184 MB: the Mesa megadriver and LLVM behind it.)"
		fi
		lim="$(ulimit -l)"
		if [ "$lim" != "unlimited" ] && [ "${lim:-0}" -lt $((MEM_MB * 1024)) ]; then
			echo
			echo "The locked-memory limit is ${lim} KiB, below the ${MEM_MB} MiB of guest"
			echo "RAM that VFIO has to pin. Raise it for this shell:"
			echo "  ulimit -l $((MEM_MB * 1024 + 65536))"
			echo "or add a limits.d entry for $(id -un) (memlock)."
		fi
		return 0
	fi

	set -- $(vendor_device)
	echo
	echo "To hand the device to VFIO, run:"
	echo "  sudo modprobe vfio-pci"
	echo "  echo $IGD_BDF | sudo tee $SYS/driver/unbind"
	echo "  echo $1 $2 | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id"
	echo "  sudo setfacl -m u:$(id -un):rw /dev/vfio/$group"
	echo
	echo "To give it back afterwards:"
	echo "  echo $1 $2 | sudo tee /sys/bus/pci/drivers/vfio-pci/remove_id"
	echo "  echo $IGD_BDF | sudo tee /sys/bus/pci/drivers/vfio-pci/unbind"
	echo "  echo $IGD_BDF | sudo tee /sys/bus/pci/drivers/i915/bind"
	return 1
}

if [ "${1:-}" = "--preflight" ]; then
	preflight
	exit $?
fi

drv="$(current_driver)"
if [ "$drv" != "vfio-pci" ]; then
	echo "$IGD_BDF is bound to '$drv', not vfio-pci." >&2
	echo "Run: sh tools/run-i915-passthrough.sh --preflight" >&2
	exit 1
fi
[ -f "$ISO" ] || { echo "no ISO at $ISO — run: make B1NIX_I915=1 iso" >&2; exit 1; }

mkdir -p "$OUT_DIR"

#
# A NIC at an address of its own.
#
# QEMU assigns the default NIC to the first free slot, which is the one legacy
# IGD mode needs — that collision is why the emulated devices were dropped
# entirely at first. Dropping the network instead makes the guest's network
# tests wait forever on a link that will never come up, which reads like a hang
# in the graphics run and is nothing of the sort. So it gets slot 3.
#
# virtio-net, not e1000.
#
# The emulated Intel card traps to the host for every packet; virtio-net hands
# it a ring and gets out of the way. On a run whose slowest part is downloading
# a couple of hundred megabytes of packages into the guest, that is the
# difference between minutes and tens of minutes — and this system has had a
# working virtio-net driver all along, the smoke suite uses it as its primary
# interface. B1NIX_NIC=e1000 restores the old card for a run that wants to
# exercise it.
NET_ARGS="-netdev user,id=n0 -device ${B1NIX_NIC:-virtio-net-pci},netdev=n0,addr=03.0"

#
# Machine model.
#
# "legacy" is QEMU's IGD passthrough mode: the device must sit at guest 00:02.0
# on an i440fx machine, and QEMU then also gives the guest the two things an
# Intel driver reads that are not in the device itself — the OpRegion, which
# carries the VBT describing how the panels and ports are wired, and the host
# bridge and LPC device ids the driver identifies the chipset from. Without
# those i915 probes and then has nothing to drive.
#
# "q35" is the plain assignment, for looking at register access alone.
#
# rombar=0 in both, and it is load-bearing rather than tidy: QEMU exposes the
# assigned card's video BIOS as an option ROM, the firmware runs it before the
# bootloader, and on this card it spins forever reading 0xffff. b1nix does not
# need it — i915 does its own modesetting and takes the VBT from the OpRegion,
# which QEMU supplies separately. With the ROM left on, the guest never reaches
# the bootloader at all: it sits in 16-bit real mode at CS=c000.
#
case "$MACHINE" in
legacy)
	# -vga none and -nic none are required, not cosmetic: QEMU's emulated VGA
	# takes guest slot 2 and its default NIC takes the next free one, and slot 2
	# is where legacy IGD mode has to place the real device. Neither is wanted
	# here — this run is about the GPU.
	MACHINE_ARGS="-machine pc,accel=kvm -vga none $NET_ARGS"
	DEV_ARGS="-device vfio-pci,host=$IGD_BDF,addr=02.0,x-igd-opregion=on,rombar=0"
	;;
q35)
	MACHINE_ARGS="-machine q35,accel=kvm -vga none $NET_ARGS"
	DEV_ARGS="-device vfio-pci,host=$IGD_BDF,rombar=0"
	;;
*)
	echo "MACHINE must be 'legacy' or 'q35'" >&2
	exit 1
	;;
esac

#
# A virtio-gpu alongside the assigned card, and a monitor socket to capture it.
#
# The assigned GPU scans out to a connector that is not plugged into anything,
# and QEMU cannot read an assigned device's framebuffer back — for a plain
# vfio-pci device it reports "doesn't support any (known) display method",
# because the gfx-plane query it wants exists only for mdev/vGPU. So the guest
# mirrors the frame onto this second, emulated display, and screendump captures
# that. See kernel/lkpi/drm_mirror.c.
SHOT="$OUT_DIR/i915-screen.ppm"
MON="$OUT_DIR/i915-monitor.sock"
rm -f "$SHOT" "$MON"
# NO_VIRTIO_GPU=1 leaves it out. A compositor enumerating two cards takes its
# multi-GPU path and asks the renderer for DMA-BUF formats, which software
# rendering does not have — so an emulated card meant only as a mirror stops the
# assigned one from being used at all.
[ "${NO_VIRTIO_GPU:-0}" = "1" ] ||
	DEV_ARGS="$DEV_ARGS -device virtio-gpu-pci"
MON_ARGS="-monitor unix:$MON,server,nowait"

# A disk that survives the run, holding downloaded Alpine packages.
#
# Every boot that installs a compositor spends minutes fetching the same
# megabytes again; bpkg keeps what it downloads under /var/cache/bpkg, and this
# is where that directory lives between runs. Created once, empty, and never
# read by anything but the guest.
CACHE_IMG="$OUT_DIR/pkgcache.img"
if [ ! -f "$CACHE_IMG" ]; then
	dd if=/dev/zero of="$CACHE_IMG" bs=1M count=512 2>/dev/null
	mke2fs -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q \
		-L b1nix-pkgcache "$CACHE_IMG" 2>/dev/null ||
		mke2fs -t ext4 -q -L b1nix-pkgcache "$CACHE_IMG"
fi
DEV_ARGS="$DEV_ARGS -drive file=$CACHE_IMG,format=raw,if=virtio"

# The root filesystem as a disk, when asked for it.
#
# An ISO that carries root.ext4 as a boot module makes the loader read the whole
# image — 1.5 GB for the browser build — off the emulated drive and copy it into
# memory before the kernel starts. The kernel prefers a disk labelled
# b1nix-root over the module, so the same file attached here is read on demand
# instead, and start-up stops paying for the parts no one touches.
if [ -n "${ROOT_IMG:-}" ]; then
	[ -f "$ROOT_IMG" ] || { echo "no such root image: $ROOT_IMG" >&2; exit 1; }
	DEV_ARGS="$DEV_ARGS -drive file=$ROOT_IMG,format=raw,if=virtio"
fi


echo "b1nix + $IGD_BDF via VFIO ($MACHINE), ${MEM_MB}M, log: $LOG"

set +e
# One CPU by default, for now.
#
# More would help — the guest is not only waiting on the network — but with
# four it panicked walking a kernel page directory whose entry pointed at a
# non-canonical address. The faulting address was the block cache's read-ahead
# buffer in the kernel arena, which every address space shares by pointer, so
# this is not a stale userspace translation: a frame that was live as a page
# table had been handed to a second owner, and the block layer was merely the
# first code to walk it. The same signature also appears in mprotect, in a run
# with no GPU at all. The writer is not identified yet, so SMP=<n> asks for
# more cores deliberately rather than getting the fault by surprise.
# -cpu host,+invtsc: the processor we are actually on, including the promise
# that its cycle counter runs at a constant rate.
#
# +invtsc has to be asked for by name. QEMU leaves CPUID 0x80000007:EDX[8]
# clear even under -cpu host because an invariant TSC blocks live migration —
# measured here as leaf7_edx=0x0 while the host itself reports constant_tsc.
# Without it the guest kernel cannot trust the counter and falls back to the
# 100 Hz tick, which gives clock_gettime a 10 ms resolution on hardware whose
# counter is perfectly good. These runs never migrate.
timeout "$TIMEOUT" qemu-system-x86_64 \
	$MACHINE_ARGS \
	-cpu host,+invtsc \
	-m "$MEM_MB" \
	-smp "${SMP:-1}" \
	-cdrom "$ISO" \
	-boot d \
	$DEV_ARGS \
	$MON_ARGS \
	-display none \
	-serial stdio \
	-no-reboot \
	> "$LOG" 2>&1 &
qemu_pid=$!

# Capture once the guest says it has mirrored a frame, or give up when QEMU
# does. Polling the log rather than sleeping a fixed time: the modeset happens
# whenever it happens, and a fixed wait either races it or wastes the run.
captured=0
finished=0
waited=0
while kill -0 "$qemu_pid" 2>/dev/null && [ "$waited" -lt "$TIMEOUT" ]; do
	if grep -aq "mirror: frame presented" "$LOG" 2>/dev/null; then
		if [ -S "$MON" ]; then
			printf 'screendump %s\n' "$SHOT" | timeout 10 socat - "unix-connect:$MON" >/dev/null 2>&1
			captured=1
		fi
		break
	fi
	# The compositor run says when it is finished, and there is nothing to be
	# gained by holding the machine to the timeout after that — see the
	# shutdown below for why it matters that this exit is a clean one.
	if grep -aq "I915-SWAY: done" "$LOG" 2>/dev/null; then
		finished=1
		break
	fi
	# A named failure ends the run immediately. Waiting out the timeout after
	# the guest has already said what went wrong costs the whole budget and
	# adds nothing to the log.
	if grep -aq "I915-SWAY: FAIL" "$LOG" 2>/dev/null; then
		echo "guest reported: $(grep -a 'I915-SWAY: FAIL' "$LOG" | tail -1)"
		finished=1
		break
	fi
	sleep 2
	waited=$((waited + 2))
done

# Ask the guest to power down rather than shooting it.
#
# b1nix's block cache is write-back, so a SIGTERM to QEMU discards whatever has
# not been flushed — which is how a run that downloaded thirty packages into the
# package-cache disk left it holding none of them and a corrupt filesystem
# besides. The monitor socket is already open for screendump; this is the same
# channel.
if [ "$finished" = 1 ] && [ -S "$MON" ]; then
	printf 'system_powerdown\n' | timeout 10 socat - "unix-connect:$MON" >/dev/null 2>&1
	i=0
	while kill -0 "$qemu_pid" 2>/dev/null && [ "$i" -lt 30 ]; do
		sleep 1
		i=$((i + 1))
	done
	kill "$qemu_pid" 2>/dev/null
fi

# Done as soon as the frame is captured. Waiting for the timeout after that
# costs the whole budget per iteration and tells us nothing new — the guest has
# already reported what it did.
if [ "$captured" = 1 ]; then
	# A short grace period first: the modeset commit runs after the frame is
	# presented, and killing the moment the capture lands would throw away its
	# result — which is the half of this that says whether the display pipeline
	# actually came up.
	sleep "${CAPTURE_GRACE:-15}"
	kill "$qemu_pid" 2>/dev/null
fi
wait "$qemu_pid" 2>/dev/null
rc=$?
set -e

# 124 is timeout's own code: the guest was still running, which for a driver
# bring-up is a result rather than a failure.
if [ "$rc" = 124 ]; then
	echo "ran to the ${TIMEOUT}s limit (still alive)"
else
	echo "qemu exited $rc"
fi

if [ "$captured" = 1 ] && [ -s "$SHOT" ]; then
	echo "screen captured: $SHOT"
else
	echo "no screen captured (the guest never reported a mirrored frame)"
fi

echo
echo "── i915 and DRM lines from the guest ──"
grep -aiE "i915|drm|GuC|HuC|GT[0-9]|vfio" "$LOG" | head -60 || true
echo
echo "── panics, if any ──"
grep -aiE "PANIC|BUG|#GP|page fault" "$LOG" | head -20 || true
