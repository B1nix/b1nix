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
# Environment:
#   IGD_BDF     PCI address of the GPU        (default: autodetected)
#   MEM_MB      guest RAM                     (default: 2048)
#   TIMEOUT     seconds before QEMU is killed (default: 120)
#   MACHINE     legacy | q35                  (default: legacy)
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${B1NIX_ARCH:-x86_64}"
ISO="$ROOT_DIR/build/$ARCH/b1nix.iso"
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
case "$MACHINE" in
legacy)
	# -vga none and -nic none are required, not cosmetic: QEMU's emulated VGA
	# takes guest slot 2 and its default NIC takes the next free one, and slot 2
	# is where legacy IGD mode has to place the real device. Neither is wanted
	# here — this run is about the GPU.
	MACHINE_ARGS="-machine pc,accel=kvm -vga none -nic none"
	DEV_ARGS="-device vfio-pci,host=$IGD_BDF,addr=02.0,x-igd-opregion=on"
	;;
q35)
	MACHINE_ARGS="-machine q35,accel=kvm -vga none -nic none"
	DEV_ARGS="-device vfio-pci,host=$IGD_BDF"
	;;
*)
	echo "MACHINE must be 'legacy' or 'q35'" >&2
	exit 1
	;;
esac

echo "b1nix + $IGD_BDF via VFIO ($MACHINE), ${MEM_MB}M, log: $LOG"

set +e
timeout "$TIMEOUT" qemu-system-x86_64 \
	$MACHINE_ARGS \
	-m "$MEM_MB" \
	-cdrom "$ISO" \
	-boot d \
	$DEV_ARGS \
	-display none \
	-serial stdio \
	-no-reboot \
	> "$LOG" 2>&1
rc=$?
set -e

# 124 is timeout's own code: the guest was still running, which for a driver
# bring-up is a result rather than a failure.
if [ "$rc" = 124 ]; then
	echo "ran to the ${TIMEOUT}s limit (still alive)"
else
	echo "qemu exited $rc"
fi

echo
echo "── i915 and DRM lines from the guest ──"
grep -aiE "i915|drm|GuC|HuC|GT[0-9]|vfio" "$LOG" | head -60 || true
echo
echo "── panics, if any ──"
grep -aiE "PANIC|BUG|#GP|page fault" "$LOG" | head -20 || true
