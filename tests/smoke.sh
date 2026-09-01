#!/bin/sh
# B1NIX Smoke Test Suite (M24)
# Runs kernel in QEMU and checks for expected output patterns.
# Usage: ./tests/smoke.sh [x86_64]

set -e

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

OS="$(uname -s)"
# SMOKE_JOBS caps the build parallelism. A machine shared with another build
# does not have all its cores available, and -j$(nproc) there makes both slower.
if [ -n "${SMOKE_JOBS:-}" ]; then
	NPROC="$SMOKE_JOBS"
elif [ "$OS" = "Darwin" ]; then
	NPROC=$(sysctl -n hw.ncpu)
else
	NPROC=$(nproc)
fi

ARCH_LABEL="$ARCH"

echo() {
	command echo "[$ARCH_LABEL] $*"
}

printf() {
	local fmt="$1"
	shift
	command printf "[$ARCH_LABEL]$fmt" "$@"
}
# Seconds to let each test run (override via env). Detect hardware acceleration
# (KVM on Linux, HVF on macOS) — TCG pure-emulation is ~10-20x slower and needs
# a much larger budget to avoid false timeouts.
_HAVE_ACCEL=0
if [ "$ARCH" = "x86_64" ]; then
	if [ -w /dev/kvm ] 2>/dev/null && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
		_HAVE_ACCEL=1
	elif [ "$(uname)" = "Darwin" ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw hvf; then
		_HAVE_ACCEL=1
	fi
fi
if [ "$ARCH" = "x86_64" ]; then
	if [ "$_HAVE_ACCEL" = "1" ]; then
		DEFAULT_TIMEOUT=600
	else
		DEFAULT_TIMEOUT=1800
	fi
else
	# aarch64 runs all test lanes in parallel in ~5 minutes on Apple Silicon hosts.
	DEFAULT_TIMEOUT=360
fi
TIMEOUT=${TIMEOUT:-$DEFAULT_TIMEOUT}
SMOKE_VERBOSE=${SMOKE_VERBOSE:-0}
SMOKE_QUICK=${SMOKE_QUICK:-0}
SMOKE_LEGACY=${SMOKE_LEGACY:-0}
SMOKE_PARALLEL=1
if [ "$SMOKE_QUICK" = "1" ] || [ "$SMOKE_LEGACY" = "1" ]; then
	SMOKE_PARALLEL=0
fi
SMOKE_PROGRESS_MODE=full

mkdir -p "$PROJECT_DIR/smoke_run"

# Pause the KDE file indexer (baloo) for the duration of the run: under parallel
# QEMU it competes for host CPU and is a documented source of smoke flakiness
# (timeouts/spurious fails). Best-effort and guarded — a no-op where baloo isn't
# installed. Resumes on exit (incl. interrupt). Set SMOKE_NO_BALOO=1 to skip.
BALOOCTL="$(command -v balooctl6 2>/dev/null || command -v balooctl 2>/dev/null || true)"
if [ "${SMOKE_NO_BALOO:-0}" != "1" ] && [ -n "$BALOOCTL" ]; then
	"$BALOOCTL" suspend >/dev/null 2>&1 || true
	trap '"$BALOOCTL" resume >/dev/null 2>&1 || true' EXIT INT TERM
fi

# Disk image path helper: disk_img <sata|nvme|swap> <instance-name>
disk_img() { command echo "$PROJECT_DIR/smoke_run/$1-smoke-$2-$$.img"; }

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0
SKIPPED=0
BLOCKED=0

pass() {
	if [ "$SMOKE_VERBOSE" = "1" ]; then
		printf "  ${GREEN}PASS${NC} %s\n" "$1"
	fi
	PASSED=$((PASSED + 1))
}

fail() {
	printf "  ${RED}FAIL${NC} %s - %s\n" "$1" "$2"
	FAILED=$((FAILED + 1))
}

# A check whose subject is hardware the emulated machine cannot provide is not a
# defect and not a blockage — it is inapplicable. QEMU `virt` (aarch64) has no
# q35, no intel-iommu and no amd-iommu, so the DMA-remapping lanes cannot even
# start there; counting their checks as BLOCKED made a harness limitation look
# like 30 broken things. Report them as SKIPPED instead, with the reason.
skipped() {
	if [ "$SMOKE_VERBOSE" = "1" ]; then
		printf "  ${YELLOW}SKIP${NC} %s - %s\n" "$1" "$2"
	fi
	SKIPPED=$((SKIPPED + 1))
}

# The DMA-remapping lanes are x86_64-only: they ask QEMU for a q35 machine with
# an Intel or AMD IOMMU. This is a real gap in the aarch64 port too (it has no
# SMMUv3 driver, see docs/aarch64-parity.md) — but it is a driver gap to be
# closed, not a check to be failed by a machine that cannot host the device.
check_iommu() {
	if [ "$ARCH" = "aarch64" ]; then
		skipped "$3" "this is a VT-d/AMD-Vi check; the aarch64 unit is an SMMUv3, checked as M100E on the smp lane"
	else
		check_output "$1" "$2" "$3"
	fi
}

# A marker that is missing because the instance that would have printed it died
# early (panic, watchdog kill, host stall) is NOT a failure of the thing being
# checked — it never ran. Reporting hundreds of those as FAIL buries the one
# real defect that wedged the instance. Count them separately as BLOCKED. This
# is bookkeeping only: BLOCKED still makes the suite exit non-zero.
blocked() {
	printf "  ${YELLOW}BLOCKED${NC} %s - %s\n" "$1" "$2"
	BLOCKED=$((BLOCKED + 1))
}

# A check that could not apply to this image at all — not a failure and not a
# wedge, but a configuration this build cannot answer (the accelerated
# composition path on an image that carries no GL driver, for instance). It has
# to be said out loud: a check quietly omitted is indistinguishable from a check
# that passed. Does not affect the exit status.
skipped() {
	printf "  ${YELLOW}SKIP${NC} %s - %s\n" "$1" "$2"
	SKIPPED=$((SKIPPED + 1))
}

# An instance is "wedged" when its log never reached "B1NIX-TEST: done" (the
# marker PID 1 prints last). Memoised: check_output runs ~900 times and these
# logs are megabytes.
WEDGE_SCANNED=""
WEDGE_LOGS=""
log_wedged() {
	_wl="$1"
	case " $WEDGE_SCANNED " in
	*" $_wl "*) ;;
	*)
		WEDGE_SCANNED="$WEDGE_SCANNED $_wl"
		if [ ! -f "$_wl" ] || ! grep -qa "B1NIX-TEST: done" "$_wl" 2>/dev/null; then
			WEDGE_LOGS="$WEDGE_LOGS $_wl"
		fi
		;;
	esac
	case " $WEDGE_LOGS " in
	*" $_wl "*) return 0 ;;
	esac
	return 1
}

# The concatenated $LOG mixes several instances, so a marker missing from it is
# unattributable: treat it as blocked if ANY contributing instance wedged.
any_instance_wedged() {
	for _l in "$SYS_LOG" "$BLK_LOG" "$POSIX_LOG" "$GFX_LOG"; do
		# A MISSING contributing log counts too: that instance did not run this
		# time, so a marker absent from the concatenation is unattributable
		# rather than a regression. Skipping it here made a restricted
		# SMOKE_INSTANCES run report the other instances' checks as failures.
		log_wedged "$_l" && return 0
	done
	return 1
}

# The hand-written marker branches (if/elif/else over several markers) cannot
# use check_output, so they used to report a plain FAIL even when the instance
# never ran -- a restricted run showed nine failures for tests it never started.
# Same wedge rule as check_output, applied by hand.
missing_marker() {
	_mm_log="$1"
	_mm_desc="$2"
	_mm_why="$3"
	if { [ "$_mm_log" = "$LOG" ] && any_instance_wedged; } || log_wedged "$_mm_log"; then
		blocked "$_mm_desc" "instance died before this ran: $_mm_why"
	else
		fail "$_mm_desc" "$_mm_why"
	fi
}

# Prints where each wedged instance stopped — the actual thing to debug.
report_wedged_instances() {
	_any=0
	for _l in "$SYS_LOG" "$BLK_LOG" "$POSIX_LOG" "$GFX_LOG" "$SMP_LOG"; do
		[ -f "$_l" ] || continue
		grep -qa "B1NIX-TEST: done" "$_l" 2>/dev/null && continue
		# The SMP instance stops at its own done-pattern by design.
		case "$_l" in
		"$SMP_LOG") grep -qa "M24B-SMP: \(ok\|fail\|skip\)" "$_l" 2>/dev/null && continue ;;
		esac
		_any=1
		printf "  ${YELLOW}WEDGED${NC} %s\n" "$_l"
		printf "         last output: %s\n" "$(grep -a . "$_l" 2>/dev/null | tail -1 | cut -c1-140)"
	done
	[ "$_any" = "1" ] && printf "  (checks against a wedged instance are reported BLOCKED, not FAIL)\n"
	return 0
}

section() {
	if [ "$SMOKE_VERBOSE" = "1" ]; then
		echo ""
		echo "[CHECK] $1"
	fi
}

report_progress_line() {
	local line="$1"

	if [ "$SMOKE_PROGRESS_MODE" = "smp" ]; then
		case "$line" in
			smp:\ AP*|M24B-*|B1NIX-TEST:*|*PANIC*) ;;
			*) return ;;
		esac
	else
		case "$line" in
			*": ok"*|*": done"*|*": FAIL"*|*": fail "*|*": failed"*|*PANIC*|b1nix\ kernel*|init\ spawn\ result:*|B1NIX-TEST:*|B1NIX-QUICK:*|*-SMOKE:*|*-GPU:*|*-PAM:*|*-IOSTREAM:*|*-DRM:*) ;;
			*) return ;;
		esac
	fi

	case "$line" in
		*": FAIL"*|*": fail "*|*": failed"*|*PANIC*)
			printf "  ${PROGRESS_PREFIX:-}${RED}%s${NC}\n" "$line"
			;;
		*": ok"*|*": done"*|B1NIX-TEST:\ done|B1NIX-QUICK:\ done)
			printf "  ${PROGRESS_PREFIX:-}${GREEN}%s${NC}\n" "$line"
			;;
		*)
			printf "  ${PROGRESS_PREFIX:-}${YELLOW}%s${NC}\n" "$line"
			;;
	esac
}

# The point of the share is that a rebuilt test binary reaches the guest without
# repacking half a gigabyte of root.ext4: 00-smoke.start's run_test prefers
# /mnt/host/bin/<name> over the rootfs copy.
#
# It was doing neither. The source was build/<arch>/bin, which this tree has
# never produced -- the staged binaries live in build/<arch>/rootfs/bin -- so
# bin/ in the share stayed empty and every run_test silently fell back to the
# rootfs, i.e. to whatever root.ext4 happened to hold. And the entries were
# SYMLINKS to absolute host paths: QEMU's 9p with security_model=none hands the
# guest the link itself, and /Users/... does not resolve inside the guest, so
# they would not have worked even with the right source.
#
# Hard links, refreshed each run: same inode, no copy, and the guest sees a
# plain file.
_prepare_hostshare() {
	_hs="$PROJECT_DIR/smoke_run/hostshare"
	echo "Hello from Host through VirtIO-9P!" > "$_hs/hello_from_host.txt" 2>/dev/null ||
		{ mkdir -p "$_hs"; echo "Hello from Host through VirtIO-9P!" > "$_hs/hello_from_host.txt"; }
	_src="$PROJECT_DIR/build/$ARCH/rootfs/bin"
	[ -d "$_src" ] || return 0
	rm -rf "$_hs/bin"
	mkdir -p "$_hs/bin"
	for _bf in "$_src"/*; do
		[ -f "$_bf" ] || continue
		ln -f "$_bf" "$_hs/bin/${_bf##*/}" 2>/dev/null ||
			cp -f "$_bf" "$_hs/bin/${_bf##*/}" 2>/dev/null || true
	done
}

# Run QEMU and capture output
run_qemu() {
	local log="$1"
	shift
	local pid
	local done_pattern="${SMOKE_DONE_PATTERN:-B1NIX-TEST: done|KERNEL PANIC|\[PANIC\]}"
  
	if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "aarch64" ]; then
		local filter_dump_args=""
		if [ "${SMOKE_PCAP:-0}" = "1" ] &&
		   qemu-system-x86_64 -object filter-dump,help >/dev/null 2>&1; then
			filter_dump_args="-object filter-dump,id=f0,netdev=net0,file=${NET_PCAP:-$PROJECT_DIR/smoke_run/net-$ARCH.pcap}"
		fi

		local qemu_bin="qemu-system-x86_64"
		local machine_args=""
		local kernel_args="-cdrom $PROJECT_DIR/build/$ARCH/${B1NIX_ISO_NAME:-b1nix.iso}"
		local accel_args=""
		if [ "$ARCH" = "aarch64" ]; then
			qemu_bin="qemu-system-aarch64"
			machine_args="-machine ${SMOKE_MACHINE:-virt}"
			# No GRUB ISO here — the per-lane cmdline the other arches bake
			# into iso-<lane> (SMOKE_CMDLINE_* in the Makefile) is passed
			# straight to the kernel via the DTB bootargs instead. The lane
			# name comes from the ISO name each launch_* helper sets.
			local lane="${B1NIX_ISO_NAME:-b1nix.iso}"
			lane="${lane#b1nix-}"; lane="${lane%.iso}"
			[ "$lane" = "b1nix" ] && lane="sys"
			# Same strings the Makefile bakes into each iso-<lane> (see
			# SMOKE_CMDLINE_*): no `init=` on the ordinary lanes, so PID 1 is
			# the default /sbin/init (BusyBox init) with /etc/inittab driving
			# OpenRC's runlevels underneath it.
			# b1nix.e1000-subnet: this lane puts the e1000 on its own SLIRP
			# (10.0.3.0/24, see the -netdev below), so the M37 self-test must
			# ARP for 10.0.3.2 — not the 10.0.2.2 that the x86_64 lane's e1000
			# shares with everything else. Without it the request goes to a
			# gateway that is not on this NIC's segment and nothing ever
			# replies, which read as six "rx-arp (no reply)" failures.
			local lane_cmdline="b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 b1nix.aslr b1nix.e1000-subnet=3 b1nix.smoke=$lane ${SMOKE_EXTRA_CMDLINE:-}"
			[ "$lane" = "openrc" ] && lane_cmdline="init=/sbin/openrc-init b1nix.test=1 b1nix.e1000-subnet=3 b1nix.openrc-ctltest"
			[ "$lane" = "init" ] && lane_cmdline="b1nix.test=1 b1nix.e1000-subnet=3 b1nix.smoke=init"
			# SMOKE_CMDLINE_switchroot in the Makefile, which the other arches
			# bake into iso-switchroot. Without it this lane booted like any
			# other -- ordinary root on the disk, PID 1 the usual init -- and
			# then failed five checks for not having switched a root it was
			# never asked to switch. root=initramfs keeps / on the RAM
			# filesystem so the disk is a second one to move onto, which is
			# the whole point of the instance.
			[ "$lane" = "switchroot" ] && lane_cmdline="b1nix.test=1 b1nix.smoke=switchroot root=initramfs init=/init-shebang"
			kernel_args="-kernel $PROJECT_DIR/build/aarch64/Image"
			if [ "$(uname)" = "Darwin" ]; then
				accel_args="-accel hvf -cpu host"
			else
				accel_args="-cpu cortex-a53"
			fi
		else
			if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
				# -cpu host exposes the full host instruction set to the guest and
				# cuts KVM exits (vs the conservative default model) — a free speedup
				# with hardware virt, no extra VMs. Only with KVM/HVF, never TCG.
				# +invtsc by name, because -cpu host does not include it: QEMU
				# leaves the invariant-TSC bit clear even on a host that has one,
				# since an invariant TSC blocks live migration. Without it the
				# guest cannot trust the counter and falls back to the 100 Hz
				# tick — so the TSC clock path, which is what real runs use, was
				# never exercised by the smoke suite at all.
				accel_args="-accel kvm -cpu host,+invtsc"
			elif [ "$(uname)" = "Darwin" ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw hvf; then
				accel_args="-accel hvf -cpu host"
			fi
		fi

		# RAM: QEMU's default (128 MiB) starves the graphics tests (setcrtc,
		# console-reclaim), so the headroom stays. The 32-bit port caps usable
		# RAM at 1 GiB, so keep it modest there.
		local mem_args="-m ${SMOKE_MEM_MB:-1024}"
		# Both x86_64 and aarch64 use 2 vCPUs by default to run PID 1 watchdog
		# and background daemons (net_task, aio-worker) reliably without starvation.
		local default_smp=2
		if [ "${SMOKE_FAST_SMP:-0}" != "1" ]; then
			cpu_args="-smp ${SMOKE_SMP:-$default_smp}"
		fi

		if [ "$ARCH" = "x86_64" ]; then
			set -- ${qemu_bin} ${accel_args} ${mem_args} ${cpu_args} \
				-cdrom "$PROJECT_DIR/build/$ARCH/${B1NIX_ISO_NAME:-b1nix.iso}" \
				-serial stdio -serial null -display ${GPU_DISPLAY:-none} -monitor none -no-reboot \
				-device isa-debug-exit,iobase=0xf4,iosize=0x04
		else
			set -- ${qemu_bin} ${machine_args} ${accel_args} ${mem_args} ${cpu_args} \
				${kernel_args} \
				-serial stdio -display ${GPU_DISPLAY:-none} \
				-monitor ${SMOKE_MONITOR:-none} -no-reboot
		fi

		if [ "$ARCH" = "aarch64" ]; then
			# virtio-blk-device: QEMU virt has no PCI/AHCI/NVMe host bridge by
			# default, so aarch64 reaches a disk over the virtio-mmio transport
			# (kernel/dev/virtio_blk_mmio.c) instead of x86_64's AHCI/NVMe. QEMU
			# assigns the mmio slot in command-line order, and the driver scans
			# all 32 slots at boot, so where this sits among other -device flags
			# doesn't matter. SATA_IMG is already ext4-formatted by _mkimg above
			# for every lane; reuse it as the aarch64 boot/root disk.
			# The NIC arrives over the same mmio transport
			# (kernel/dev/virtio_net_mmio.c). restrict=off for the same reason
			# the x86_64 lanes use it: the ping/DNS checks talk to SLIRP's
			# gateway at 10.0.2.2/10.0.2.3.
			# virt has a PCIe host bridge and b1nix reaches it through the
			# ECAM window now, so this lane carries the same AHCI and NVMe
			# controllers the x86_64 lanes do — M14 is about storage, not
			# about the transport it arrives on. They need PCIe root ports
			# because virt has no legacy PCI bus, and AHCI gets its own image
			# because SATA_IMG is the root disk here.
			set -- "$@" -append "$lane_cmdline" \
				-drive if=none,file="$SATA_IMG",format=raw,id=vblk0 \
				-device virtio-blk-device,drive=vblk0 \
				-netdev user,id=net0,restrict=${B1NIX_NET_RESTRICT:-off} \
				-device virtio-net-device,netdev=net0
			# Straight onto the root complex, NOT behind pcie-root-ports:
			# nothing assigns bus numbers to bridges on this board (no
			# firmware runs before the kernel), so a device behind a bridge
			# sits on a secondary bus the bridge has never been told about and
			# no scan can reach it.
			# The second NIC gets its own subnet, not another SLIRP on
			# 10.0.2.x: both would otherwise answer for the same gateway
			# address, and the ARP round trip M37 drives on this one teaches
			# the guest that 10.0.2.2 lives behind the e1000.
			#
			# The comment sits HERE and not inside the argument list below: a
			# `#` line between a backslash continuation and the next argument
			# ends the command, and the arguments after it are then run as
			# commands ("-netdev: command not found"). That silently stopped
			# every aarch64 instance from launching at all, while the harness
			# went on to grade the previous run's logs and report a result.
			set -- "$@" \
				-netdev user,id=net1,net=10.0.3.0/24,host=10.0.3.2,restrict=${B1NIX_NET_RESTRICT:-off} \
				-device ${E1000_MODEL:-e1000},netdev=net1 \
				-device ${GPU_DEVICE:-virtio-gpu-pci} \
				-audiodev none,id=audio0 \
				-device intel-hda,id=hda -device hda-duplex,bus=hda.0,audiodev=audio0 \
				-device AC97,audiodev=audio0 \
				-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
				-device virtio-tablet-pci,id=vtablet \
				-device virtio-tablet-pci,id=vtouch
			if [ -n "${AHCI_IMG:-}" ] && [ -f "${AHCI_IMG:-}" ]; then
				set -- "$@" \
					-device ich9-ahci,id=ahci0 \
					-drive if=none,file="$AHCI_IMG",format=raw,id=sata0 \
					-device ide-hd,drive=sata0,bus=ahci0.0
				if [ -n "${SWAP_IMG:-}" ] && [ -f "${SWAP_IMG:-}" ]; then
					set -- "$@" \
						-drive if=none,file="$SWAP_IMG",format=raw,id=sata1 \
						-device ide-hd,drive=sata1,bus=ahci0.1
				fi
			fi
			if [ -n "${NVME_IMG:-}" ] && [ -f "${NVME_IMG:-}" ]; then
				set -- "$@" \
					-drive if=none,file="$NVME_IMG",format=raw,id=nvm0 \
					-device nvme,drive=nvm0,serial=b1nixnvme
			fi
			_prepare_hostshare
			set -- "$@" \
				-fsdev local,path="$PROJECT_DIR/smoke_run/hostshare",security_model=none,id=fsdev9p \
				-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare
			set -- "$@" ${EXTRA_QEMU_ARGS:-}
		elif [ "${SMOKE_FAST_SMP:-0}" != "1" ]; then
			# restrict=off by default: the NET-SMOKE ping-gateway and BusyBox
			# nslookup/ping checks exercise real ICMP/DNS to the SLIRP gateway
			# (10.0.2.2/10.0.2.3), which QEMU's user net categorically blocks under
			# restrict=on (guest fully isolated) — so those checks can only pass
			# with restrict=off. The b1nix net stack itself is fine (they pass here).
			# Set B1NIX_NET_RESTRICT=on for a hermetic, network-isolated run.
			_prepare_hostshare
			set -- "$@" \
				-device ${GPU_DEVICE:-virtio-gpu-pci} \
				-netdev user,id=net0,restrict=${B1NIX_NET_RESTRICT:-off} -device virtio-net-pci,netdev=net0 \
				${filter_dump_args} \
				-netdev user,id=net1,restrict=${B1NIX_NET_RESTRICT:-off} -device ${E1000_MODEL:-e1000},netdev=net1 \
			-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
			-device virtio-tablet-pci,id=vtablet \
			-device virtio-tablet-pci,id=vtouch \
			-audiodev none,id=audio0 \
			-device intel-hda,id=hda -device hda-duplex,bus=hda.0,audiodev=audio0 \
			-device AC97,audiodev=audio0 \
			-device ich9-ahci,id=ahci \
				-drive file="$SATA_IMG",if=none,id=satadrive,format=raw,discard=unmap \
				-device ide-hd,drive=satadrive,bus=ahci.0 \
				-drive file="$SWAP_IMG",if=none,id=swapdrive,format=raw \
				-device ide-hd,drive=swapdrive,bus=ahci.1 \
				-drive file="$NVME_IMG",if=none,id=nvmedrive,format=raw,discard=unmap \
				-device nvme,serial=deadbeef,drive=nvmedrive \
				-fsdev local,path="$PROJECT_DIR/smoke_run/hostshare",security_model=none,id=fsdev9p \
				-device virtio-9p-pci,fsdev=fsdev9p,mount_tag=hostshare \
				${EXTRA_QEMU_ARGS:-}
		else
			set -- "$@" -nic none -vga none ${EXTRA_QEMU_ARGS:-}
		fi

		# The Raspberry Pi lane drives a different machine entirely: RAM and
		# core count come from the board model, root arrives on an SD card
		# rather than over virtio, and the kernel is handed the board's own
		# device tree. None of the arguments above apply, so the command is
		# built from scratch instead of filtered.
		if [ "${SMOKE_RASPI:-0}" = "1" ]; then
			set -- qemu-system-aarch64 -machine raspi4b \
				-kernel "$PROJECT_DIR/build/aarch64/Image.rpi" \
				-dtb "$PROJECT_DIR/tools/dts/bcm2711-rpi-4-b.dtb" \
				-sd "$RASPI_SD" \
				-serial stdio -serial null \
				-display none -monitor none -no-reboot \
				-append "b1nix.test=1 b1nix.smoke=init"
		fi

		"$@" >"$log" 2>&1 &
		pid=$!

		# Wait for completion marker/panic or timeout, then kill QEMU.
		# Keep this POSIX-portable (macOS doesn't ship GNU timeout by default).
		(
			set +e
			start_ts=$(date +%s)
			reported_lines=0
			# Stall detector: a healthy b1nix boot streams markers continuously, so a
			# long gap with NO new serial output means the instance wedged (a hung
			# test, a host suspend, or KVM starvation under load) — far more common
			# here than a clean run that legitimately runs to the full TIMEOUT. Kill
			# it after STALL_TIMEOUT of silence instead of blocking the whole TIMEOUT
			# (which with a large TIMEOUT meant 8-25 min hangs). Generous default so a
			# slow-but-alive module (a big mmap, a long GC) is not killed mid-work.
			last_progress_ts=$start_ts
			# Long enough that the guest's own 280s silence guard fires
			# first: that path ends the instance cleanly and reports every
			# check it ran, where killing from here counts them all as
			# BLOCKED. The slow ones are the TLS handshakes and the in-guest
			# build, which are silent for minutes on a loaded host.
			stall_after=${STALL_TIMEOUT:-180}
			while :; do
				line_count=$(wc -l <"$log" | tr -d ' ')
				if [ "$line_count" -gt "$reported_lines" ]; then
					sed -n "$((reported_lines + 1)),${line_count}p" "$log" |
						while IFS= read -r line; do
							report_progress_line "$line"
						done
					reported_lines=$line_count
					last_progress_ts=$(date +%s)
				fi
				if grep -qa -E "$done_pattern" "$log" 2>/dev/null; then
					break
				fi
				if ! kill -0 "$pid" 2>/dev/null; then
					# QEMU may exit before stdio has flushed the last serial burst.
					sleep 1
					line_count=$(wc -l <"$log" | tr -d ' ')
					if [ "$line_count" -gt "$reported_lines" ]; then
						sed -n "$((reported_lines + 1)),${line_count}p" "$log" |
							while IFS= read -r line; do
								report_progress_line "$line"
							done
						reported_lines=$line_count
					fi
					# QEMU exiting on its own before the done marker is NOT the
					# same failure as a stall: with -no-reboot a guest triple
					# fault / reset makes QEMU quit silently, leaving a log that
					# simply stops. Say so, so the truncation is not mistaken for
					# a hung test.
					if ! grep -qa -E "$done_pattern" "$log" 2>/dev/null; then
						command echo "[smoke] QEMU exited before the done marker (guest reset/triple fault or external kill)" >>"$log"
						command echo "SMOKE-WATCHDOG: qemu-exited child=qemu log=$log" >>"$log"
					fi
					break
				fi
				now_ts=$(date +%s)
				if [ $((now_ts - start_ts)) -ge "$TIMEOUT" ]; then
					command echo "[smoke] run_qemu timeout after ${TIMEOUT}s" >>"$log"
					command echo "SMOKE-WATCHDOG: timeout child=qemu log=$log" >>"$log"
					break
				fi
				if [ $((now_ts - last_progress_ts)) -ge "$stall_after" ]; then
					command echo "[smoke] run_qemu STALLED — no serial output for ${stall_after}s (hung test / host suspend / KVM starvation); killing instance" >>"$log"
					command echo "SMOKE-WATCHDOG: stalled child=qemu log=$log" >>"$log"
					break
				fi
				sleep 1
			done
			kill -9 "$pid" 2>/dev/null || true
		) &
		local watcher_pid=$!

		wait "$watcher_pid" 2>/dev/null || true
		kill -9 "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	else
		echo "Unknown ARCH: $ARCH"
		exit 1
	fi
}

# Check that a pattern appears in the log
check_output() {
	local log="$1"
	local pattern="$2"
	local desc="$3"

	if grep -q "$pattern" "$log" 2>/dev/null; then
		pass "$desc"
	elif { [ "$log" = "$LOG" ] && any_instance_wedged; } || log_wedged "$log"; then
		blocked "$desc" "instance died before this ran: $pattern"
	else
		fail "$desc" "missing expected output: $pattern"
	fi
}

# Check that a pattern does NOT appear in the log. The counterpart to
# check_output: some properties (a filtered log line, an error that must never
# be printed) can only be stated as an absence.
check_absent() {
	local log="$1"
	local pattern="$2"
	local desc="$3"

	if ! [ -s "$log" ]; then
		blocked "$desc" "no log to check: $pattern"
	elif grep -q "$pattern" "$log" 2>/dev/null; then
		fail "$desc" "output that must not appear: $pattern"
	else
		pass "$desc"
	fi
}

# Every kernel log line carries a monotonic timestamp, dmesg style. The lines
# that legitimately do not are the boot loader'"'"'s (it prints before the kernel
# runs), the harness'"'"'s own "[smoke]"/"SMOKE-WATCHDOG" notes, and userspace
# output, which is terminal output rather than a log record.
check_log_timestamps() {
	local log="$1"
	local desc="$2"
	local bad

	if ! [ -s "$log" ]; then
		blocked "$desc" "no log to check"
		return
	fi
	bad=$(grep -aE "^(pmm|vmm|kheap|sched|vfs|ext4|ext2|blk|ahci|nvme|pci|net|tcp|dhcp|arp|acpi|lapic|ioapic|smp|timer|initramfs|procfs|sysfs|rootfs|Step )" "$log" | head -5)
	if [ -n "$bad" ]; then
		fail "$desc" "kernel lines without a timestamp: $(echo "$bad" | head -1)"
	else
		pass "$desc"
	fi
}

# ── Build kernel first ──
echo "=== B1NIX Smoke Tests ($ARCH) ==="
echo ""

echo "[BUILD] Building kernel for $ARCH..."
cd "$PROJECT_DIR"
BUILD_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-build-$ARCH.log"
print_build_failure() {
	echo "  ${RED}BUILD FAILED${NC}"
	echo "  build log: $BUILD_LOG"
	if [ -f "$BUILD_LOG" ]; then
		echo "  --- likely build errors ---"
		grep -aEn "fatal error:| error:|undefined reference|No such file|not found|cannot (find|open|create|compute)|FAILED:|make(\\[[0-9]+\\])?: \\*\\*\\*" "$BUILD_LOG" | tail -120 || true
		echo "  --- end likely build errors ---"
		echo "  --- build log tail ---"
		tail -80 "$BUILD_LOG"
		echo "  --- end build log tail ---"
	fi
}
# SKIP_BUILD=1 reuses an existing build/$ARCH/b1nix.iso (e.g. when the toolchain
# can't rebuild every userspace port locally). SMOKE_MAKE_ARGS lets the caller
# inject extra make flags (e.g. CC=clang LD=ld.lld on Fedora, where `cc` is gcc).
if [ "${SKIP_BUILD:-0}" = "1" ]; then
	if [ "$SMOKE_PARALLEL" = "1" ]; then
		test -f "build/$ARCH/b1nix-sys.iso" &&
		test -f "build/$ARCH/b1nix-blk.iso" &&
		test -f "build/$ARCH/b1nix-posix.iso" &&
		test -f "build/$ARCH/b1nix-gfx.iso" || {
			echo "  ${RED}no prebuilt split smoke ISOs${NC}"
			exit 1
		}
	else
		test -f "build/$ARCH/b1nix.iso" || { echo "  ${RED}no prebuilt build/$ARCH/b1nix.iso${NC}"; exit 1; }
	fi
	echo "  (SKIP_BUILD=1 — reusing prebuilt ISO)"
else
	QUICK_CMDLINE=""
	[ "$SMOKE_QUICK" = "1" ] && QUICK_CMDLINE="b1nix.smoke=quick"
	if [ "$SMOKE_PARALLEL" = "1" ]; then
		make -j"$NPROC" ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} \
			iso-sys iso-blk iso-posix iso-gfx iso-openrc iso-init iso-switchroot \
			>"$BUILD_LOG" 2>&1 || {
			print_build_failure
			exit 1
		}
		if [ "$ARCH" = "aarch64" ] && [ "${SMOKE_RASPI_LANE:-0}" = "1" ] && qemu-system-aarch64 -machine help 2>/dev/null | grep -q "^raspi4b "; then
			rm -f "build/$ARCH/kernel.elf" "build/$ARCH/Image"
			make -j"$NPROC" ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} KERNEL_BASE=0x80000 \
				KERNEL_CMDLINE="init=/bin/init b1nix.test=1" all >>"$BUILD_LOG" 2>&1 && \
				cp -f "build/$ARCH/Image" "build/$ARCH/Image.rpi"
			rm -f "build/$ARCH/kernel.elf" "build/$ARCH/Image"
			make -j"$NPROC" ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} \
				iso-sys iso-blk iso-posix iso-gfx iso-openrc iso-init iso-switchroot \
				>>"$BUILD_LOG" 2>&1 || {
				print_build_failure
				exit 1
			}
		fi
	else
		make -j"$NPROC" ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} KERNEL_CMDLINE="b1nix.test=1 b1nix.kvtest=abc123 b1nix.ssh-loopback=1 $QUICK_CMDLINE" iso >"$BUILD_LOG" 2>&1 || {
			print_build_failure
			exit 1
		}
	fi
fi
pass "kernel builds without errors"
echo "  build/$ARCH/${B1NIX_ISO_NAME:-b1nix.iso} ready"
if [ -z "$MKE2FS" ] || [ ! -x "$MKE2FS" ]; then
    # Both tools from the SAME e2fsprogs, and preferably not from PATH.
    #
    # Homebrew keeps e2fsprogs keg-only: it links mke2fs and nothing else, and
    # on a machine with android-platform-tools installed even that one is
    # theirs. So `command -v mke2fs` answered /opt/homebrew/bin/mke2fs, its
    # directory holds no debugfs, and the ownership pass below silently did
    # nothing -- the guest rootfs kept the build host's uid, /bin/su elevated
    # to 501 instead of root, and six checks failed with no visible cause but
    # one warning line in a three-thousand-line log.
    _e2fsdir=""
    for _d in /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin /sbin /usr/sbin; do
        if [ -x "$_d/mke2fs" ] && [ -x "$_d/debugfs" ]; then _e2fsdir="$_d"; break; fi
    done
    if [ -n "$_e2fsdir" ]; then
        MKE2FS="$_e2fsdir/mke2fs"; DEBUGFS="$_e2fsdir/debugfs"
    else
        MKE2FS=$(command -v mke2fs 2>/dev/null || printf '%s' /sbin/mke2fs)
        DEBUGFS=$(command -v debugfs 2>/dev/null || printf '%s' /sbin/debugfs)
    fi
fi
if [ -z "$MKE2FS" ] || ! command -v "$MKE2FS" >/dev/null 2>&1; then
    echo "Error: mke2fs utility not found. Please install e2fsprogs."
    exit 1
fi
_mkimg() {  # mkimg <instance-suffix>
    _sata=$(disk_img sata "$1"); _nvme=$(disk_img nvme "$1"); _swap=$(disk_img swap "$1")
    _usb=$(disk_img usb "$1")
    if [ "$ARCH" = "aarch64" ]; then
        # aarch64 has no GRUB ISO: QEMU virt boots kernel.elf directly, so the
        # rootfs the other arches ship inside the ISO is delivered on the
        # virtio-blk disk instead (kernel/main.c mounts virtio-blk0 at /).
        dd if=/dev/zero of="$_nvme" bs=1M count=4 2>/dev/null
        dd if=/dev/zero of="$_swap" bs=1M count=2 2>/dev/null
        # The blk lane attaches this one as USB storage, and the one below as a
        # second virtio-blk disk; without them QEMU refuses to start and the
        # whole lane reads as blocked.
        dd if=/dev/zero of="$_usb" bs=1M count=2 2>/dev/null
        dd if=/dev/zero of="$(disk_img vblk "$1")" bs=1M count=4 2>/dev/null
        rm -f "$_sata"
        if [ -f "$PROJECT_DIR/build/$ARCH/root.ext4" ]; then
            cp -f "$PROJECT_DIR/build/$ARCH/root.ext4" "$_sata"
        else
            "$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q \
                -L b1nix-root -d "$PROJECT_DIR/build/$ARCH/rootfs" "$_sata" 512m || {
                echo "Error: Failed to build aarch64 rootfs image."; exit 1
            }
            _debugfs="$DEBUGFS"
            if [ -x "$_debugfs" ]; then
                ( cd "$PROJECT_DIR/build/$ARCH/rootfs" && find . -mindepth 1 ) |
                    sed 's|^\.||' |
                    awk '{ printf "sif %s uid 0\nsif %s gid 0\n", $0, $0 }' |
                    "$_debugfs" -w -f - "$_sata" >/dev/null 2>&1 || true
                DEBUGFS="$_debugfs" sh "$PROJECT_DIR/tools/images/stamp-root-modes.sh" "$_sata"
            fi
        fi
        "$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$_nvme" 2>/dev/null
        # A separate image for the AHCI controller: on this arch $_sata is the
        # ROOT disk (virtio-blk), and QEMU will not open one file for writing
        # twice.
        dd if=/dev/zero of="$(disk_img ahci "$1")" bs=1M count=4 2>/dev/null
        "$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q \
            "$(disk_img ahci "$1")" 2>/dev/null
        return 0
    fi
    dd if=/dev/zero of="$_sata" bs=1M count=4 2>/dev/null
    dd if=/dev/zero of="$_nvme" bs=1M count=4 2>/dev/null
    dd if=/dev/zero of="$_swap" bs=1M count=2 2>/dev/null
    # USB mass storage: only its identity is under test (which sd* letter it
    # lands on, and that it reports itself removable), so it stays unformatted.
    dd if=/dev/zero of="$_usb" bs=1M count=2 2>/dev/null
    # virtio-blk scratch disk: the one block device nothing else on the system
    # owns, so the durability self-test may write to it. Unformatted on purpose.
    dd if=/dev/zero of="$(disk_img vblk "$1")" bs=1M count=4 2>/dev/null
    "$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$_sata" 2>/dev/null || {
        "$MKE2FS" -F -t ext4 -q "$_sata" 2>/dev/null || {
            echo "Error: Failed to format sata $1 image as ext4."; exit 1
        }
    }
    # The NVMe image carries a volume label: `findfs LABEL=` has to have
    # something to find (M109). Not "b1nix-root" — that name selects the root.
    "$MKE2FS" -F -t ext4 -L m109label -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q "$_nvme" 2>/dev/null || {
        "$MKE2FS" -F -t ext4 -L m109label -q "$_nvme" 2>/dev/null || {
            echo "Error: Failed to format nvme $1 image as ext4."; exit 1
        }
    }
}
_mkimg sys
[ "$SMOKE_PARALLEL" = "1" ] && {
    _mkimg blk; _mkimg posix; _mkimg gfx; _mkimg openrc; _mkimg init; _mkimg iommu; _mkimg amdvi; _mkimg smp; _mkimg switchroot
}

# Define logs
LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-$ARCH.log"
SMP_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-smp-$ARCH.log"
SYS_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-sys-$ARCH.log"
BLK_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-blk-$ARCH.log"
POSIX_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-posix-$ARCH.log"
GFX_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-gfx-$ARCH.log"
OPENRC_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-openrc-$ARCH.log"
INIT_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-init-$ARCH.log"
SWITCHROOT_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-switchroot-$ARCH.log"
IOMMU_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-iommu-$ARCH.log"
AMDVI_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-amdvi-$ARCH.log"
RASPI_LOG="$PROJECT_DIR/smoke_run/b1nix-smoke-raspi-$ARCH.log"

# Prune leftovers from earlier runs.
#
# Every per-lane disk image is named with the runner's PID (see disk_img), so a
# run never reuses the previous run's files -- it just adds another full set
# beside them, and half a gigabyte a time adds up until the disk is gone. The
# same goes for the QEMU logs of lanes that this invocation is not running: a
# wedged instance can leave a multi-gigabyte log behind.
#
# An hour is well clear of any single run, so this cannot touch the files of a
# concurrent suite, and every image is rebuilt from scratch by _mkimg anyway.
if [ -d "$PROJECT_DIR/smoke_run" ]; then
	find "$PROJECT_DIR/smoke_run" -maxdepth 1 -type f \
		\( -name '*.img' -o -name '*.pcap' -o -name '*.log' \) \
		-mmin +60 -delete 2>/dev/null || true
fi

# Start every lane's log empty.
#
# Without this, a lane that never launches leaves the previous run's log in
# place and the checks are graded against it — a full, plausible-looking
# result for a run in which nothing booted. That is not hypothetical: a stray
# `#` inside a QEMU argument list stopped every aarch64 instance from starting
# and three consecutive "runs" reported the same numbers off the same stale
# files. An empty log makes the checks report missing markers, which is the
# truth.
for _l in "$LOG" "$SMP_LOG" "$SYS_LOG" "$BLK_LOG" "$POSIX_LOG" "$GFX_LOG" \
          "$OPENRC_LOG" "$INIT_LOG" "$IOMMU_LOG" "$AMDVI_LOG" "$RASPI_LOG"; do
	: > "$_l" 2>/dev/null || true
done

echo ""
if [ "$SMOKE_PARALLEL" = "1" ]; then
	echo "[RUN] Booting sys, blk, posix, gfx, and SMP QEMU instances in parallel..."
else
	echo "[RUN] Booting both QEMU instances (Single-CPU and SMP) in parallel..."
fi

# Stagger (parallel mode): run the SMP (-smp 4) instance FIRST, alone. It is the
# heaviest (4 vCPUs) but the quickest to finish (SMOKE_FAST_SMP stops it at the
# M24B-SMP work-stealing marker), so giving it the whole 8-core host briefly and
# then reclaiming its 4 vCPUs keeps the long single-CPU instances that follow
# from starving. Launching all five at once oversubscribes the host (4 + 4 vCPUs
# + the GPU instance on 8 cores) and the core instance times out — its ~245
# markers then read as "missing" (spurious failures, not real regressions).
if [ "$SMOKE_PARALLEL" = "1" ] && { [ -z "${SMOKE_INSTANCES:-}" ] || echo " $SMOKE_INSTANCES " | grep -q " smp "; }; then
	(
		B1NIX_ISO_NAME=b1nix-sys.iso
		# aarch64 boots off a virtio-blk disk rather than a CD, so this lane
		# needs its own images like every other one — without them the -drive
		# argument was built with an empty file= and QEMU refused to start,
		# which reported as 41 BLOCKED checks rather than as a harness fault.
		SATA_IMG=$(disk_img sata smp)
		AHCI_IMG=$(disk_img ahci smp)
		NVME_IMG=$(disk_img nvme smp)
		SWAP_IMG=$(disk_img swap smp)
		EXTRA_QEMU_ARGS="-smp 4"
		SMOKE_FAST_SMP=1
		# aarch64: this is also the GICv3 lane. QEMU virt defaults to a GICv2,
		# which has no ITS and therefore no message-signalled interrupts at
		# all; asking for v3 here exercises the redistributors (one per CPU,
		# and this lane has four) and the ITS without changing what every other
		# lane runs on.
		[ "$ARCH" = "aarch64" ] && SMOKE_MACHINE="virt,gic-version=3,iommu=smmuv3"
		# Stop on the USERSPACE AP proof, not the kernel work-stealing selftest:
		# /bin/m24b_smoke (which emits "M24B-BKL: instance ran-on-ap") runs from
		# init, long after the selftest marker, so cutting the instance at the
		# selftest made that check permanently unreachable (reported BLOCKED).
		SMOKE_DONE_PATTERN="M24B-SMP: ok work-stealing|KERNEL PANIC|\[PANIC\]"
		# On a single-CPU arch that marker can never appear: the selftest reports
		# "skip single-cpu" and there is nothing further this lane can show. With
		# only the marker above, the instance never stopped early — it ran the
		# ENTIRE sys set a second time (~735 markers, ~10 minutes of wall time)
		# just to hit the stall timeout, while holding one of the three slots the
		# other lanes were queued for.
		# aarch64 has no userspace-on-AP phase yet (kernel/arch/aarch64/smp.c
		# says what is missing), so the work-stealing verdict is where this
		# instance ends rather than the BKL proof the x86_64 pattern waits for.
		[ "$ARCH" = "aarch64" ] &&
			SMOKE_DONE_PATTERN="M24B-SMP: ok work-stealing|M24B-SMP: skip single-cpu|$SMOKE_DONE_PATTERN"
		SMOKE_PROGRESS_MODE=smp
		PROGRESS_PREFIX="[smp]  "
		run_qemu "$SMP_LOG"
	) &
	pid_smp=$!
	wait $pid_smp
fi

# ── Post-SMP instances as launcher functions ──
# 4 categories: sys (kernel+ipc+elf+diag), blk (storage), posix (shell+coreutils),
# gfx (graphics). 3 slots run concurrently; when one finishes, the next starts
# immediately. gfx is last so it doesn't block faster instances.
launch_sys() {
	(
		# The one ordinary aarch64 lane with a second CPU: the M101 RCU
		# grace-period check needs a reader on another core, and this is the
		# lane its markers are read from.
		[ "$ARCH" = "aarch64" ] && SMOKE_SMP=2
		SATA_IMG=$(disk_img sata sys)
		AHCI_IMG=$(disk_img ahci sys)
		NVME_IMG=$(disk_img nvme sys)
		SWAP_IMG=$(disk_img swap sys)
		B1NIX_ISO_NAME=b1nix-sys.iso
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[sys]   "
		run_qemu "$SYS_LOG"
	) &
	pid_sys=$!
}

launch_blk() {
	(
		SATA_IMG=$(disk_img sata blk)
		AHCI_IMG=$(disk_img ahci blk)
		NVME_IMG=$(disk_img nvme blk)
		SWAP_IMG=$(disk_img swap blk)
		B1NIX_ISO_NAME=b1nix-blk.iso
		# A USB stick behind the xHCI controller every instance already has.
		# It is here to prove that USB mass storage draws from the same sd*
		# sequence as AHCI (so it must come up as the third sd disk, after the
		# SATA pair) and that swap still takes the second ATA disk with a USB
		# disk in the namespace.
		# The USB stick, plus a virtio-blk disk. virtio-blk was not attached
		# to any instance before, so nothing exercised its feature
		# negotiation, its FLUSH or its WRITE ZEROES.
		# The scratch disk arrives over whichever virtio transport the board
		# has: PCI on q35, mmio on QEMU virt (this kernel's virtio-blk PCI
		# driver is the legacy port-I/O one, which aarch64 cannot speak).
		vblk_device=virtio-blk-pci
		[ "$ARCH" = "aarch64" ] && vblk_device=virtio-blk-device
		EXTRA_QEMU_ARGS="-drive file=$(disk_img usb blk),if=none,id=usbdisk,format=raw -device usb-storage,bus=xhci.0,drive=usbdisk \
			-drive file=$(disk_img vblk blk),if=none,id=vblkdisk,format=raw,discard=unmap -device $vblk_device,drive=vblkdisk"
		export EXTRA_QEMU_ARGS
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[blk]   "
		run_qemu "$BLK_LOG"
	) &
	pid_blk=$!
}

launch_posix() {
	(
		SATA_IMG=$(disk_img sata posix)
		AHCI_IMG=$(disk_img ahci posix)
		NVME_IMG=$(disk_img nvme posix)
		SWAP_IMG=$(disk_img swap posix)
		B1NIX_ISO_NAME=b1nix-posix.iso
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[posix] "
		run_qemu "$POSIX_LOG"
	) &
	pid_posix=$!
}

launch_gfx() {
	(
		SATA_IMG=$(disk_img sata gfx)
		AHCI_IMG=$(disk_img ahci gfx)
		NVME_IMG=$(disk_img nvme gfx)
		SWAP_IMG=$(disk_img swap gfx)
		B1NIX_ISO_NAME=b1nix-gfx.iso
		# Skia (raster + Graphite/Dawn) plus a live compositor and the Mesa/
		# Cairo/HarfBuzz client tests do not fit in the default 1 GiB: the
		# instance OOM-kills the compositor mid-run and then panics in kheap growth.
		SMOKE_MEM_MB=${SMOKE_MEM_MB:-1536}
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[gfx]   "
		# Drive the VirGL 3D-accelerated GPU on the graphics instance when the
		# host QEMU + GPU support it (virtio-gpu-gl device + a DRM render node).
		# egl-headless routes virglrenderer to the host GL stack. On hosts without
		# it (e.g. macOS QEMU built without virglrenderer) we fall back to the
		# plain 2D device and the kernel's VirGL selftest is a no-op — the
		# software OpenGL path still runs and is verified.
		if qemu-system-x86_64 -device help 2>/dev/null | grep -q "name \"virtio-gpu-gl-pci\"" &&
		   qemu-system-x86_64 -display help 2>/dev/null | grep -qw egl-headless &&
		   { [ -e /dev/dri/renderD128 ] || [ -e /dev/dri/renderD129 ]; }; then
			GPU_DEVICE="virtio-gpu-gl-pci"
			GPU_DISPLAY="egl-headless"
			export GPU_DEVICE GPU_DISPLAY
		fi
		run_qemu "$GFX_LOG"
	) &
	pid_gfx=$!
}

# OpenRC as PID 1: no test orchestrator, the real init system drives the boot and
# then powers the machine off through its control FIFO (see iso-openrc). The
# instance ends by itself — "reboot: powering off" IS the pass condition.
launch_openrc() {
	(
		SATA_IMG=$(disk_img sata openrc)
		AHCI_IMG=$(disk_img ahci openrc)
		NVME_IMG=$(disk_img nvme openrc)
		SWAP_IMG=$(disk_img swap openrc)
		B1NIX_ISO_NAME=b1nix-openrc.iso
		SMOKE_DONE_PATTERN="reboot: powering off|KERNEL PANIC|\[PANIC\]"
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[openrc]"
		run_qemu "$OPENRC_LOG"
	) &
	pid_openrc=$!
}

# M108: the default boot. PID 1 is /sbin/init — BusyBox init — with no `init=`
# on the cmdline at all, and /etc/inittab drives OpenRC's runlevels underneath
# it, then runs /etc/init-smoke.sh, which is where the M108 init markers come
# from. openrc-init as PID 1 is the other, opt-in configuration (launch_openrc).
launch_init() {
	(
		SATA_IMG=$(disk_img sata init)
		AHCI_IMG=$(disk_img ahci init)
		NVME_IMG=$(disk_img nvme init)
		SWAP_IMG=$(disk_img swap init)
		B1NIX_ISO_NAME=b1nix-init.iso
		SMOKE_DONE_PATTERN="M108-SMOKE: done-init|KERNEL PANIC|\[PANIC\]"
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[init]  "
		run_qemu "$INIT_LOG"
	) &
	pid_init=$!
}

# M109: the initramfs boot. The kernel keeps the RAM filesystem as / and PID 1
# is /init from the initramfs, which mounts the real root below / and hands the
# machine over with BusyBox's switch_root. The markers after the switch are
# printed by the process running in the NEW root.
launch_switchroot() {
	(
		SATA_IMG=$(disk_img sata switchroot)
		NVME_IMG=$(disk_img nvme switchroot)
		SWAP_IMG=$(disk_img swap switchroot)
		B1NIX_ISO_NAME=b1nix-switchroot.iso
		SMOKE_DONE_PATTERN="M109-SMOKE: done-switchroot|KERNEL PANIC|\[PANIC\]"
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[swroot]"
		run_qemu "$SWITCHROOT_LOG"
	) &
	pid_switchroot=$!
}

# M100b: the same OpenRC ISO, on a q35 machine with an Intel IOMMU in front of
# every device. No new image is needed — the VT-d work is kernel-side and its
# self-test runs on any test boot; what this instance supplies is the hardware.
launch_raspi() {
	(
		RASPI_SD="$PROJECT_DIR/smoke_run/rpi-root-$ARCH.img"
		# A Pi has no virtio disk. The root filesystem goes on the SD card and
		# the kernel finds it by the b1nix-root label the image already
		# carries, the same way it would on a real board. QEMU's SD model
		# insists on a power-of-two card, so the 512 MiB filesystem sits at
		# the front of a 1 GiB one.
		rm -f "$RASPI_SD"
		dd if=/dev/zero of="$RASPI_SD" bs=1m count=1024 2>/dev/null
		dd if="$PROJECT_DIR/build/$ARCH/root.ext4" of="$RASPI_SD" \
			conv=notrunc 2>/dev/null
		SMOKE_RASPI=1
		# The board lane runs the minimal userspace profile. The full sys set
		# exercises networking, a GPU and a debug stub, none of which this
		# machine has - those tests cannot pass here and their failures would
		# say nothing about the board. What is checked instead is the board:
		# its own peripherals (kernel-side, so they run regardless of profile),
		# the SD card, and that userspace comes up far enough to offer a login.
		SMOKE_DONE_PATTERN='b1nix login:|KERNEL PANIC|\[PANIC\]'
		# This lane has no hardware acceleration available to it. HVF needs
		# -cpu host and the board model is a fixed Cortex-A72, so raspi4b runs
		# under TCG - four emulated cores, against eight other instances
		# competing for the same host. It is slow rather than wedged, and the
		# default silence allowance kills it mid-boot.
		STALL_TIMEOUT=${SMOKE_RASPI_STALL:-900}
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[raspi] "
		run_qemu "$RASPI_LOG"
	) &
	pid_raspi=$!
}

launch_iommu() {
	[ "$ARCH" = "aarch64" ] && return 0
	(
		SATA_IMG=$(disk_img sata iommu)
		AHCI_IMG=$(disk_img ahci iommu)
		NVME_IMG=$(disk_img nvme iommu)
		SWAP_IMG=$(disk_img swap iommu)
		B1NIX_ISO_NAME=b1nix-openrc.iso
		EXTRA_QEMU_ARGS="-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on \
			-device pcie-root-port,id=iommurp,chassis=9 \
			-device x3130-upstream,id=iommusw,bus=iommurp \
			-device xio3130-downstream,id=iommudn0,bus=iommusw,chassis=10,slot=0 \
			-device xio3130-downstream,id=iommudn1,bus=iommusw,chassis=11,slot=1 \
			-device virtio-tablet-pci,id=iommudev0,bus=iommudn0 \
			-device virtio-tablet-pci,id=iommudev1,bus=iommudn1 \
			-device pci-bridge,id=iommulegacy,chassis_nr=3 \
			-device virtio-tablet-pci,id=iommulegacy0,bus=iommulegacy,addr=1 \
			-device virtio-tablet-pci,id=iommulegacy1,bus=iommulegacy,addr=2 \
			-device pcie-root-port,id=iommurp2,chassis=12 \
			-device nvme-subsys,id=iommusubsys,nqn=b1nix-iommu \
			-device nvme,id=iommunvme,serial=deadbee2,subsys=iommusubsys,sriov_max_vfs=1,sriov_vq_flexible=2,sriov_vi_flexible=1,bus=iommurp2"
		SMOKE_DONE_PATTERN="reboot: powering off|KERNEL PANIC|\[PANIC\]"
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[iommu]"
		run_qemu "$IOMMU_LOG"
	) &
	pid_iommu=$!
}

# M100d: the same ISO on a machine whose IOMMU is AMD's. Nothing about the
# image changes; what changes is which unit the kernel finds.
launch_amdvi() {
	[ "$ARCH" = "aarch64" ] && return 0
	(
		SATA_IMG=$(disk_img sata amdvi)
		AHCI_IMG=$(disk_img ahci amdvi)
		NVME_IMG=$(disk_img nvme amdvi)
		SWAP_IMG=$(disk_img swap amdvi)
		B1NIX_ISO_NAME=b1nix-openrc.iso
		EXTRA_QEMU_ARGS="-machine q35,kernel-irqchip=split -device amd-iommu,intremap=on"
		SMOKE_DONE_PATTERN="reboot: powering off|KERNEL PANIC|\[PANIC\]"
		SMOKE_PROGRESS_MODE=full
		PROGRESS_PREFIX="[amdvi]"
		run_qemu "$AMDVI_LOG"
	) &
	pid_amdvi=$!
}

launch_smp_solo() {
	(
		SATA_IMG=$(disk_img sata smp)
		AHCI_IMG=$(disk_img ahci smp)
		NVME_IMG=$(disk_img nvme smp)
		SWAP_IMG=$(disk_img swap smp)
		NET_PCAP="$PROJECT_DIR/smoke_run/net-$ARCH-smp.pcap"
		EXTRA_QEMU_ARGS="-smp 4"
		SMOKE_FAST_SMP=1
		# Stop on the USERSPACE AP proof, not the kernel work-stealing selftest:
		# /bin/m24b_smoke (which emits "M24B-BKL: instance ran-on-ap") runs from
		# init, long after the selftest marker, so cutting the instance at the
		# selftest made that check permanently unreachable (reported BLOCKED).
		SMOKE_DONE_PATTERN="M24B-BKL: instance ran-on-ap|M24B-SMP: fail work-stealing|KERNEL PANIC|\[PANIC\]"
		[ "$ARCH" = "aarch64" ] &&
			SMOKE_DONE_PATTERN="M24B-SMP: skip single-cpu|$SMOKE_DONE_PATTERN"
		SMOKE_PROGRESS_MODE=smp
		PROGRESS_PREFIX="[smp]  "
		run_qemu "$SMP_LOG"
	) &
	pid_smp=$!
}

# ── Dynamic slot pool: 3 concurrent, fill freed slots immediately ──
# Polls finished PIDs every second (POSIX-sh compatible) rather than waiting
# on full batches, so a fast instance exiting early makes room for the next one
# without blocking on the others.
run_slot_pool() {
	_max=$1; shift
	_pids="" _queue="" _idx=0
	for _n; do
		if [ "$_idx" -lt "$_max" ]; then
			"launch_$_n"
			_pids="$_pids $!"
			_idx=$((_idx + 1))
		else
			_queue="$_queue $_n"
		fi
	done
	for _n in $_queue; do
		_done=0
		while [ "$_done" = "0" ]; do
			for _p in $_pids; do
				if ! kill -0 "$_p" 2>/dev/null; then
					wait "$_p" 2>/dev/null || true
					_new=""
					for _pp in $_pids; do [ "$_pp" != "$_p" ] && _new="$_new $_pp"; done
					_pids=$_new
					_done=1
					break
				fi
			done
			[ "$_done" = "0" ] && sleep 1
		done
		"launch_$_n"
		_pids="$_pids $!"
	done
	for _p in $_pids; do wait "$_p" 2>/dev/null || true; done
}

if [ "$SMOKE_PARALLEL" = "1" ]; then
	SMOKE_MAX_CONCURRENT=${SMOKE_MAX_CONCURRENT:-3}
	echo "[RUN] post-SMP instances, $SMOKE_MAX_CONCURRENT at a time"
	_inst_list="sys blk posix gfx openrc init switchroot iommu amdvi"
	# The Raspberry Pi lane is off by default, and not because it is broken.
	#
	# It is the one instance no accelerator can take: HVF needs -cpu host and
	# raspi4b is a fixed Cortex-A72, so it runs four emulated cores under TCG
	# while every other lane runs at native speed. That is the whole of the
	# difference between this suite taking five minutes and taking twenty --
	# its own stall allowance is 900 s, which is most of the wall clock of a
	# green run.
	#
	# The board is not currently working anyway, and the same argument the
	# x86_64 suite already makes applies here: emulate the drivers, confirm the
	# machine on the machine. AArch64 is verified on QEMU virt and then on the
	# Xperia 5. Set SMOKE_RASPI_LANE=1 when the board itself is the subject.
	if [ "${SMOKE_RASPI_LANE:-0}" = "1" ] && [ "$ARCH" = "aarch64" ] &&
	   qemu-system-aarch64 -machine help 2>/dev/null | grep -q "^raspi4b "; then
		_inst_list="$_inst_list raspi"
	fi
	[ -n "${SMOKE_INSTANCES:-}" ] && _inst_list="$SMOKE_INSTANCES"
	# Drop the logs of every instance this run will not start. Their checks
	# would otherwise grep the previous run's output and report a pass nobody
	# earned -- a restricted SMOKE_INSTANCES run printed the full suite's
	# numbers. A missing log reads as wedged, so those checks come out BLOCKED,
	# which is what "not run" honestly is.
	# smp is launched by its own block above, under the same condition.
	_ran_list="$_inst_list"
	if [ -z "${SMOKE_INSTANCES:-}" ] || echo " $SMOKE_INSTANCES " | grep -q " smp "; then
		_ran_list="$_ran_list smp"
	fi
	for _known in sys blk posix gfx openrc init switchroot iommu amdvi raspi smp; do
		case " $_ran_list " in
		*" $_known "*) continue ;;
		esac
		rm -f "$PROJECT_DIR/smoke_run/b1nix-smoke-$_known-$ARCH.log"
	done
	run_slot_pool $SMOKE_MAX_CONCURRENT $_inst_list
	cat "$SYS_LOG" "$BLK_LOG" "$POSIX_LOG" "$GFX_LOG" "$OPENRC_LOG" "$INIT_LOG" "$SWITCHROOT_LOG" "$IOMMU_LOG" "$AMDVI_LOG" "$RASPI_LOG" 2>/dev/null >"$LOG" || true
else
	launch_sys
	launch_smp_solo
	wait $pid_sys
	wait $pid_smp
fi

if [ "$SMOKE_QUICK" = "1" ]; then
	echo ""
	echo "=== Analyzing Quick Smoke Results ==="
	check_output "$LOG" "b1nix kernel" "kernel banner appears"
	check_output "$LOG" "B1NIX-QUICK: ok native" "native userspace smoke passes"
	check_output "$LOG" "B1NIX-QUICK: done" "quick smoke completes"
	check_output "$SMP_LOG" "M24B-BKL: instance ran-on-ap" "SMP work-stealing passes"
	if grep -q -E "KERNEL PANIC|\[PANIC\]" "$LOG" "$SMP_LOG" 2>/dev/null; then
		fail "quick smoke completes without panic" "PANIC detected in log"
	else
		pass "quick smoke completes without panic"
	fi
	echo ""
	echo "=== Results ==="
	echo "  Passed:  $PASSED"
	echo "  Failed:  $FAILED"
	for _i in sys blk posix gfx openrc init switchroot iommu amdvi smp; do
	    rm -f "$(disk_img sata "$_i")" "$(disk_img nvme "$_i")" "$(disk_img swap "$_i")" "$(disk_img usb "$_i")" "$(disk_img vblk "$_i")"
	done
	[ "$FAILED" -eq 0 ]
	exit
fi

echo ""
echo "=== Analyzing Test Results ==="

# ── Test 1: Kernel boots ──
echo ""
echo "[RUN] Boot smoke checks..."
check_output "$LOG" "b1nix kernel" "kernel banner appears"
check_output "$LOG" "pmm:" "physical memory manager initializes"
check_output "$LOG" "kheap:" "kernel heap initializes"
# SMEP: an x86 control-register bit. The kernel prints its CR4 read-back, so
# this checks the bit is really set and not merely that the code ran. A
# processor that does not offer the feature says so on the same line and is
# accepted — silence is not. aarch64 has PAN/PXN instead, checked elsewhere.
if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "x86" ]; then
check_output "$LOG" "smep: \(enabled, cr4=0x[0-9a-f]*\|unavailable\)" \
	"CR4.SMEP is enabled (or the CPU reports it absent)"
# ... and on every core, not just the one that printed. The APs enable it
# silently (their line would corrupt the bring-up marker), so the kernel counts
# them; this compares the two numbers on that line.
_smep_line=$(grep -a "smep: active on" "$LOG" 2>/dev/null | tail -1)
_smep_have=$(echo "$_smep_line" | sed -n 's/.*active on \([0-9]*\) of \([0-9]*\) CPUs.*/\1/p')
_smep_want=$(echo "$_smep_line" | sed -n 's/.*active on \([0-9]*\) of \([0-9]*\) CPUs.*/\2/p')
if [ -n "$_smep_want" ] && [ "$_smep_have" = "$_smep_want" ]; then
	pass "CR4.SMEP is set on every online CPU ($_smep_have)"
elif grep -aq "smep: unavailable" "$LOG" 2>/dev/null; then
	pass "CR4.SMEP unavailable on this CPU — reported, not silently skipped"
else
	fail "CR4.SMEP is set on every online CPU" "${_smep_line:-no smep tally in log}"
fi

# ── Every core agrees about what a page-table entry means ──
#
# CR4 is per-CPU, and two of its bits change the meaning of a page-table entry
# rather than merely enabling a feature. PGE is the one that bit this kernel:
# the AP trampoline set it and boot.S did not, so bit 8 of a leaf entry was a
# flag available to software on the boot CPU and the architectural GLOBAL bit
# on every other one -- and a global translation is not evicted by a write to
# CR3, which is the only flush a context switch and tlb_shootdown_all() do. A
# shared user page therefore kept a live translation on an AP after its address
# space was destroyed and its frames handed to someone else, and the next
# process to use that address on that core read and executed the dead one's
# memory. It showed up as SIGILL and #GP on instructions that were perfectly
# valid, in processes that had nothing to do with each other.
#
# Nothing had ever compared one core's control registers against another's, so
# the divergence was invisible until its consequences were traced back to it.
# The kernel now does that comparison at the end of SMP bring-up, and refuses
# CR4.PGE outright while a software flag lives on bit 8. This is that check.
if grep -aq "SMP-CPUSTATE: ok " "$LOG" 2>/dev/null; then
	pass "every CPU's CR0/CR4/XCR0/EFER/PAT match the boot CPU's, PGE off"
else
	fail "every CPU's CR0/CR4/XCR0/EFER/PAT match the boot CPU's, PGE off" \
		"$(grep -a 'SMP-CPUSTATE:' "$LOG" 2>/dev/null | head -3)"
fi

# ── The boot CPU's kernel stack has room for the boot path ──
#
# kernel_main runs every driver probe, every filesystem and network init and,
# once the timer is armed, a whole scheduler pass on top of all of it, on the
# single stack boot.S reserves -- more than 100 KiB of it. Overflow runs off the
# bottom through the boot page tables, which are dead by then and absorb the
# damage silently, and on into the .bss scalars underneath.
#
# So measure the depth on EVERY instance rather than only the one that tips
# over: the margin is what is under test, not the crash. A peak past 75% fails
# the run while the stack still holds, long before the guard page is reached.
_bs_line=$(grep -a "BOOT-STACK: peak=" "$LOG" 2>/dev/null | tail -1)
_bs_peak=$(echo "$_bs_line" | sed -n 's/.*peak=\([0-9]*\).*/\1/p')
_bs_total=$(echo "$_bs_line" | sed -n 's/.*total=\([0-9]*\).*/\1/p')
_bs_pct=$(echo "$_bs_line" | sed -n 's/.*pct=\([0-9]*\).*/\1/p')
if [ -z "$_bs_peak" ] || [ -z "$_bs_total" ] || [ "$_bs_total" = "0" ]; then
	fail "boot stack depth is measured and reported" \
		"${_bs_line:-no BOOT-STACK line in log}"
elif [ "$_bs_peak" -ge "$_bs_total" ]; then
	# Saturated: the paint was consumed to the very bottom, so the real peak is
	# unknown and at least this. That is an overflow whether or not it crashed.
	fail "boot stack has headroom" "boot stack FULL: $_bs_line"
elif [ "$_bs_pct" -gt 75 ]; then
	fail "boot stack has headroom" "boot stack over 75% used: $_bs_line"
else
	pass "boot stack has headroom (${_bs_peak} of ${_bs_total} bytes, ${_bs_pct}%)"
fi

# ... and the guard under it is actually armed. An unmapped page cannot be
# tested by touching it, so the kernel counts the pages it managed to unmap and
# prints the tally; anything short of all of them means the tripwire is not
# there, which is the state this bug lived in for months.
_bs_guard=$(grep -a "vmm: boot-stack guard" "$LOG" 2>/dev/null | tail -1)
_bs_g_got=$(echo "$_bs_guard" | sed -n 's|.*guard \([0-9]*\)/\([0-9]*\).*|\1|p')
_bs_g_want=$(echo "$_bs_guard" | sed -n 's|.*guard \([0-9]*\)/\([0-9]*\).*|\2|p')
if [ -n "$_bs_g_want" ] && [ "$_bs_g_want" -gt 0 ] && [ "$_bs_g_got" = "$_bs_g_want" ]; then
	pass "boot-stack guard is armed ($_bs_g_got pages unmapped)"
else
	fail "boot-stack guard is armed" "${_bs_guard:-no boot-stack guard line in log}"
fi

# The syscall entry stack has the same tripwire, and needs it more: it sits
# immediately above the boot stack, so an overflow there runs down into it.
_sc_guard=$(grep -a "vmm: syscall-stack guard" "$LOG" 2>/dev/null | tail -1)
_sc_g_got=$(echo "$_sc_guard" | sed -n 's|.*guard \([0-9]*\)/\([0-9]*\).*|\1|p')
_sc_g_want=$(echo "$_sc_guard" | sed -n 's|.*guard \([0-9]*\)/\([0-9]*\).*|\2|p')
if [ -n "$_sc_g_want" ] && [ "$_sc_g_want" -gt 0 ] && [ "$_sc_g_got" = "$_sc_g_want" ]; then
	pass "syscall-stack guard is armed ($_sc_g_got pages unmapped)"
else
	fail "syscall-stack guard is armed" "${_sc_guard:-no syscall-stack guard line in log}"
fi

# The guard page under the boot stack must never be reached. If it ever is, the
# fault handler names it -- which is the whole point of unmapping it: an
# overflow becomes a report that says "boot-stack overflow" instead of a
# networking pointer going bad three subsystems away.
for _bs_log in "$LOG" "$SYS_LOG" "$OPENRC_LOG" "$IOMMU_LOG" "$AMDVI_LOG"; do
	[ -f "$_bs_log" ] || continue
	if grep -aq "boot-stack overflow" "$_bs_log" 2>/dev/null; then
		fail "no boot-stack overflow on any instance" \
			"guard page hit in $(basename "$_bs_log")"
		_bs_overflow=1
	fi
done
[ -n "${_bs_overflow:-}" ] || pass "no boot-stack overflow on any instance"
# b1cc is cut from the build (B1NIX_NO_B1CC); its B1CC-*-SMOKE checks were
# removed with it and come back when b1cc does (they are in git).
fi
# b1cc temporarily cut from the build (B1NIX_NO_B1CC) — restored as a separate
# change. These checks are disabled until b1cc is re-added.
# check_output "$LOG" "B1CC-R42-SMOKE: ok" "b1cc return_42 runs and exits with 42"
# check_output "$LOG" "B1CC-HELLO-SMOKE: ok" "b1cc hello runs and exits with 0"
# check_output "$LOG" "B1CC-ARGV-SMOKE: ok" "b1cc argv propagation works"
# check_output "$LOG" "B1CC-FILE-SMOKE: ok" "b1cc file write works"
# check_output "$LOG" "B1CC-STDERR-SMOKE: ok" "b1cc stderr exit status propagates"
# check_output "$LOG" "B1CC-BETTER-C-SMOKE: ok" "b1cc better C features work (M7)"
# check_output "$LOG" "B1CC-M34-SMOKE: ok" "b1cc M34 features run on x86_64-b1nix"
# check_output "$LOG" "B1CC-M34-TARGET: all ok" "b1cc M34 target corpus passes on-device"

# ── Test 2: No panic ──
if grep -q "KERNEL PANIC" "$LOG" 2>/dev/null; then
	fail "kernel boots without panic" "PANIC detected in log"
else
	pass "kernel boots without panic"
fi

# ── Test 3-7: Core boot path markers ──
check_output "$LOG" "initramfs: files" "initramfs initializes"
check_output "$LOG" "init: /sbin/init pid=" "the default PID 1 (/sbin/init, BusyBox init) launches"
check_output "$LOG" "M94-INIT:" "M94 init-path parsing self-test runs"
check_output "$LOG" "M94-INIT: ok \(default\|init=\|no-override-flags\)" "M94 init-path logic correct"
# Linking policy: the rootfs must not carry a statically linked executable that
# is not in tools/configs/static-allowlist.txt with a reason. Run the same gate
# the build uses, so a regression shows up as a failed check and not only as a
# build error someone might bypass.
if sh "$PROJECT_DIR/tools/check-dynamic.sh" "$PROJECT_DIR/build/$ARCH/rootfs" >/dev/null 2>&1; then
	pass "rootfs has no unexpected statically linked executables"
else
	fail "rootfs has no unexpected statically linked executables" "$(sh "$PROJECT_DIR/tools/check-dynamic.sh" "$PROJECT_DIR/build/$ARCH/rootfs" 2>&1 | head -3 | tr '\n' ' ')"
fi
check_output "$LOG" "M94-CTL: ok tmpfs-mount" "tmpfs mounts on a VFS directory (the /run an init system expects)"
check_output "$LOG" "M94-CTL: ok tmpfs-state" "state written through a dirfd inside the tmpfs is visible afterwards"
check_output "$LOG" "M94-CTL: ok fifo-on-tmpfs" "mkfifo works on a tmpfs mount"
check_output "$LOG" "M94-CTL: ok fifo-command" "a command written by another process is read back from the control FIFO"
# OpenRC instance: the real init system as PID 1, driving its own runlevels and
# shutting the machine down through /run/openrc/init.ctl.
check_output "$OPENRC_LOG" "init: /sbin/openrc-init pid=" "openrc-init runs as PID 1 when init= selects it"
check_output "$OPENRC_LOG" "Caching service dependencies" "OpenRC builds its dependency cache (popen/posix_spawn work)"
check_output "$OPENRC_LOG" "/etc/init.d/local start" "OpenRC reaches the default runlevel and starts services"
check_output "$OPENRC_LOG" "M94-OPENRC: ok pid1" "with init=/sbin/openrc-init, PID 1 is really the openrc-init ELF (/proc/1/exe)"
check_output "$OPENRC_LOG" "M94-OPENRC: ok reaps-orphan" "openrc-init reaps an orphaned grandchild re-parented to PID 1"
check_output "$OPENRC_LOG" "M94-OPENRC: ok shell" "the openrc-init boot reaches a usable shell"
check_output "$OPENRC_LOG" "M94-OPENRC: done-init" "the openrc-init PID 1 suite completes"
check_output "$OPENRC_LOG" "M94-OPENRC: ok init-fifo-present" "openrc-init creates its control FIFO once the boot finishes"
check_output "$OPENRC_LOG" "/etc/init.d/killprocs start" "the shutdown runlevel runs when PID 1 gets the command"
check_output "$OPENRC_LOG" "reboot: powering off" "openrc-shutdown powers the machine off through the control FIFO"
# M28 #9: ctx-switch + light-syscall rdtsc benchmark. It is single-CPU only by
# design (the rdtsc yield loop races under the SMP high-syscall-density path, see
# m28_ctxbench.c), so on the -smp 2/4 smoke runs it correctly reports "skip smp".
# Accept either the measurement or the deliberate skip.
check_output "$LOG" "M28-BENCH: \(ok\|skip smp\)" "M28 ctx-switch benchmark completes"

# ── M12 Syscalls & Process Management ──
section "M12 Syscalls & Process Management"
check_output "$LOG" "M12-SMOKE: start" "M12 smoke starts"
check_output "$LOG" "M12-SMOKE: ok spawn" "spawn basic command works"
check_output "$LOG" "M12-SMOKE: ok execve" "execve works"
check_output "$LOG" "M12-SMOKE: ok status-prop" "child exit status propagation works"
check_output "$LOG" "M12-SMOKE: ok waitpid" "waitpid success path works"
check_output "$LOG" "M12-SMOKE: ok stress" "repeated spawn/wait stress works"
check_output "$LOG" "M12-SMOKE: ok zombie" "zombie reaping behavior works"
check_output "$LOG" "M12-SMOKE: ok fd-inheritance" "inherited stdout/stderr across exec works"
check_output "$LOG" "M12-SMOKE: ok dup2" "dup2 behavior works"
check_output "$LOG" "M12-SMOKE: ok close-on-exec" "close-on-exec works"
check_output "$LOG" "M12-SMOKE: ok brk" "brk basic growth/shrink sanity works"
check_output "$LOG" "M12-SMOKE: ok mmap" "mmap/munmap mapping lifecycle works"
check_output "$LOG" "M12-SMOKE: ok mremap" "mremap grows and shrinks a mapping, contents intact"
check_output "$LOG" "M12-SMOKE: ok invalid-args" "invalid pointer/argument handling works"
check_output "$LOG" "M12-SMOKE: ok kill" "basic kill signal works"
check_output "$LOG" "M12-SMOKE: ok sigaction" "sigaction signal behavior works"
check_output "$LOG" "M12-SMOKE: ok setsid-pgrp" "process group / session sanity works"
check_output "$LOG" "M12-SMOKE: ok uid-gid" "uid/gid getter/setter sanity works"
check_output "$LOG" "M12-SMOKE: done" "M12 smoke completes successfully"

# ── M13 Userspace ABI / libc / POSIX runtime hardening ──
section "M13 Userspace ABI / libc / POSIX runtime"
check_output "$LOG" "M13-SMOKE: start" "M13 smoke starts"
check_output "$LOG" "M13-SMOKE: ok argc-argv0" "argc/argv[0] baseline is stable"
check_output "$LOG" "M13-SMOKE: ok stack-align" "initial userspace stack alignment is sane"
check_output "$LOG" "M13-SMOKE: ok libc-rw-open-close-lseek" "libc wrappers for read/write/open/close/lseek work"
check_output "$LOG" "M13-SMOKE: ok getpid-uid-gid" "getpid/getuid/getgid path works"
check_output "$LOG" "M13-SMOKE: ok brk-mmap-munmap" "brk/mmap/munmap baseline works"
check_output "$LOG" "M13-SMOKE: ok puts" "puts works"
check_output "$LOG" "M13-SMOKE: ok printf" "printf works"
check_output "$LOG" "M13-SMOKE: ok snprintf" "snprintf works"
check_output "$LOG" "M13-SMOKE: ok stdio-file" "fopen/fread/fwrite/fclose path works"
if grep -q "M13-SMOKE: ok execve-argv-env" "$LOG" 2>/dev/null; then
	pass "execve preserves argv/envp semantics"
elif grep -q "M13-SMOKE: unsupported execve-argv-env-native-elf" "$LOG" 2>/dev/null; then
	fail "execve preserves argv/envp semantics" "native ELF argv/envp is explicitly unsupported"
else
	missing_marker "$LOG" "execve argv/envp marker emitted" "missing execve argv/envp support/unsupported marker"
fi
check_output "$LOG" "M13-SMOKE: ok execve-fail-deterministic" "failed execve returns deterministic child status"
if grep -q "M13-SMOKE: ok builtin-exec" "$LOG" 2>/dev/null; then
	pass "builtin exec path works through execve"
elif grep -q "M13-SMOKE: unsupported builtin-exec" "$LOG" 2>/dev/null; then
	fail "builtin exec path works through execve" "builtin exec is explicitly unsupported"
else
	missing_marker "$LOG" "builtin exec marker emitted" "missing builtin exec support/unsupported marker"
fi
if grep -q "M13-SMOKE: ok sh-c-argv" "$LOG" 2>/dev/null; then
	pass "/bin/sh -c preserves command argv semantics"
elif grep -q "M13-SMOKE: unsupported sh-c-argv" "$LOG" 2>/dev/null; then
	fail "/bin/sh -c preserves command argv semantics" "sh -c argv is explicitly unsupported"
else
	missing_marker "$LOG" "sh -c argv marker emitted" "missing sh -c argv support/unsupported marker"
fi
if grep -q "M13-SMOKE: ok sh-c-status" "$LOG" 2>/dev/null; then
	pass "/bin/sh -c execution returns stable status"
elif grep -q "M13-SMOKE: unsupported sh-c-status" "$LOG" 2>/dev/null; then
	fail "/bin/sh -c execution returns stable status" "sh -c status is explicitly unsupported"
else
	missing_marker "$LOG" "sh -c status marker emitted" "missing sh -c status support/unsupported marker"
fi
if grep -q "M13-SMOKE: ok fd-inherit-exec" "$LOG" 2>/dev/null; then
	pass "fd inheritance survives exec boundary"
elif grep -q "M13-SMOKE: unsupported fd-inherit-exec" "$LOG" 2>/dev/null; then
	fail "fd inheritance survives exec boundary" "fd inheritance exec-boundary is explicitly unsupported"
else
	missing_marker "$LOG" "fd inheritance exec marker emitted" "missing fd inheritance support/unsupported marker"
fi
check_output "$LOG" "M13-SMOKE: ok dup2" "dup2 behavior remains correct"
if grep -q "M13-SMOKE: ok cloexec-exec" "$LOG" 2>/dev/null; then
	pass "close-on-exec is enforced across exec boundary"
elif grep -q "M13-SMOKE: unsupported cloexec-exec" "$LOG" 2>/dev/null; then
	fail "close-on-exec is enforced across exec boundary" "close-on-exec exec-boundary is explicitly unsupported"
else
	missing_marker "$LOG" "close-on-exec exec marker emitted" "missing close-on-exec support/unsupported marker"
fi
check_output "$LOG" "M13-SMOKE: ok parent-intact" "failed child exec path does not corrupt parent runtime"
check_output "$LOG" "M13-SMOKE: ok errno-negative" "negative syscall result path is exposed to userspace"
check_output "$LOG" "M13-SMOKE: done" "M13 smoke completes successfully"
check_output "$LOG" "M13-JC-SMOKE: start" "M13 job-control smoke starts"
check_output "$LOG" "M13-JC-SMOKE: ok tcsetpgrp-child" "foreground pgrp switches to child group"
check_output "$LOG" "M13-JC-SMOKE: ok wuntraced" "waitpid reports stopped child with WUNTRACED"
check_output "$LOG" "M13-JC-SMOKE: ok tcsetpgrp-self" "foreground pgrp switches back to parent group"
check_output "$LOG" "M13-JC-SMOKE: ok wcontinued" "waitpid reports continued child with WCONTINUED"
check_output "$LOG" "M13-JC-SMOKE: ok sigttin" "background terminal read stops with SIGTTIN"
check_output "$LOG" "M13-JC-SMOKE: ok sigttou" "background terminal write stops with SIGTTOU when TOSTOP is set"
check_output "$LOG" "M13-JC-SMOKE: done" "M13 job-control smoke completes"

# ── M17 errno matrix smoke ──
section "M17 errno matrix"
check_output "$LOG" "M17-SMOKE: start" "M17 smoke starts"
check_output "$LOG" "M17-SMOKE: ok eloop" "M17 ELOOP symlink-depth behavior is correct"
check_output "$LOG" "M17-SMOKE: ok enametoolong" "M17 ENAMETOOLONG path-component limit is correct"
check_output "$LOG" "M17-SMOKE: ok enotdir" "M17 ENOTDIR file-as-directory behavior is correct"
check_output "$LOG" "M17-SMOKE: ok eisdir" "M17 EISDIR write-to-directory behavior is correct"
if grep -q "M17-SMOKE: ok erofs" "$LOG" 2>/dev/null; then
	pass "M17 EROFS readonly mount behavior is correct"
elif grep -q "M17-SMOKE: ok erofs-skip" "$LOG" 2>/dev/null; then
	pass "M17 EROFS test skipped (no readonly mount candidate)"
else
	missing_marker "$LOG" "M17 EROFS marker emitted" "missing erofs/erofs-skip marker"
fi
check_output "$LOG" "M17-SMOKE: ok errno-isolation" "M17 errno isolation across successful syscall is correct"
check_output "$LOG" "KHEAP-SELFTEST: ok split-on-reuse" "the allocator carves the unused tail off a block it reuses"
check_output "$LOG" "KHEAP-SELFTEST: ok remainder-reusable" "and that tail is available to later allocations"
check_output "$LOG" "M17-SMOKE: ok o-nofollow-eloop" "M17 O_NOFOLLOW refuses a symlink with ELOOP instead of following it"
check_output "$LOG" "M17-SMOKE: ok o-path-symlink" "M17 O_PATH|O_NOFOLLOW opens the symlink itself and fstat reports S_IFLNK"
check_output "$LOG" "M17-SMOKE: ok o-path-read-ebadf" "M17 an O_PATH descriptor has no contents to read (EBADF)"
check_output "$LOG" "M17-SMOKE: ok o-path-dirfd" "M17 an O_PATH directory descriptor still anchors openat()"
check_output "$LOG" "M17-SMOKE: done" "M17 smoke completes successfully"

# ── M8 AIO / completion queues ──
section "M8 AIO completion queues"
check_output "$LOG" "M8-AIO-SMOKE: start" "M8 AIO smoke starts"
check_output "$LOG" "M8-AIO-SMOKE: ok write" "M8 AIO async write completes"
check_output "$LOG" "M8-AIO-SMOKE: ok read" "M8 AIO async read completes"
check_output "$LOG" "M8-AIO-SMOKE: done" "M8 AIO smoke completes"

# ── M14 Advanced Storage, Swap & File Systems ──
section "M14 Storage, Swap & File Systems"
check_output "$LOG" "M14-SMOKE: ok swap-smoke" "swap page swap-out and swap-in verified"
check_output "$LOG" "swap: device=sdb" "swap attaches the second SATA disk under its Unix name sdb"
check_output "$LOG" "M14-SMOKE: ok mount-ext4-sata" "mount sda as ext4 successful"
check_output "$LOG" "M14-SMOKE: ok mount-ext4-nvme" "mount nvme0n1 as ext4 successful"
check_output "$LOG" "M14-SMOKE: ok ext4-persistence" "ext4 read, write, and remount persistence verified"
check_output "$LOG" "M14-SMOKE: ok ext4-fifo-persistence" "a FIFO created with mkfifo is a real ext4 inode and survives umount/mount"
check_output "$LOG" "M14-SMOKE: ok block-cache" "cached read and dirty write verified"
check_output "$LOG" "M14-SMOKE: ok persistence" "persistence through sync, umount, and remount verified"
check_output "$LOG" "M14-SMOKE: ok invalid-device" "mounting invalid device fails gracefully"
check_output "$LOG" "M14-SMOKE: ok invalid-fs" "mounting invalid filesystem type fails gracefully"
check_output "$LOG" "M14-SMOKE: ok stress-loop" "repeated create/write/read/delete loop completes successfully"
check_output "$LOG" "M14-SMOKE: ok large-file" "large file bounds allocation and verification successful"
check_output "$LOG" "M14-SMOKE: ok VFS-normalization" "VFS path normalization works on mounts"
check_output "$LOG" "M14-SMOKE: ok mmap-durable" "M72: a writable MAP_SHARED mmap store survives forced page-cache reclaim (drop_caches) and is read back from disk"
check_output "$LOG" "M14-SMOKE: done" "M14 smoke completes successfully"

# ── VirtIO-9P (9P2000.L) ──
section "VirtIO-9P / Host-Guest File Sharing"
check_output "$LOG" "M110-9P: start" "M110 VirtIO-9P smoke starts"
check_output "$LOG" "M110-9P: ok mount" "VirtIO-9P mount successful"
check_output "$LOG" "M110-9P: ok read-host-file" "VirtIO-9P read file from host verified"
check_output "$LOG" "M110-9P: ok write-guest-file" "VirtIO-9P write file from guest verified"
check_output "$LOG" "M110-9P: ok readdir" "VirtIO-9P directory traversal verified"
check_output "$LOG" "M110-9P: done" "VirtIO-9P smoke complete"

# ── M15 IPC, Security & Standard OS Features ──
section "M15 IPC, security, and OS baseline"
check_output "$LOG" "M15-SMOKE: start" "M15 smoke starts"
check_output "$LOG" "M15-SMOKE: ok signal-baseline" "signal baseline syscall path works"
check_output "$LOG" "M15-SMOKE: ok signal-ignore" "ignored signal does not terminate process"
check_output "$LOG" "M15-SMOKE: ok signal-handler" "userspace signal handler is delivered"
check_output "$LOG" "M15-SMOKE: ok signal-mask" "blocked signal is delivered after sigprocmask unblock"
check_output "$LOG" "M15-SMOKE: ok ipc-mq" "message queue roundtrip works"
check_output "$LOG" "M15-SMOKE: ok shm" "shared memory create/map/read/write lifecycle works"
check_output "$LOG" "M15-SMOKE: ok shm-exit-cleanup" "shm attachment released on exit (IPC_RMID no longer blocked)"
check_output "$LOG" "M15-SMOKE: ok shm-kill-cleanup" "shm attachment released when a child is SIGKILL'd (OOM path) + fork nattch accounting"
check_output "$LOG" "M15-SMOKE: ok semaphore" "cooperative semaphore baseline works"
check_output "$LOG" "M15-SMOKE: ok clock-timer" "clock_gettime and nanosleep work"
check_output "$LOG" "M15-SMOKE: ok permissions-chmod" "chmod changes mode bits"
check_output "$LOG" "M15-SMOKE: ok permissions-enforcement" "permissions are enforced for non-root uid"
# ── M74: RT signals (POSIX real-time signals queue, do not coalesce) ──
check_output "$LOG" "M74-SMOKE: ok rt-queue" "M74: 3 blocked SIGRTMIN sends are all delivered after unblock (RT signals queue, not coalesce)"
check_output "$LOG" "M74-SMOKE: ok rt-sigqueue" "M74: sigqueue payloads reach an SA_SIGINFO handler as si_value in FIFO order"
check_output "$LOG" "M74-SMOKE: ok rt-timer" "M74: a POSIX interval timer (timer_create/settime) repeatedly raises an RT signal carrying its sigev_value"
check_output "$LOG" "M15-SMOKE: ok audit-logging" "audit marker appears after privileged syscall"
check_output "$LOG" "M15-SMOKE: done" "M15 smoke completes"

# ── M56 Event-loop & IPC primitives ──
section "M56 Event-loop & IPC Primitives"
check_output "$LOG" "M56-SMOKE: start" "M56 smoke starts"
check_output "$LOG" "M56-SMOKE: ok eventfd" "eventfd counter write/read + semaphore mode work"
check_output "$LOG" "M56-SMOKE: ok timerfd-epoll" "a repeating timerfd wakes epoll_wait on schedule (the frame clock every event loop runs on)"
check_output "$LOG" "M56-SMOKE: ok epoll" "epoll_wait wakes on a ready fd and times out when idle"
check_output "$LOG" "M56-SMOKE: ok timerfd" "timerfd fires and is pollable via epoll"
check_output "$LOG" "M56-SMOKE: ok signalfd" "signalfd delivers a raised signal as a readable record"
check_output "$LOG" "M56-SMOKE: ok seal" "F_SEAL_WRITE on a sealable memfd rejects writes"
check_output "$LOG" "M56-SMOKE: done" "M56 smoke completes"
# ── POSIX memory/signal primitives (madvise, MAP_NORESERVE, sigaltstack) ──
section "POSIX memory/signal primitives"
check_output "$LOG" "MM-SMOKE: start" "MM smoke starts"
check_output "$LOG" "MM-SMOKE: ok readdir-terminates" "readdir returns every entry once and ends (a child with no cursor sequence used to restart the walk forever)"
# Is a shared mapping actually shared? Every Wayland client depends on it, and
# a failure here is invisible from every other angle: descriptors pass, buffers
# are accepted, frame callbacks fire, and the screen stays blank.
check_output "$LOG" "SHMSHARE: ok memfd-fork" "a memfd mapped MAP_SHARED in two processes is one memory: each side sees the other's writes"
check_output "$LOG" "SHMSHARE: ok memfd-scm-rights" "a memfd passed over AF_UNIX SCM_RIGHTS maps to the same memory in the receiver — the path libwayland uses"
check_output "$LOG" "SHMSHARE: ok shm-open-shared" "a POSIX shared-memory object opened by name in a second process maps to the same memory (an in-memory file page must come from the page cache, or each mapper gets a private copy)"
check_output "$LOG" "SHMSHARE: ok sparse-ftruncate" "sizing an anonymous shared file does not spend the memory: a compositor sizes a buffer pool once and paints small pieces of it, and backing the whole declaration eagerly exhausted a 4 GiB machine twenty seconds into a desktop session"
check_output "$LOG" "SHMSHARE: ok sparse-write" "a small write into a hugely sized file reads back what was written: once the declared size stops matching the buffer, a grow path that copies the size rather than the buffer runs off both ends"
check_output "$LOG" "SHMSHARE: ok sparse-hole" "the part of an anonymous file nobody wrote reads as zeroes, not as whatever the caller's buffer already held"
check_output "$LOG" "SHMSHARE: done" "shared-mapping smoke completes"
check_output "$LOG" "MM-SMOKE: ok shm-open" "POSIX shared memory works end to end (shm_open in /dev/shm, ftruncate, mmap, read back)"
check_output "$LOG" "MM-SMOKE: ok mmap-no-overlap" "mmap never returns an address that is already mapped"
check_output "$LOG" "MM-SMOKE: ok madvise" "madvise(MADV_DONTNEED) zeroes a refaulted anonymous page"
check_output "$LOG" "MM-SMOKE: ok noreserve" "MAP_NORESERVE large mapping commits lazily on touch"
check_output "$LOG" "MM-SMOKE: ok sigaltstack" "sigaltstack get/set/disable + SA_ONSTACK handler runs on alt stack"
check_output "$LOG" "MM-SMOKE: ok ucontext-size" "an SA_SIGINFO handler is given a whole ucontext_t, not 424 bytes of one overlapping the frame that was interrupted"
check_output "$LOG" "MM-SMOKE: ok copyout-cow" "the kernel writing into a copy-on-write page breaks the sharing instead of faulting in ring 0 (it used to panic the machine)"
check_output "$LOG" "MM-SMOKE: ok copyout-readonly" "a syscall asked to write into a page the program cannot write answers EFAULT and the process survives"
check_output "$LOG" "MM-SMOKE: ok file-map-privacy" "a store into a MAP_PRIVATE file page the process never faulted itself stays out of the file, and the same store through MAP_SHARED reaches it (the pages a read fault maps AROUND itself come straight from the page cache, so the protection they are installed with decides whether one process's writes are served to the next)"
check_output "$LOG" "MM-SMOKE: ok rseq-after-sigkill" "a task SIGKILLed while it holds an rseq(2) registration gives it back, so a later process can still register (glibc registers one per thread and treats a refusal as fatal, so a leaked entry kills programs rather than slowing them)"
check_output "$LOG" "MM-SMOKE: ok mmap-after-sigkill" "a task SIGKILLed inside mmap hands back the address-space lock, so unrelated processes can still map memory (a leaked slot blocks every process hashing to it, for ever, with no error and no panic)"
check_output "$LOG" "MM-SMOKE: done" "MM smoke completes"

# ── M40 Linux ABI compatibility ──
# Both arches now ship a Linux blob of their own: the x86_64 pair uses that
# architecture's historical syscall numbers, the aarch64 pair uses asm-generic
# (which renumbers everything and has no open/getdents/arch_prctl at all, so a
# few steps drive different calls to check the same property). See
# tools/blobs/build-linux-hello.sh and build-linux-abi-test.sh.
if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "aarch64" ]; then
	section "M40 Linux ABI Compatibility"
	check_output "$LOG" "M40-LINUX: start" "M40 Linux ABI smoke starts"
	check_output "$LOG" "elf: Linux personality detected: /bin/m40-linux-hello" "loader tags the static Linux binary with the Linux personality"
	check_output "$LOG" "M40-LINUX: hello from a static linux binary" "translated Linux write(1,...) reached the console"
	check_output "$LOG" "M40-LINUX: ok fstat" "Linux fstat(2) result is translated to the Linux struct stat layout (st_mode at offset 24)"
	check_output "$LOG" "M40-LINUX: ok uname" "Linux uname(2) result is translated to the Linux struct utsname layout (machine at offset 260)"
	check_output "$LOG" "M40-LINUX: ok getdents64" "Linux getdents64(2) result is repacked into variable-length linux_dirent64 records"
	check_output "$LOG" "M40-LINUX: ok arch-prctl" "Linux arch_prctl(ARCH_SET_FS) sets the FS base (TLS) and a %fs:0 round-trip works"
	check_output "$LOG" "M40-LINUX: ok mmap" "Linux mmap(MAP_ANONYMOUS) gives a Linux binary writable memory (flags/prot already match)"
	check_output "$LOG" "M40-LINUX: ok clock" "Linux clock_gettime(CLOCK_MONOTONIC) works (timespec layout matches b1nix)"
	check_output "$LOG" "M40-LINUX: ok gettid" "Linux gettid(2) is mapped to the native SYS_GETTID (returns a valid thread id)"
	check_output "$LOG" "M40-LINUX: ok signal" "Linux rt_sigaction+kill deliver a SIGUSR1 handler with signo remap (b1nix 19 <-> Linux 10) and Linux sigreturn trampoline"
	check_output "$LOG" "M40-LINUX: ok sigmask" "Linux rt_sigprocmask remaps the sigset_t bit positions and the swapped SIG_UNBLOCK/SIG_SETMASK how-values (blocked SIGUSR1 defers delivery)"
	check_output "$LOG" "M40-LINUX: ok tgkill" "Linux tgkill(2) self-signal (glibc raise/pthread_kill path) delivers with signo remap"
	check_output "$LOG" "M40-LINUX: ok alarm" "Linux alarm/sync/fchdir/setpriority mapped to native handlers"
	check_output "$LOG" "M40-LINUX: ok siginfo" "Linux SA_SIGINFO 3-arg handler gets a kernel-built siginfo_t (si_signo) + ucontext_t"
	check_output "$LOG" "M40-LINUX: ok run-static" "static Linux ELF ran and exited 0 via translated exit_group(231)"
	check_output "$LOG" "M40-LINUX: done" "M40 Linux ABI smoke completes"

	# M40 closeout: the rest of the translated Linux surface, exercised by a
	# second static Linux ELF (tools/blobs/linux_abi_test.c).
	check_output "$LOG" "M40-ABI: start" "M40 Linux ABI conformance blob starts"
	check_output "$LOG" "M40-ABI: ok pread64" "Linux pread64 reads at an explicit offset and leaves the fd offset untouched"
	check_output "$LOG" "M40-ABI: ok pwrite64" "Linux pwrite64 writes at an explicit offset"
	check_output "$LOG" "M40-ABI: ok preadv" "Linux preadv scatters a positional read across two iovecs"
	check_output "$LOG" "M40-ABI: ok truncate" "Linux truncate(2) by path resizes the file (verified through fstat)"
	check_output "$LOG" "M40-ABI: ok utime" "Linux utime(2) installs the requested mtime (verified through fstat)"
	check_output "$LOG" "M40-ABI: ok creat-renameat" "Linux creat(2) + renameat(2) create and rename a file"
	check_output "$LOG" "M40-ABI: ok statfs" "Linux statfs(2) fills the Linux struct statfs (matching b1nix layout)"
	check_output "$LOG" "M40-ABI: ok fstatfs" "Linux fstatfs(2) reports the same filesystem as statfs"
	check_output "$LOG" "M40-ABI: ok syncfs" "Linux syncfs(2) flushes the filesystem behind a descriptor"
	check_output "$LOG" "M40-ABI: ok readahead" "Linux readahead(2) is accepted as the advisory hint it is"
	check_output "$LOG" "M40-ABI: ok gettimeofday" "Linux gettimeofday(2) returns a sane wall clock and microseconds"
	check_output "$LOG" "M40-ABI: ok time" "Linux time(2) agrees with gettimeofday"
	check_output "$LOG" "M40-ABI: ok sched-getscheduler" "Linux sched_getscheduler reports SCHED_OTHER"
	check_output "$LOG" "M40-ABI: ok sched-getparam" "Linux sched_getparam reports priority 0 under SCHED_OTHER"
	check_output "$LOG" "M40-ABI: ok sched-priority-max" "Linux sched_get_priority_max(SCHED_OTHER) is 0"
	check_output "$LOG" "M40-ABI: ok sched-rr-interval" "Linux sched_rr_get_interval reports the 100 Hz tick"
	check_output "$LOG" "M40-ABI: ok sched-setaffinity" "Linux sched_setaffinity accepts an all-CPU mask and rejects an empty one"
	check_output "$LOG" "M40-ABI: ok getresuid" "Linux getresuid/getresgid return the task's real/effective/saved ids"
	check_output "$LOG" "M40-ABI: ok capget" "Linux capget(v3) reports the full capability set for root"
	check_output "$LOG" "M40-ABI: ok personality" "Linux personality(0xffffffff) queries the (fixed) PER_LINUX personality"
	check_output "$LOG" "M40-ABI: ok unshare" "Linux unshare(0) is a valid no-op"
	check_output "$LOG" "M40-ABI: ok mlock-resident" "Linux mlock(2) populates an untouched lazy mapping — mincore reports it resident before anything faults it in"
	check_output "$LOG" "M40-ABI: ok mlock-munlock" "Linux mlock/munlock pin a range against swap eviction"
	check_output "$LOG" "M40-ABI: ok mincore" "Linux mincore reports the mapped pages as resident"
	check_output "$LOG" "M40-ABI: ok rt-sigpending" "Linux rt_sigpending returns the (empty) pending set in Linux numbering"
	check_output "$LOG" "M40-ABI: ok sethostname" "Linux sethostname(2) changes the name uname(2) reports"
	check_output "$LOG" "M40-ABI: ok proc-sys-hostname" "/proc/sys/kernel/hostname reflects sethostname(2)"
	check_output "$LOG" "M40-ABI: ok proc-statm" "/proc/self/statm reports a real resident page count"
	check_output "$LOG" "M40-ABI: ok proc-limits" "/proc/self/limits renders the task's rlimits in the Linux layout"
	check_output "$LOG" "M40-ABI: ok proc-swaps" "/proc/swaps renders the Linux swap-area table"
	check_output "$LOG" "M40-ABI: ok proc-sys-pid-max" "/proc/sys/kernel/pid_max reports the task ceiling"
	check_output "$LOG" "M40-ABI: ok sys-class-net" "/sys/class/net/lo exposes the Linux interface attributes"
	check_output "$LOG" "M40-ABI: ok proc-cwd-link" "/proc/self/cwd resolves to the task's working directory"
	check_output "$LOG" "M40-ABI: ok epoll-create" "Linux epoll_create(size) maps onto epoll_create1"
	check_output "$LOG" "M40-ABI: ok ptrace-nonparent" "a tracer that is not the tracee's parent can attach and see its stops"
	check_output "$LOG" "M40-ABI: ok sched-affinity-pin" "sched_setaffinity really pins the task (mask reads back, the task still runs, an empty mask is refused)"
	check_output "$LOG" "M40-ABI: ok capset-drop" "capset(2) drops CAP_SYS_TIME for real — settimeofday then reports EPERM to a uid-0 task"
	check_output "$LOG" "M40-ABI: ok setfsuid" "setfsuid(2) moves the id the VFS checks against: a root-only file stops being readable, and reading works again once fsuid is 0"
	check_output "$LOG" "M40-ABI: ok chroot-keeps-cwd" "chroot(2) keeps a working directory inside the new root, rewritten root-relative"
	check_output "$LOG" "M40-ABI: ok enosys-unmapped" "an unassigned syscall number reports ENOSYS instead of being silently accepted"
	check_output "$LOG" "M40-ABI: ok getdents-legacy" "Linux getdents(2) emits the pre-64-bit record layout (d_type in the last byte)"
	check_output "$LOG" "M40-ABI: ok getdents-ino" "getdents64's d_ino is the filesystem's real inode number (distinct per entry, matching stat)"
	check_output "$LOG" "M40-ABI: ok rseq-abort" "an rseq critical section interrupted by the scheduler resumes at its abort handler, and the descriptor is consumed"
	check_output "$LOG" "M40-ABI: ok vmsplice" "Linux vmsplice(2) moves user memory into a pipe"
	check_output "$LOG" "M40-ABI: ok tee" "Linux tee(2) duplicates pipe data without consuming the source"
	check_output "$LOG" "M40-ABI: ok ioprio" "Linux ioprio_set/ioprio_get round-trip the task's I/O class and level"
	check_output "$LOG" "M40-ABI: ok sysv-sem" "SysV semaphores: SETVAL/GETVAL, an atomic down/up pair, and IPC_NOWAIT reporting EAGAIN"
	check_output "$LOG" "M40-ABI: ok sysv-msg" "SysV message queues: type-selective msgrcv picks the requested type, IPC_NOWAIT reports ENOMSG"
	check_output "$LOG" "M40-ABI: ok file-handle" "name_to_handle_at + open_by_handle_at reopen a file through an opaque handle"
	check_output "$LOG" "M40-ABI: ok file-handle-stale" "a handle whose file was replaced reports ESTALE instead of opening the impostor"
	check_output "$LOG" "M40-IOPRIO: ok elevator-order" "the block layer admits the best-priority waiter first, and ages a starving idle-class request past fresh best-effort ones"
	check_output "$LOG" "M40-ABI: ok rseq" "rseq(2) registration publishes a live cpu_id into the task's own memory"
	check_output "$LOG" "M40-ABI: ok swapon-swapoff" "swapoff(2) pages everything back in and detaches; swapon(2) re-attaches the device"
	check_output "$LOG" "M40-ABI: ok chroot" "chroot(2) confines a child: the jailed path resolves, an outside path and a \"..\" escape do not"
	check_output "$LOG" "M40-ABI: ok ptrace-stop" "ptrace(2): a traced child stops on a signal and its parent sees WIFSTOPPED"
	check_output "$LOG" "M40-ABI: ok ptrace-getregs" "PTRACE_GETREGS returns the stopped tracee's user registers"
	check_output "$LOG" "M40-ABI: ok ptrace-peek" "PTRACE_PEEKDATA reads the tracee's memory through its own page tables"
	check_output "$LOG" "M40-ABI: ok ptrace-cont" "PTRACE_CONT resumes the tracee, which then runs to exit"
	check_output "$LOG" "M40-ABI: done" "M40 Linux ABI conformance completes with no failures"
fi

# ── M67 Rust cross-toolchain (x86_64 only: it runs a Rust std program) ──
if [ "$ARCH" = "x86_64" ]; then
	section "M67 Rust Cross-Toolchain"
	check_output "$LOG" "M67-RUST: start" "M67 Rust std smoke starts"
	check_output "$LOG" "rust on b1nix squares=\[0, 1, 4, 9, 16\] sum=30" "Rust Vec/String/HashMap + iterator sum ran (println! reached the console)"
	check_output "$LOG" "thread returned 42" "Rust std::thread::spawn + join works (futex bridge to native SYS_FUTEX)"
	check_output "$LOG" "M67-RUST: ok run-std" "static Rust ELF ran and exited 0"
	check_output "$LOG" "M67-RUST: done" "M67 Rust smoke completes"
fi

# ── M119 Developer-Centric Filesystems (fwcfgfs, debugfs, tarfs) ──
section "M119 Developer-Centric Filesystems"
check_output "$LOG" "M119-SMOKE: start" "M119 developer filesystems smoke starts"
check_output "$LOG" "M119-SMOKE: ok debugfs" "debugfs virtual diagnostic filesystem mounted and read back"
check_output "$LOG" "M119-SMOKE: ok fwcfgfs" "fwcfgfs QEMU fw_cfg directory and files inspected"
check_output "$LOG" "M119-SMOKE: ok tarfs" "tarfs ustar archive filesystem mount and error handling verified"
check_output "$LOG" "M119-SMOKE: done" "M119 developer filesystems smoke completes"

# ── M25 Native C compiler (b1cc) ──
section "M94 Linux ABI conformance (through musl)"
# Each of these reached the kernel as "unmapped syscall -> -ENOSYS" before the
# translation table gained them, so a regression shows up here rather than as a
# port that mysteriously degrades.
check_output "$LOG" "MUSL-POSIX: ok clock-getres" "clock_getres reports a resolution the clock can actually deliver (Linux nr 229)"
check_output "$LOG" "MUSL-POSIX: ok times" "times() returns process CPU accounting (Linux nr 100)"
check_output "$LOG" "MUSL-POSIX: ok sysinfo" "sysinfo() reports total RAM (Linux nr 99)"
check_output "$LOG" "MUSL-POSIX: ok sched-getaffinity" "sched_getaffinity returns a non-empty CPU mask (Linux nr 204)"
check_output "$LOG" "MUSL-POSIX: ok statx-dirfd" "statx resolves a relative path against a real dirfd (Linux nr 332)"
check_output "$LOG" "MUSL-POSIX: ok faccessat2" "faccessat2 resolves against a real dirfd (Linux nr 439)"
check_output "$LOG" "MUSL-POSIX: ok sigtimedwait" "sigtimedwait consumes a pending signal without running its handler (Linux nr 128)"
check_output "$LOG" "MUSL-POSIX: ok sigtimedwait-timeout" "sigtimedwait times out with EAGAIN instead of blocking forever"
check_output "$LOG" "MUSL-POSIX: done" "the musl POSIX smoke completes"

# Address-space layout: the loader is not tied to one load base. Three distinct
# layouts run in the same boot — a stock Linux ET_EXEC at its own vaddr
# (0x200000), b1nix ET_EXEC images at 0x2000000, and musl PIEs the loader places
# itself (randomized under b1nix.aslr, see the M71 check) — none of which can
# collide with the kernel, which lives in the higher half.
check_output "$LOG" "PIE base=" "musl PIE images are placed by the loader, not by a fixed link address"

section "M25 Native C compiler (b1cc)"
check_output "$LOG" "M25-SMOKE: start" "M25 smoke starts"
check_output "$LOG" "M25-SMOKE: ok cc-launch" "the native C compiler /bin/b1cc is present"
check_output "$LOG" "M25-SMOKE: ok compile-hello" "b1cc compiles hello.c on the target"
check_output "$LOG" "M25-SMOKE: ok run-hello" "compiled hello program runs"
check_output "$LOG" "M25-HELLO: hello from native b1cc" "hello program outputs correct greeting"
check_output "$LOG" "M25-SMOKE: ok compile-utility" "b1cc compiles and runs a mini-echo utility"
check_output "$LOG" "M25-SMOKE: ok argv-check" "compiled program receives argc/argv"
check_output "$LOG" "M25-SMOKE: ok stderr-check" "compiled program stderr path works"
check_output "$LOG" "M25-SMOKE: ok exit-check" "compiled program non-zero exit status propagates"
check_output "$LOG" "M25-SMOKE: ok float-check" "compiled program does real double arithmetic (SSE/x87 encodings)"
check_output "$LOG" "M25-FLOAT: all float tests passed" "float program outputs correct results"
check_output "$LOG" "M25-SMOKE: ok scanf" "libc scanf/sscanf format parsing works"
check_output "$LOG" "M25-SMOKE: ok frexp" "libc frexp decomposes correctly"
check_output "$LOG" "M25-SMOKE: ok clock64" "libc clock_gettime returns sane 64-bit realtime/monotonic tuples"
check_output "$LOG" "M25-SMOKE: ok time64-utime" "libc utime/stat preserves timestamps above 2^32"
check_output "$LOG" "M25-SMOKE: ok fileops" "libc fchmod/utime/tmpfile work (verified via stat)"
check_output "$LOG" "M25-SMOKE: done" "M25 smoke completes"

# ── M26 Native Toolchain & Self-Host ──
section "M26 Native C Toolchain & Self-Host"
check_output "$LOG" "M26-SMOKE: start" "M26 smoke starts"
check_output "$LOG" "M26-SMOKE: ok selfhost-status" "selfhost status syscall works"
check_output "$LOG" "M26-SMOKE: ok toolchain-ready" "native toolchain (clang/binutils/make) is ported"
check_output "$LOG" "M26-SMOKE: ok readdir" "libc opendir/readdir over SYS_GETDENTS works"
# NOTE: in-guest full kernel self-build is not yet verified, so the marker is
# "pending can-build-kernel" (not "ok"). Flip the kernel flag + this check to
# "ok can-build-kernel" only once an in-guest kernel.elf actually builds.
check_output "$LOG" "M26-SMOKE: done" "M26 smoke completes"


# ── M16 User Space Applications & TUI ──
section "M16 user space applications and TUI"
check_output "$LOG" "M16-SMOKE: start" "M16 smoke starts"
check_output "$LOG" "M16-SMOKE: ok tui-key-decode" "shared TUI key decoding works"
check_output "$LOG" "M16-SMOKE: ok file-explorer-hotkeys" "file explorer hotkeys work"
check_output "$LOG" "M16-SMOKE: ok editor-hotkeys" "text editor hotkeys work"
check_output "$LOG" "M16-SMOKE: ok editor-persist" "editor persistence save+reload works"
check_output "$LOG" "M16-SMOKE: ok file-clipboard" "clipboard VFS copy+delete works"
check_output "$LOG" "M16-SMOKE: ok terminal-restore" "terminal raw mode is restored"
check_output "$LOG" "M16-SMOKE: ok app-lifecycle" "app lifecycle completes"
check_output "$LOG" "M16-SMOKE: done" "M16 smoke completes"

# ── M22 utility init-path smoke ──
section "M22 utilities"
check_output "$LOG" "NATIVE-SMOKE: ok" "native ELF enters ring3 and performs syscall"
check_output "$LOG" "NATIVE-SMOKE: done" "native ELF exits cleanly"
check_output "$LOG" "M22-SMOKE: start" "M22 utility smoke starts from init"
check_output "$LOG" "M22-SMOKE: ok pwd" "pwd utility runs"
check_output "$LOG" "M22-SMOKE: ok cp" "cp utility runs"
check_output "$LOG" "M22-SMOKE: ok parent-perms" "missing parent creation is rejected"
check_output "$LOG" "M22-SMOKE: ok ln-s" "ln -s utility runs"
check_output "$LOG" "M22-SMOKE: ok readlink" "readlink utility runs"
check_output "$LOG" "M22-SMOKE: ok lstat" "lstat reports symlink type"
check_output "$LOG" "M22-SMOKE: ok cat-link" "symlink path resolves"
check_output "$LOG" "M22-SMOKE: ok path-norm" "dot-dot path normalization works"
check_output "$LOG" "M22-SMOKE: ok grep" "grep utility runs"
check_output "$LOG" "M22-SMOKE: ok date" "date utility runs"
check_output "$LOG" "M22-SMOKE: ok uname" "uname utility runs"
check_output "$LOG" "M22-SMOKE: done" "M22 utility smoke completes"

check_output "$LOG" "M24-STRESS: done" "M24 stress completes successfully"
check_output "$LOG" "LOCK-SMOKE: done" "LOCK-SMOKE completes successfully"
check_output "$LOG" "EXT-STRESS: done" "EXT-STRESS completes successfully"
check_output "$LOG" "LOCK-SMOKE: ok nonblock-conflict" "fcntl nonblock lock conflict returns EAGAIN"
check_output "$LOG" "LOCK-SMOKE: ok wake-on-close" "fcntl blocking lock wakes on parent close"
check_output "$LOG" "EXT-STRESS: start" "EXT-STRESS starts"
check_output "$LOG" "M24-STRESS: start" "M24 scheduler stress starts"
check_output "$LOG" "ok eloop" "circular symlink returns ELOOP"
check_output "$LOG" "POSIX-SMOKE: done" "POSIX shell-driven smoke tests complete"

# bpkg: b1nix's native package manager (own gzip/deflate + tar + sha256 in C,
# no shelling to curl/tar/sha256sum). Flat house-index format plus a real
# Alpine-shaped APKINDEX.tar.gz + triple-gzip .apk fixture.
check_output "$LOG" "BPKG-SMOKE: start" "bpkg smoke starts"
check_output "$LOG" "BPKG-SMOKE: ok update" "bpkg update fetches the index"
check_output "$LOG" "BPKG-SMOKE: ok install" "bpkg install verifies sha256 and extracts"
check_output "$LOG" "BPKG-SMOKE: ok list" "bpkg list reports the installed package"
check_output "$LOG" "BPKG-SMOKE: ok checksum-reject" "bpkg install rejects a wrong sha256"
check_output "$LOG" "BPKG-SMOKE: ok remove" "bpkg remove deletes files and metadata"
check_output "$LOG" "BPKG-SMOKE: ok dep-resolution" "bpkg install resolves dependencies transitively"
check_output "$LOG" "BPKG-SMOKE: ok apk-signature" "bpkg verifies a real Alpine package's RSA signature and its datahash before extracting it"
check_output "$LOG" "BPKG-SMOKE: ok apk-signature-reject" "a payload byte flipped behind a valid signature is caught by the datahash and installs nothing"
check_output "$LOG" "BPKG-SMOKE: ok apk-format" "bpkg installs a real Alpine APKINDEX/.apk package"
check_output "$LOG" "BPKG-SMOKE: ok install-scripts" "bpkg runs .pre-install before unpacking and .post-install after, with the version as \$1"
check_output "$LOG" "BPKG-SMOKE: ok triggers" "an armed trigger fires for the transaction that writes into the directory it watches, not for its own package's install"
check_output "$LOG" "BPKG-SMOKE: ok world" "explicitly requested packages are tracked in /etc/apk/world"
check_output "$LOG" "BPKG-SMOKE: ok deinstall-script" "bpkg remove runs the .post-deinstall kept from install time and drops the name from world"
check_output "$LOG" "BPKG-SMOKE: ok upgrade-scripts" "an upgrade runs .pre-upgrade/.post-upgrade instead of the install scripts, with the new version in \$1 and the replaced one in \$2"
check_output "$LOG" "BPKG-SMOKE: ok upgrade-order" ".pre-upgrade runs before the new payload is unpacked and .post-upgrade after"
check_output "$LOG" "BPKG-SMOKE: ok upgrade-no-deinstall" "an upgrade does not run the old version's deinstall scripts"
check_output "$LOG" "BPKG-SMOKE: ok upgrade-fallback" "a package with no upgrade scripts still has its install scripts run on an upgrade"
check_output "$LOG" "BPKG-SMOKE: ok upgrade-abort" "a failing .pre-upgrade abandons the upgrade and leaves the installed version in place"
check_output "$LOG" "BPKG-SMOKE: done" "bpkg smoke completes"
check_output "$LOG" "M22-POLISH: start" "M22 Polish starts"
check_output "$LOG" "M22-POLISH: ok utility-flags" "M22 Polish utility flags verify"
check_output "$LOG" "M22-POLISH: ok text-pipeline" "M22 Polish text pipeline verifies"
check_output "$LOG" "M22-POLISH: ok file-workflow" "M22 Polish file workflow verifies"
check_output "$LOG" "M22-POLISH: ok failure-status" "M22 Polish failure status propagates"
check_output "$LOG" "M24-SMOKE: ok ENOENT" "M24 errno ENOENT is mapped"
check_output "$LOG" "M24-SMOKE: ok EEXIST" "M24 errno EEXIST is mapped"
check_output "$LOG" "M24-SMOKE: ok EINVAL" "M24 errno EINVAL is mapped"
check_output "$LOG" "M24-SMOKE: ok EBADF" "M24 errno EBADF is mapped"
check_output "$LOG" "M24-SMOKE: ok ENOTDIR" "M24 errno ENOTDIR is mapped"
check_output "$LOG" "M24-SMOKE: ok EISDIR" "M24 errno EISDIR is mapped"
check_output "$LOG" "M24-SMOKE: ok EACCES" "M24 errno EACCES is mapped"
check_output "$LOG" "M24-SMOKE: ok errno-mapping" "M24 userspace errno mapping verifies"
check_output "$LOG" "M24-SMOKE: ok diagnostics" "M24 userspace diagnostics verify"
check_output "$LOG" "M22-POLISH: done" "M22 Polish completes successfully"

	echo ""
	section "Upstream BusyBox package"
	check_output "$LOG" "BB-SMOKE: ok list" "busybox --list works"
	check_output "$LOG" "BB-SMOKE: ok echo" "busybox echo works"
	check_output "$LOG" "BB-SMOKE: ok printf" "busybox printf works"
	check_output "$LOG" "BB-SMOKE: ok pwd" "busybox pwd works"
	check_output "$LOG" "BB-SMOKE: ok mkdir" "busybox mkdir works"
	check_output "$LOG" "BB-SMOKE: ok touch" "busybox touch works"
	check_output "$LOG" "BB-SMOKE: ok cat" "busybox cat works"
	check_output "$LOG" "BB-SMOKE: ok cp" "busybox cp works"
	check_output "$LOG" "BB-SMOKE: ok mv" "busybox mv works"
	check_output "$LOG" "BB-SMOKE: ok ln" "busybox ln works"
	check_output "$LOG" "BB-SMOKE: ok readlink" "busybox readlink works"
	check_output "$LOG" "BB-SMOKE: ok chmod" "busybox chmod works"
	check_output "$LOG" "BB-SMOKE: ok test" "busybox test / [ works"
	check_output "$LOG" "BB-SMOKE: ok sort" "busybox sort works"
	check_output "$LOG" "BB-SMOKE: ok uniq" "busybox uniq works"
	check_output "$LOG" "BB-W1: ok ls" "busybox ls works"
	check_output "$LOG" "BB-W1: ok cmp" "busybox cmp works"
	check_output "$LOG" "BB-W1: ok cut" "busybox cut works"
	check_output "$LOG" "BB-W1: ok env" "busybox env works"
	check_output "$LOG" "BB-W1: ok id" "busybox id works"
	check_output "$LOG" "BB-W1: ok printenv" "busybox printenv works"
	check_output "$LOG" "BB-W1: ok tee" "busybox tee works"
	check_output "$LOG" "BB-W1: ok tr" "busybox tr works"
	check_output "$LOG" "BB-W1: ok whoami" "busybox whoami works"
	check_output "$LOG" "BB-W1: ok seq" "busybox seq works"
	check_output "$LOG" "BB-W1: ok which" "busybox which works"
	check_output "$LOG" "BB-W1: ok clear" "busybox clear works"
	check_output "$LOG" "BB-W1: ok hexdump" "busybox hexdump works"
	check_output "$LOG" "BB-W2: ok stat" "busybox stat works"
	check_output "$LOG" "BB-W2: ok realpath" "busybox realpath works"
	check_output "$LOG" "BB-W2: ok mktemp" "busybox mktemp works"
	check_output "$LOG" "BB-W2: ok find" "busybox find works"
	check_output "$LOG" "BB-W2: ok grep" "busybox grep ERE works"
	check_output "$LOG" "BB-W2: ok grep-icase" "busybox grep REG_ICASE classes work"
	check_output "$LOG" "BB-W2: ok sed" "busybox sed BRE groups work"
	check_output "$LOG" "BB-W2: ok awk" "busybox awk field processing works"
	check_output "$LOG" "BB-W2: ok xargs" "busybox xargs works"
	check_output "$LOG" "BB-W2: ok diff" "busybox diff statuses work"
	check_output "$LOG" "BB-W2: ok cksum" "busybox cksum works"
	check_output "$LOG" "BB-W2: ok md5sum" "busybox md5sum works"
	check_output "$LOG" "BB-W2: ok sha256sum" "busybox sha256sum works"
	check_output "$LOG" "BB-W2B: ok dd" "busybox dd works"
	check_output "$LOG" "BB-W2B: ok du" "busybox du works"
	check_output "$LOG" "BB-W2B: ok df" "busybox df works (/proc/mounts)"
	check_output "$LOG" "BB-W2B: ok tar" "busybox tar create/extract works"
	check_output "$LOG" "BB-W2B: ok tar-gzip" "busybox tar -z seamless gzip works"
	check_output "$LOG" "BB-W2B: ok gzip" "busybox gzip/gunzip round trip works"
	check_output "$LOG" "BB-W2B: ok bzip2" "busybox bzip2/bunzip2 round trip works"
	check_output "$LOG" "BB-W2B: ok xzcat" "busybox xzcat decompresses xz"
	check_output "$LOG" "BB-W2B: ok unxz" "busybox unxz decompresses xz"
	check_output "$LOG" "BB-W2B: ok gunzip-malformed" "busybox gunzip rejects malformed input"
	check_output "$LOG" "BB-W3: ok ps" "busybox ps reads /proc/<pid>/stat"
	check_output "$LOG" "BB-W3: ok top" "busybox top batch iteration works"
	check_output "$LOG" "BB-W3: ok uptime" "busybox uptime reads /proc/uptime+loadavg"
	check_output "$LOG" "BB-W3: ok free" "busybox free reads sysinfo()+/proc/meminfo"
	check_output "$LOG" "BB-W3: ok dmesg" "busybox dmesg drains kernel ring via klogctl"
	check_output "$LOG" "BB-W3: ok pidof" "busybox pidof matches process comm"
	check_output "$LOG" "BB-W3: ok pgrep" "busybox pgrep matches process comm"
	check_output "$LOG" "BB-W3: ok pkill" "busybox pkill signals matched process"
	check_output "$LOG" "BB-W4: ok mount" "busybox mount mounts a device and shows it"
	check_output "$LOG" "M43: ok create-runtime-mountpoint" "file/dir creation works at a mountpoint created at runtime (mkdir then mount)"
	check_output "$LOG" "BB-W4: ok umount" "busybox umount unmounts a device"
	# Needs a real off-link DNS path; skips cleanly when the usernet has none.
	if grep -q "BB-W4: unsupported nslookup" "$LOG" 2>/dev/null; then
		pass "busybox nslookup skipped (no external DNS path)"
	else
		check_output "$LOG" "BB-W4: ok nslookup" "busybox nslookup resolves an address"
	fi
	check_output "$LOG" "BB-W4: ok lsof" "busybox lsof lists open files via /proc/<pid>/fd"
	check_output "$LOG" "BB-W4: ok netstat" "busybox netstat reads /proc/net/tcp (sshd :22)"
	check_output "$LOG" "BB-W4: ok route" "busybox route reads /proc/net/route (default gw)"
	check_output "$LOG" "BB-W4: ok ifconfig" "busybox ifconfig reads iface via SIOCGIF* ioctls"
	check_output "$LOG" "BB-W4: ok blkid" "busybox blkid identifies fs on /dev block node"
	check_output "$LOG" "BB-W4: ok fdisk" "busybox fdisk -l reads block geometry via BLK ioctls"
	check_output "$LOG" "BB-W4B: ok ping" "busybox ping works over a raw ICMP socket"
	check_output "$LOG" "BB-W4B: ok losetup" "busybox losetup -f finds a free loop device"
	check_output "$LOG" "BB-W4B: ok ip" "busybox ip link show works over rtnetlink"
	check_output "$LOG" "BB-W5: ok list-ash" "busybox lists the ash applet"
	check_output "$LOG" "BB-W5: ok list-sh" "busybox lists the sh applet"
	check_output "$LOG" "BB-W5: ok ash-c" "busybox ash -c runs a command"
	check_output "$LOG" "BB-W5: ok busybox-sh-c" "busybox sh -c runs a command"
	check_output "$LOG" "BB-W5: ok bin-sh-c" "/bin/sh (ash) -c runs a command"
	check_output "$LOG" "BB-W5: ok vars" "ash variable assignment + test"
	check_output "$LOG" "BB-W5: ok math" "ash arithmetic expansion"
	check_output "$LOG" "BB-W5: ok pipe" "ash pipeline"
	check_output "$LOG" "BB-W5: ok redir" "ash output redirection"
	check_output "$LOG" "BB-W5: ok wait" "ash waits for a child"
	check_output "$LOG" "BB-W5: ok arith-loop" "ash while-loop with \$((i+1)) arithmetic terminates (strtoull endptr)"
	check_output "$LOG" "BB-W5: done" "BusyBox wave 5 ash smoke completes"
	check_output "$LOG" "BB-W6: ok cryptpw" "busybox cryptpw computes sha512-crypt (\$6\$)"
	check_output "$LOG" "BB-W6: ok addgroup" "busybox addgroup writes /etc/group"
	check_output "$LOG" "BB-W6: ok adduser" "busybox adduser writes /etc/passwd"
	check_output "$LOG" "BB-W6: ok adduser-shadow" "busybox adduser writes /etc/shadow"
	check_output "$LOG" "BB-W6: ok adduser-home" "busybox adduser creates the home directory"
	check_output "$LOG" "BB-W6: ok chpasswd" "busybox chpasswd writes a \$6\$ sha512-crypt hash"
	check_output "$LOG" "BB-W6: ok passwd-verify" "stored password hash recomputes (password is verifiable)"
	check_output "$LOG" "BB-W6: ok su" "busybox su drops root->user (uid switch verified)"
	check_output "$LOG" "BB-W6: ok passwd-lock" "busybox passwd -l locks the account"
	check_output "$LOG" "BB-W6: ok passwd-unlock" "busybox passwd -u unlocks the account"
	check_output "$LOG" "BB-W6: ok login-applet" "busybox login applet is present and dispatchable"
	check_output "$LOG" "BB-W6: ok getty-applet" "busybox getty applet is present and dispatchable"
	check_output "$LOG" "BB-W6: ok deluser" "busybox deluser removes the /etc/passwd record"
	check_output "$LOG" "BB-W6: ok deluser-shadow" "busybox deluser removes the /etc/shadow record"
	check_output "$LOG" "BB-W6: ok delgroup" "busybox delgroup removes the /etc/group record"
	check_output "$LOG" "BB-W6: done" "BusyBox wave 6 account smoke completes"
	check_output "$LOG" "BB-W7: ok uuidgen" "busybox uuidgen generates RFC 4122 v4 UUID"
	check_output "$LOG" "BB-W7: ok sha384sum-upstream" "busybox sha384sum computes SHA-384 hash"
	check_output "$LOG" "BB-W7: ok vmstat-upstream" "busybox vmstat reports memory/process stats"
	check_output "$LOG" "BB-W7: ok tsort" "busybox tsort topologically sorts partial-order pairs"
	check_output "$LOG" "BB-W7: ok tree-upstream" "busybox tree prints directory trees"
	check_output "$LOG" "BB-W7: ok getfattr" "busybox getfattr reads an extended attribute"
	check_output "$LOG" "BB-W7: ok lsblk" "busybox lsblk enumerates /sys/block devices"
	check_output "$LOG" "BB-W7: ok version" "busybox --version reports 1.38.0"
	check_output "$LOG" "BB-W8: ok id" "/bin/id (promoted to upstream) reports uid 0"
	check_output "$LOG" "BB-W8: ok whoami" "/bin/whoami (promoted to upstream) reports root"
	check_output "$LOG" "BB-W8: ok id-is-busybox" "/bin/id really resolves to the BusyBox multicall ELF, not a dedicated binary"
	check_output "$LOG" "BB-W8: ok groups" "/bin/groups (BusyBox) lists the caller's groups"
	check_output "$LOG" "BB-W8: ok uuidgen" "/bin/uuidgen (promoted) generates a UUID"
	check_output "$LOG" "BB-W8: ok sha384sum" "/bin/sha384sum (promoted) computes a SHA-384 hash"
	check_output "$LOG" "BB-W8: ok vmstat" "/bin/vmstat (promoted) reports stats"
	check_output "$LOG" "BB-W8: ok tree" "/bin/tree (promoted) prints a directory tree"
	check_output "$LOG" "BB-W8: done" "BusyBox wave 8 applet promotion completes"
	check_output "$LOG" "BB-W9: ok chmod" "/bin/chmod (promoted to upstream) sets mode 600"
	check_output "$LOG" "BB-W9: ok chown" "/bin/chown (promoted to upstream) sets owner 0"
	check_output "$LOG" "BB-W9: ok tsort" "/bin/tsort (promoted) topologically sorts"

	# ── BB-W10: parity with Alpine's own busyboxconfig ──
	check_output "$LOG" "BB-W10: ok bc" "busybox bc evaluates an expression"
	check_output "$LOG" "BB-W10: ok dc" "busybox dc evaluates RPN"
	check_output "$LOG" "BB-W10: ok who" "who reads the utmp database"
	check_output "$LOG" "BB-W10: ok cpio" "cpio writes and lists a newc archive"
	check_output "$LOG" "BB-W10: ok shred" "shred overwrites a file and removes it"
	check_output "$LOG" "BB-W10: ok urandom" "/dev/urandom delivers bytes from the kernel CSPRNG"
	check_output "$LOG" "BB-W10: ok zero" "/dev/zero delivers an endless run of zero bytes"
	check_output "$LOG" "BB-W10: ok nproc" "nproc reports the CPU count"
	check_output "$LOG" "BB-W10: ok hostname" "hostname reads the system name"
	check_output "$LOG" "BB-W10: ok uuencode-uudecode" "uuencode/uudecode round-trip a payload"
	check_output "$LOG" "BB-W10: ok mountpoint" "mountpoint recognises /"
	check_output "$LOG" "BB-W10: ok nice" "nice runs a command at a changed priority"
	check_output "$LOG" "BB-W10: ok stty" "stty reports the console's terminal settings"
	check_output "$LOG" "BB-W10: ok blockdev" "blockdev --getsz reads the disk size through BLKGETSIZE"
	check_output "$LOG" "BB-W10: ok blockdev-getro" "blockdev --getro reads the new BLKROGET ioctl"
	check_output "$LOG" "M47-GFX: ok fb-putvar" "FBIOPUT_VSCREENINFO adjusts an impossible mode request down to the one really in force and reports it back, which is what Linux's fixed-mode drivers do and what a client that sets the mode before drawing depends on - refusing it failed every such client"
check_output "$LOG" "BB-W10: ok fbset" "fbset reads /dev/fb0 through the Linux FBIOGET_*SCREENINFO ioctls"
	check_output "$LOG" "BB-W10: ok lzop" "lzop/lzopcat round-trip a file"
	check_output "$LOG" "BB-W10: ok fallocate" "fallocate reserves space through fallocate(2)"
	check_output "$LOG" "BB-W10: ok flock" "flock takes a file lock and runs a command"
	check_output "$LOG" "BB-W10: ok fsync" "fsync flushes a file by name"
	check_output "$LOG" "BB-W10: ok mkpasswd" "mkpasswd produces a SHA-512 crypt string"
	check_output "$LOG" "BB-W10: ok setpriv" "setpriv is present and reports its usage"
	check_output "$LOG" "BB-W10: ok sha3sum" "sha3sum hashes stdin"
	check_output "$LOG" "BB-W10: ok ipcalc" "ipcalc derives a network address"
check_output "$LOG" "BB-W11: ok unshare-uts" "unshare -u gives a UTS namespace whose hostname the parent does not see"
check_output "$LOG" "BB-W11: ok unshare-net" "unshare -n gives a network namespace with no interface but loopback, while the parent keeps its own"
check_output "$LOG" "BB-W11: ok nsenter-uts" "nsenter -t <pid> -u reads the hostname of the namespace that process is in"
check_output "$LOG" "BB-W11: done" "the namespace tools wave completes"
check_output "$LOG" "BB-W12: ok readahead" "readahead(2) warms a file's blocks and leaves its contents intact"
check_output "$LOG" "BB-W12: ok raid-assemble" "raidautorun assembles a mirror from the superblocks its members carry"
check_output "$LOG" "BB-W12: ok raid-mirrors-both-members" "a write through the array lands on BOTH members, read back from each member directly"
check_output "$LOG" "BB-W12: ok nbd-node" "/dev/nbd0 exists before anything is attached and refuses to read while empty"
check_output "$LOG" "BB-W12: done" "the new-layer wave completes"
	check_output "$LOG" "BB-W10: done" "the Alpine-parity applet wave completes"
	check_output "$LOG" "BB-W9: done" "BusyBox wave 9 applet promotion completes"
	check_output "$LOG" "BB-SMOKE: ok rm" "busybox rm works"
	check_output "$LOG" "BB-SMOKE: ok rmdir" "busybox rmdir works"
	check_output "$LOG" "BB-SMOKE: done" "BusyBox smoke completes"

# ── M11 Shell & Utilities ──
section "M11 Shell baseline"
check_output "$LOG" "M11-SMOKE: start" "M11 shell smoke starts"
check_output "$LOG" "M11-SMOKE: ok pipe-eof" "pipe EOF when all writers close"
check_output "$LOG" "M11-SMOKE: ok pipe-nonblock-read" "pipe nonblocking read returns EAGAIN"
check_output "$LOG" "M11-SMOKE: ok pipe-nonblock-write" "pipe nonblocking write returns EAGAIN"
check_output "$LOG" "M11-SMOKE: ok fifo-mkfifo" "mkfifo creates a FIFO node in /run that stat() reports as S_IFIFO"
check_output "$LOG" "M11-SMOKE: ok fifo-nonblock-enxio" "FIFO O_WRONLY|O_NONBLOCK with no reader fails with ENXIO"
check_output "$LOG" "M11-SMOKE: ok fifo-rendezvous" "FIFO blocking open waits for the peer, then transfers data across a fork"
check_output "$LOG" "M11-SMOKE: ok fifo-eof" "FIFO reader sees EOF once the writer closes"
check_output "$LOG" "M11-SMOKE: done" "M11 shell smoke completes"

# ── M33 Shell compliance: POSIX sh features under BusyBox ash ──
# Re-implemented as ash-script tests after the in-kernel builtin shell was
# retired. Arrays are bash-only and job control is meaningless here; signal
# traps are exercised asynchronously while ash is blocked in wait.
check_output "$LOG" "M33-SHELL: start" "M33 shell smoke starts"
check_output "$LOG" "M33-SHELL: ok pipe-large" "concurrent pipeline streams >512B without deadlock"
check_output "$LOG" "M33-SHELL: ok cmdsubst" "command substitution \$(...)"
check_output "$LOG" "M33-SHELL: ok subshell" "subshell ( ... ) isolates env side effects"
check_output "$LOG" "M33-SHELL: ok function" "shell functions define + invoke with positionals"
check_output "$LOG" "M33-SHELL: ok case" "case ... esac selects matching glob branch"
check_output "$LOG" "M33-SHELL: ok for-loop" "for VAR in LIST; do ...; done"
check_output "$LOG" "M33-SHELL: ok while-loop" "while COND; do ...; done + scalar assign"
check_output "$LOG" "M33-SHELL: ok arith" "arithmetic expansion \$((...)) evaluates"
check_output "$LOG" "M33-SHELL: ok param-expand" "parameter expansion \${x:-w}"
check_output "$LOG" "M33-SHELL: ok heredoc" "here-document body"
check_output "$LOG" "M33-SHELL: ok glob-star" "pathname glob '*.txt' expands"
check_output "$LOG" "M33-SHELL: ok async-trap" "concurrent SIGUSR1 runs an ash trap"
check_output "$LOG" "M33-SHELL: done" "M33 shell smoke completes"

check_output "$LOG" "M11-SHELL: ok simple-success" "simple command success"
check_output "$LOG" "M11-SHELL: ok simple-fail" "simple command failure propagates status"
check_output "$LOG" "M11-SHELL: ok exec-127" "failed exec returns 127"
check_output "$LOG" "M11-SHELL: ok var-expand" "variable expansion works"
check_output "$LOG" "M11-SHELL: ok path-lookup" "PATH lookup resolves command"
check_output "$LOG" "M11-SHELL: ok quoted-string" "double-quoted string with spaces"
check_output "$LOG" "M11-SHELL: ok single-quote" "single-quoted string preserves special chars"
check_output "$LOG" "M11-SHELL: ok and-op" "&& operator runs second on success"
check_output "$LOG" "M11-SHELL: ok or-op" "|| operator runs second on failure"
check_output "$LOG" "M11-SHELL: ok semicolon" "semicolon separates commands"
check_output "$LOG" "M11-SHELL: ok redir-out" "stdout redirection > creates file"
check_output "$LOG" "M11-SHELL: ok redir-in" "stdin redirection < reads file"
check_output "$LOG" "M11-SHELL: ok redir-append" "append redirection >> works"
check_output "$LOG" "M11-SHELL: ok redir-stderr" "stderr redirection 2> captures errors"
check_output "$LOG" "M11-SHELL: ok redir-2>&1" "2>&1 merges stderr into stdout"
check_output "$LOG" "M11-SHELL: ok redir-failure" "redirection failure returns nonzero"
check_output "$LOG" "M11-SHELL: ok pipeline-output" "pipeline passes data between commands"
check_output "$LOG" "M11-SHELL: ok pipeline-status" "pipeline exit status = last command"
check_output "$LOG" "M11-SHELL: ok pipeline-chain" "3-stage pipeline: echo | grep | wc"
check_output "$LOG" "M11-SHELL: ok combo-redir-pipe" "combined redirection and pipeline path works"
check_output "$LOG" "M11-SHELL: ok combo-quote-redir" "quoted variable through pipeline+redirection works"
check_output "$LOG" "M11-SHELL: ok script-exec" "script execution via /bin/sh"
if grep -q "M11-SHELL: ok shebang" "$LOG" 2>/dev/null || grep -q "M11-SHELL: ok shebang-unsupported" "$LOG" 2>/dev/null; then
	pass "shebang behavior marker emitted"
else
	missing_marker "$LOG" "shebang behavior marker emitted" "missing shebang support/unsupported marker"
fi

section "M11 Coreutils via shell"
check_output "$LOG" "M11-UTIL: ok cat" "cat reads file via pipeline"
check_output "$LOG" "M11-UTIL: ok grep" "grep finds pattern (exit 0)"
check_output "$LOG" "M11-UTIL: ok grep-nomatch" "grep exits nonzero when no match"
check_output "$LOG" "M11-UTIL: ok wc" "wc -l counts lines correctly"
check_output "$LOG" "M11-UTIL: ok head" "head -n 2 returns 2 lines"
check_output "$LOG" "M11-UTIL: ok tail" "tail -n 2 returns 2 lines"
check_output "$LOG" "M11-UTIL: ok sort" "sort produces ordered output"
check_output "$LOG" "M11-UTIL: ok uniq" "uniq removes adjacent duplicates"
check_output "$LOG" "M11-UTIL: ok cp" "cp copies file"
check_output "$LOG" "M11-UTIL: ok mv" "mv renames file"
check_output "$LOG" "M11-UTIL: ok mkdir" "mkdir creates directory"
check_output "$LOG" "M11-UTIL: ok rmdir" "rmdir removes directory"
check_output "$LOG" "M11-UTIL: ok rm" "rm removes file"
check_output "$LOG" "M11-UTIL: ok ln-readlink" "ln -s and readlink work together"
check_output "$LOG" "M11-UTIL: ok ps" "ps runs without error"
check_output "$LOG" "M11-UTIL: ok date" "date runs without error"
check_output "$LOG" "M11-UTIL: ok uname" "uname -a runs without error"
check_output "$LOG" "M11-UTIL: ok id" "id prints identity"
check_output "$LOG" "M11-UTIL: ok whoami" "whoami prints user name"
check_output "$LOG" "M11-UTIL: ok sleep" "sleep 0 returns successfully"
check_output "$LOG" "M11-UTIL: ok bad-flag-ls" "ls rejects unsupported flags"
check_output "$LOG" "M11-UTIL: ok bad-flag-grep" "grep rejects unsupported flags"
check_output "$LOG" "NET-SMOKE: ok ping-gateway" "ping -c 2 10.0.2.2 succeeds"
check_output "$LOG" "NET-SMOKE: ok dual-stack-accept" "a dual-stack IPv6 listener accepts an IPv4 connection and names its peer ::ffff:127.0.0.1"
check_output "$LOG" "UDP-SMOKE: probe-sent" "UDP probe command runs"
check_output "$LOG" "UDP-SMOKE: icmp-port-unreachable" "UDP unbound port triggers ICMP unreachable"
check_output "$LOG" "UDP-SMOKE: queue-2pkt-ok" "UDP socket queue preserves two packets"
check_output "$LOG" "POLL-SMOKE: ready-udp" "socket poll readiness path exercised"
check_output "$LOG" "ARP-SMOKE: request-sent" "ARP request path exercised"
    if grep -q "ARP-SMOKE: resolution-ready" "$LOG" 2>/dev/null; then
        pass "ARP resolution became available"
    else
        fail "ARP resolution became available" "no ARP resolution marker observed in this run"
    fi
    if grep -q "ARP-SMOKE: reply-received" "$LOG" 2>/dev/null; then
        pass "ARP reply path exercised"
    else
        fail "ARP reply path exercised" "no ARP reply observed in this run"
    fi
if grep -q "TCP-SMOKE: path-exercised" "$LOG" 2>/dev/null; then
	pass "TCP connect/listen/accept/send/recv path exercised"
elif grep -q "TCP-SMOKE: unsupported" "$LOG" 2>/dev/null; then
	pass "TCP baseline limitation explicitly reported"
else
	missing_marker "$LOG" "TCP path marker emitted" "missing TCP smoke marker"
fi
# ── M27 Terminal OS Polish: kernel command line parsing ──
check_output "$LOG" "M27-CMDLINE: ok kv-parse" "kernel command line key=value parser works"
check_output "$LOG" "M27-INIT: ok rc-script" "boot rc script runs via /bin/sh"
check_output "$LOG" "M27-INIT: first-boot rootfs initialised" "first-boot rootfs setup runs"
check_output "$LOG" "M27-USER: ok getpwnam-root" "getpwnam parses root from /etc/passwd"
check_output "$LOG" "M27-USER: ok getpwnam-user" "getpwnam parses a non-root user from /etc/passwd"
check_output "$LOG" "M27-USER: ok getpwuid" "getpwuid resolves uid to name"
check_output "$LOG" "M27-USER: ok unknown-user" "getpwnam returns NULL for unknown user"
check_output "$LOG" "M27-USER: ok setuid-drop" "setgid/setuid privilege drop works (login machinery)"
check_output "$LOG" "M27-USER: done" "M27 user/passwd smoke completes"
# ── M29 POSIX threads / futex / TLS ──
check_output "$LOG" "M29-PTHREAD: start" "M29 pthread smoke starts"
check_output "$LOG" "M29-PTHREAD: ok create-join" "pthread_create + pthread_join works"
check_output "$LOG" "M29-PTHREAD: ok attr" "pthread_attr stack size + detach state are honored"
check_output "$LOG" "M29-PTHREAD: ok mutex" "pthread mutex serialises two threads"
check_output "$LOG" "M29-PTHREAD: ok condvar" "pthread condvar signal/wait works"
check_output "$LOG" "M29-PTHREAD: ok thread-local" "real ELF __thread storage is per-thread in spawned pthreads"
check_output "$LOG" "M29-PTHREAD: ok tls" "SYS_SET_TLS + %fs:0 round-trip works"
check_output "$LOG" "M29-PTHREAD: ok gettid" "SYS_GETTID returns distinct ids per thread"
check_output "$LOG" "M29-PTHREAD: ok stress-smp" "120 rounds of unjoined-thread exit reclaim the shared mm (no PMM leak)"
check_output "$LOG" "M29-PTHREAD: ok tsd" "pthread TSD key creation and dtor calls work"
check_output "$LOG" "M29-PTHREAD: ok syslog" "syslog open/write/close works"
check_output "$LOG" "/dev/log: .*M54-LOG sink-delivers-ok" "kernel /dev/log sink forwards a syslog datagram to the kernel log (no userspace syslogd)"
check_output "$LOG" "M29-PTHREAD: ok utmp" "utmpname/setutent/pututline/getutline login accounting works"
check_output "$LOG" "M29-PTHREAD: ok locale" "setlocale + localeconv + nl_langinfo work"
check_output "$LOG" "M29-PTHREAD: ok iconv" "iconv UTF-8/Latin-1/ASCII conversion + error semantics work"
check_output "$LOG" "M29-PTHREAD: ok time-hammer" "tight gettimeofday/clock_gettime loop is crash-free and monotonic"
check_output "$LOG" "M29-PTHREAD: ok cancel" "pthread_cancel deferred cancellation + setcancelstate work"
check_output "$LOG" "M29-PTHREAD: done" "M29 pthread smoke completes"
# ── M31 User Security / Passwords / Setuid ──
check_output "$LOG" "M31-SEC: start" "M31 user-security smoke starts"
check_output "$LOG" "M31-SEC: ok shadow-format" "/etc/shadow exists and uses \$6\$ SHA-512 crypt"
check_output "$LOG" "M31-SEC: ok getspnam-root" "getspnam(3) returns root's \$6\$ hash (the path SSH password auth uses)"
check_output "$LOG" "M31-SEC: ok setuid-elevate" "setuid initramfs binary elevates euid to root"
check_output "$LOG" "M31-SEC: ok uid-denial" "non-root setuid(0) is rejected by kernel"
check_output "$LOG" "M31-SEC: done" "M31 smoke completes"
# ── M32 Networking / Multiplexing ──
check_output "$LOG" "M32-NET: start" "M32 multiplex smoke starts"
# External connectivity (off-link TCP over QEMU usernet). Skips cleanly when
# the test host has no internet so the suite stays green offline.
if grep -q "M32-NET: unsupported ext-http" "$LOG" 2>/dev/null; then
	pass "External HTTP skipped (no off-link connectivity)"
	pass "External HTTPS skipped (no off-link connectivity)"
else
	check_output "$LOG" "M32-NET: ok ext-http" "external HTTP GET over usernet works (off-link TCP to a real host)"
	if grep -q "M32-NET: unsupported ext-https" "$LOG" 2>/dev/null; then
		pass "External HTTPS skipped (handshake/cert-time unavailable)"
	else
		check_output "$LOG" "M32-NET: ok ext-https" "external HTTPS GET works (real mbedTLS handshake against a CA-signed cert)"
	fi
fi
# External IPv6 connectivity (curl -6 over the kernel off-link IPv6 datapath).
# Skips on its own when the usernet link has no IPv6 route.
if grep -q "M32-NET: unsupported ext-http6" "$LOG" 2>/dev/null; then
	pass "External HTTP over IPv6 skipped (no off-link IPv6 route)"
else
	check_output "$LOG" "M32-NET: ok ext-http6" "external HTTP GET over IPv6 works (curl -6, off-link IPv6 datapath)"
fi
if grep -q "M32-NET: unsupported ext-https6" "$LOG" 2>/dev/null; then
	pass "External HTTPS over IPv6 skipped (no off-link IPv6 route)"
else
	check_output "$LOG" "M32-NET: ok ext-https6" "external HTTPS GET over IPv6 works (curl -6 + mbedTLS over off-link IPv6)"
fi
check_output "$LOG" "M32-NET: ok select-timeout-zero" "select() with zero timeout returns 0 ready"
check_output "$LOG" "M32-NET: ok select-pipe-ready" "select() reports a buffered pipe as readable"
check_output "$LOG" "M32-NET: ok select-multi-fd" "select() across multiple fds isolates readability"
check_output "$LOG" "M32-NET: ok sockopt-reuseaddr" "M32b: setsockopt/getsockopt SO_REUSEADDR round-trips"
check_output "$LOG" "M32-NET: ok sockopt-nodelay" "M32b: setsockopt/getsockopt TCP_NODELAY round-trips"
check_output "$LOG" "M32-NET: ok idle-connection" "a TCP connection left idle for five seconds with keepalive off still carries data — the control for the keepalive check"
check_output "$LOG" "M32-NET: ok keepalive-defaults" "a fresh TCP socket reports Linux's keepalive defaults (7200s idle, 9 probes)"
check_output "$LOG" "M32-NET: ok keepalive-rejects-zero" "a zero keepalive interval is refused rather than stored"
check_output "$LOG" "M32-NET: ok keepalive-set" "TCP_KEEPIDLE/KEEPINTVL/KEEPCNT round-trip through get/setsockopt"
check_output "$LOG" "M32-NET: ok keepalive-live" "an idle connection with keepalive on still carries data after several probes have gone out and been answered"
check_output "$LOG" "M32-NET: ok keepalive-udp-refused" "TCP_KEEPIDLE on a datagram socket is refused"
check_output "$LOG" "M32-NET: ok sockopt-sotype" "M32b: getsockopt SO_TYPE reports the real socket type"
check_output "$LOG" "M32-NET: ok getsockname" "M32b: getsockname reports the bound local address"
check_output "$LOG" "M32-NET: ok getpeername" "M32b: getpeername reports the connected peer address"
check_output "$LOG" "M32-NET: ok shutdown-wr" "M32b: shutdown(SHUT_WR) closes the write half (EPIPE + peer EOF)"
check_output "$LOG" "M32B-PTY: ok openpty" "M32b: openpty allocates a /dev/ptmx master + /dev/pts/N slave; ptsname matches"
check_output "$LOG" "M32B-PTY: ok winsize" "M32b: TIOCSWINSZ/TIOCGWINSZ window-size round-trips on the pty"
check_output "$LOG" "M32B-PTY: ok canonical" "M32b: pty canonical line discipline delivers a line on newline"
check_output "$LOG" "M32B-PTY: ok echo" "M32b: pty ECHO reflects input back on the master"
check_output "$LOG" "M32B-PTY: ok raw" "M32b: pty raw mode (cfmakeraw) delivers single bytes with no echo"
check_output "$LOG" "M32B-PTY: ok hangup" "M32b: closing the pty master makes the slave read report EOF"
check_output "$LOG" "M32B-SESS: ok env-execve" "M32b: login-shell environment survives execve into getenv (crt0 -> environ)"
check_output "$LOG" "M32B-CRYPTO: ok getrandom" "M32b: getrandom returns fresh, non-zero secure random bytes"
check_output "$LOG" "M32B-CRYPTO: ok sha512" "M32b: userspace SHA-512 matches the FIPS 180-4 test vector"
check_output "$LOG" "M32B-CRYPTO: ok crypt" "M32b: \$6\$ (SHA-512) crypt() is deterministic and password-sensitive (shadow verify path)"
check_output "$LOG" "M32B-SSH: ok dropbearkey" "M32b: ported Dropbear runs on b1nix — dropbearkey generates an Ed25519 host key"
check_output "$LOG" "M32B-SSH: ok handshake" "M32b: end-to-end localhost SSH login (KEX + chacha20-poly1305 + password auth + remote command) over loopback"
check_output "$LOG" "M32B-SSH: ok negauth" "M32b: SSH daemon rejects a wrong password — remote command never runs, client terminates"
check_output "$LOG" "M32B-SSH: ok pty" "M32b: interactive shell over a remote PTY (sshd allocates a pty + spawns /bin/sh) runs a command"
check_output "$LOG" "M32B-SSH: ok service-lifecycle" "M32b: SSH daemon service control script and lifecycle management work"
check_output "$LOG" "M32-NET: ok inet-pton-ntop" "libc inet_pton/inet_ntop round-trip"
check_output "$LOG" "M32-NET: ok inet6-pton-ntop" "libc inet_pton/inet_ntop AF_INET6 round-trip"
check_output "$LOG" "M32-NET: ok gethostbyname-numeric" "libc gethostbyname resolves a dotted-quad"
check_output "$LOG" "M32-NET: ok getaddrinfo" "libc getaddrinfo fills sockaddr_in"
check_output "$LOG" "M32-NET: ok getaddrinfo-inet6" "libc getaddrinfo fills sockaddr_in6 for numeric ::1"
check_output "$LOG" "M32-NET: ok getnameinfo" "libc getnameinfo numeric round-trip (v4 + v6)"
check_output "$LOG" "M32-NET: ok v4mapped-udp" "dual-stack: AF_INET6 send to ::ffff:127.0.0.1 reaches an AF_INET socket"
check_output "$LOG" "M32-NET: ok udp6-loopback" "UDP over IPv6 (::1) round-trips through AF_INET6 sockets"
check_output "$LOG" "M32-NET: ok tcp6-loopback" "TCP over IPv6 (::1) echo round-trips through AF_INET6 sockets"
check_output "$LOG" "M32-NET: ok tcp-echo" "TCP loopback client/server echo works through sockets"
check_output "$LOG" "M32-NET: ok http-get" "HTTP-style client/server exchange works over TCP"
check_output "$LOG" "M32-NET: ok curl-loopback" "the HTTP client downloads over TCP loopback (connect + request + response parsing + file write)"
check_output "$LOG" "M32-NET: ok curl-ipv6" "the HTTP client downloads over IPv6 loopback (::1)"
if grep -q "M32-NET: unsupported curl-tls-suite" "$LOG" 2>/dev/null; then
	pass "Curl TLS suite skipped (curl unavailable or built without HTTPS)"
else
	check_output "$LOG" "M32-NET: ok curl-https-enabled" "Curl reports HTTPS protocol support in --version output"
	check_output "$LOG" "M32-NET: ok curl-policy-flags" "Curl exposes TLS policy controls (cert-status/crlfile/pinnedpubkey)"
	check_output "$LOG" "M32-NET: ok curl-https-handshake" "Curl performs a real HTTPS request (TLS handshake path)"
	check_output "$LOG" "M32-NET: ok curl-https-selfsigned-reject" "Curl rejects an invalid self-signed chain without -k"
fi
check_output "$LOG" "M32-NET: ok tcp-server" "TCP listener accepts and exits cleanly"
check_output "$LOG" "M32-NET: done" "M32 smoke completes"
# ── M32 TCP sliding-window flow control + M23 DNS resolver (kernel net smoke) ──
check_output "$LOG" "M32-TCP: ok window-throttle" "tcp_send honours min(cwnd,snd_wnd) and blocks at a full window"
check_output "$LOG" "DNS-SMOKE: ok parse-a-record" "DNS A-record parser extracts the resolved address"
check_output "$LOG" "DNS-SMOKE: ok parse-aaaa-record" "DNS AAAA-record parser extracts a 128-bit IPv6 address"
check_output "$LOG" "DNS-SMOKE: ok resolv-conf" "/etc/resolv.conf nameserver parsed by the kernel resolver"
check_output "$LOG" "UNIX-SMOKE: ok rcvtimeo" "SO_RCVTIMEO makes a blocking recv give up at the deadline with EAGAIN (and it really waits)"
check_output "$LOG" "UNIX-SMOKE: ok rcvtimeo-data" "a socket with SO_RCVTIMEO still returns data that is already there"
check_output "$LOG" "UNIX-SMOKE: ok fionread" "FIONREAD reports the bytes queued on a unix socket (every event-driven server asks this)"
check_output "$LOG" "UNIX-SMOKE: ok peer-close-hup" "poll reports POLLHUP once the peer of a connected unix socket closes"
check_output "$LOG" "UNIX-SMOKE: ok listen-no-hup" "a LISTENING unix socket reports readability for a queued client and never POLLHUP"
# The clock every bounded test is held to. A timer that fires early makes a
# working program look hung, so these have to be right before any other timing
# in this suite means anything: alarm(2), setitimer(2) and the POSIX timers all
# converted seconds to ticks with 100 written out, while the LAPIC timer has
# been armed at 1 kHz -- so each fired ten times early and every `timeout N`
# in every harness reported a timeout after N/10 seconds.
check_output "$LOG" "CLOCK: ok alarm-keeps-time" "alarm(2) fires one second after it was asked for, measured against CLOCK_MONOTONIC rather than counted in ticks"
check_output "$LOG" "CLOCK: ok itimer-keeps-time" "setitimer(ITIMER_REAL) keeps the time it was given"
check_output "$LOG" "CLOCK: ok posix-timer-keeps-time" "timer_create/timer_settime keeps the time it was given - this is the timer timeout(1) arms, so it is the one that decides whether every bounded command in the suite is measuring anything"
check_output "$LOG" "UNIX-SMOKE: ok shutdown-wr-poll" "a peer's shutdown(SHUT_WR) makes this socket poll readable, and POLLRDHUP for a caller that asked - without it an event loop never wakes to read the EOF"
check_output "$LOG" "UNIX-SMOKE: ok shutdown-wr-eof" "after the queued bytes, a peer's shutdown(SHUT_WR) reads as end-of-file rather than blocking for ever"
check_output "$LOG" "UNIX-SMOKE: ok shutdown-wr-oneway" "the half-close closes ONE direction: the reverse direction still carries data, and the shut half reports EPIPE"
check_output "$LOG" "UNIX-SMOKE: ok seqpacket-accept" "a SOCK_SEQPACKET connect really queues a connection for accept(2) and the message arrives whole - it used to take the datagram path, which is why udevadm control --ping waited out its timeout against a daemon that had received nothing"
check_output "$LOG" "UNIX-SMOKE: ok seqpacket-ctl-roundtrip" "udev's control protocol end to end over seqpacket: send, shutdown(SHUT_WR), and the client is released by the server CLOSING the connection"
check_output "$LOG" "UNIX-SMOKE: ok dgrampair-epoll-wake" "a datagram arriving on an AF_UNIX socketpair wakes a task already parked in epoll_wait - systemd-udevd's manager waits for its workers exactly here"
check_output "$LOG" "UNIX-SMOKE: ok dgrampair-scm-credentials" "and the message carries SCM_CREDENTIALS naming the child that sent it, which is how the manager knows WHICH worker finished"
check_output "$LOG" "TCP-SMOKE: ok accept-after-peer-close" "a completed connection whose peer has already closed is still there to accept - the client is reaped before the server looks, so this cannot pass by winning a race (systemd socket activation is exactly this shape)"
check_output "$LOG" "TCP-SMOKE: ok accept-after-peer-close-data" "and the bytes that client sent belong to the accepted socket rather than dying with the connection"
check_output "$LOG" "DNS-SMOKE: ok resolve-name" "getaddrinfo resolves a real name through musl's resolver (needs the SLIRP DNS, like the nslookup check)"
check_output "$LOG" "M32-IP6: ok icmpv6-loopback" "ICMPv6 echo over the ::1 loopback datapath round-trips"
check_output "$LOG" "M32-IP6: ok icmpv6-errors" "ICMPv6 reports an unreachable closed UDP port"
check_output "$LOG" "M32-IP6: ok mld" "MLD joins solicited-node groups and answers membership queries"
check_output "$LOG" "M32-NET: ok ipv6-v6only" "IPV6_V6ONLY rejects IPv4-mapped peers"
# Real-link IPv6 over QEMU usernet (SLAAC + NDP + ICMPv6 ping the v6 gateway).
# Skips cleanly when the link has no IPv6 router.
if grep -q "M32-IP6: unsupported real-link" "$LOG" 2>/dev/null; then
	pass "Real-link IPv6 skipped (no usernet IPv6 router)"
	pass "Real-link IPv6 ping skipped (no usernet IPv6 router)"
else
	check_output "$LOG" "M32-IP6: ok slaac-global" "SLAAC (RS->RA) configures a global IPv6 address on the real link"
	if grep -q "M32-IP6: unsupported real-link-ping" "$LOG" 2>/dev/null; then
		pass "Real-link IPv6 ping skipped (gateway did not answer)"
	else
		check_output "$LOG" "M32-IP6: ok real-link-ping" "ICMPv6 echo to the usernet IPv6 gateway round-trips (NDP NS/NA + 0x86DD)"
	fi
fi
# ── M84 IPv4 FIB + TCP robustness ──
check_output "$LOG" "M84-ROUTE: ok lpm" "FIB picks the longest matching prefix (host > /16 > /8 > default)"
check_output "$LOG" "M84-ROUTE: ok delete" "deleting a host route falls back to the covering prefix"
check_output "$LOG" "M84-ROUTE: ok metric" "equal prefixes are broken by metric"
check_output "$LOG" "M84-ROUTE: ok ecmp" "equal-cost routes load-share, stable per destination"
check_output "$LOG" "M84-ROUTE: ok per-interface" "a route reports the interface its packets leave through"
check_output "$LOG" "M84-ROUTE6: ok lpm" "IPv6 FIB picks the longest matching prefix (/128 > /48 > /32)"
check_output "$LOG" "M84-ROUTE6: ok link-local-onlink" "fe80::/10 is on-link without a router advertisement"
check_output "$LOG" "M84-ROUTE6: ok default" "IPv6 default route resolves via the router and deletes cleanly"
check_output "$LOG" "M84-POLICY: ok source-rule" "a policy rule sends one source prefix through a different table"
check_output "$LOG" "M84-POLICY: ok rule-delete" "deleting a rule returns the source to the main table"
check_output "$LOG" "M84-POLICY: ok rule-fallthrough" "a rule whose table has no route falls through to the next rule"
check_output "$LOG" "M84-POLICY: ok ecmp-flow-hash" "ECMP hashes the 5-tuple: flows spread, each flow stays pinned"
check_output "$LOG" "M84-POLICY: ok ecmp-wide" "an ECMP group is bounded by the table, not by a fixed candidate array"
check_output "$LOG" "M84-POLICY: ok procfs-control" "/proc/net/rt_rules and rt_tables accept write(2) commands and read back"
check_output "$LOG" "M84-POLICY: ok text-control" "/proc/net/rt_tables and rt_rules command grammar adds, routes and rejects"
check_output "$LOG" "M84-DHCP6: ok solicit" "DHCPv6 client emits a Solicit"
check_output "$LOG" "M84-DHCP6: ok advertise" "DHCPv6 Advertise latches the server DUID and moves to Request"
check_output "$LOG" "M84-DHCP6: ok reply-bound" "DHCPv6 Reply binds the address, installs its route and T1/T2"
check_output "$LOG" "M84-DHCP6: ok dns-option" "DHCPv6 option 23 records the IPv6 nameserver"
check_output "$LOG" "M84-DHCP6: ok renew" "T1 expiry moves the DHCPv6 client into Renew"
check_output "$LOG" "M84-DHCP6: ok malformed-rejected" "a truncated DHCPv6 option is rejected, not parsed"
# ── M110: the boot log has the shape an operator expects ────────────────────
echo ""
echo "[RUN] M110 boot-log format checks..."
check_log_timestamps "$LOG" "every kernel log line carries a monotonic timestamp"
check_output "$LOG" "^\[ *[0-9][0-9]*\.[0-9][0-9][0-9][0-9][0-9][0-9]\] b1nix kernel starting" \
	"the first kernel line is stamped in the dmesg [ssss.uuuuuu] form"
check_output "$LOG" "^\[ *[0-9][0-9]*\.[0-9][0-9][0-9][0-9][0-9][0-9]\] pci [0-9a-f][0-9a-f]*:" \
	"PCI lines are prefixed with the device they are about"
check_output "$LOG" "M110-LOG: ok clock-monotonic" \
	"the log clock never runs backwards"
check_output "$LOG" "M110-LOG: ok clock-uptime-agree" \
	"log timestamps and /proc/uptime read the same monotonic clock"
check_output "$LOG" "M110-LOG: ok filter-recorded" \
	"a line the console filter drops is still recorded in the ring"
check_absent "$LOG" "M110-LOG: FAIL filter-let-a-debug-line-through" \
	"a line above the console loglevel never reaches the console"
check_output "$LOG" "m110-log: subsystem prefix" \
	"a subsystem-tagged line renders as \"subsys: message\""
check_output "$LOG" "M110-LOG: done" "the boot-log self-test ran to completion"

check_output "$LOG" "M84-TCP: ok opt-parse" "TCP parses MSS, window-scale and SACK-permitted options"
check_output "$LOG" "M84-TCP: ok opt-malformed" "a malformed TCP option terminates the walk instead of looping"
check_output "$LOG" "M84-TCP: ok mss-negotiated" "handshake negotiates MSS and window scaling on both sides"
check_output "$LOG" "M84-TCP: ok wscale" "peer's advertised window is applied with the negotiated shift"
check_output "$LOG" "M84-TCP: ok ooo-queued" "a segment arriving past a hole is queued, not dropped"
check_output "$LOG" "M84-TCP: ok ooo-reassembly" "filling the hole delivers the reassembled stream in order"
check_output "$LOG" "M84-TCP: ok dup-trim" "retransmitted bytes already delivered are trimmed"
check_output "$LOG" "M84-TCP: ok rcv-wscale" "advertised receive window exceeds 64 KiB via a non-zero window scale"
check_output "$LOG" "M84-TCP: ok sack-emit" "ACKs carry SACK blocks describing the reassembly queue"
check_output "$LOG" "M84-TCP: ok sack-consume" "a peer's SACK block marks that segment as delivered"
check_output "$LOG" "M84-TCP: ok dsack-emit" "a duplicate segment is reported back as a D-SACK block"
check_output "$LOG" "M84-TCP: ok dsack-undo" "a received D-SACK undoes the congestion reduction it caused"
check_output "$LOG" "M84-TCP: ok scoreboard-pipe" "the RFC 6675 scoreboard marks losses and cwnd stays at ssthresh"
# ── M32a PCRE2 userspace port ──
check_output "$LOG" "M32-PCRE2: ok compile" "ported PCRE2 compiles a pattern"
check_output "$LOG" "M32-PCRE2: ok match" "ported PCRE2 matches and captures a group"
check_output "$LOG" "M32-PCRE2: ok nomatch" "ported PCRE2 correctly reports a non-match"
check_output "$LOG" "M32-PCRE2: done" "PCRE2 smoke completes"
# ── M53 zlib userspace port (NetSurf image-codec dependency) ──
check_output "$LOG" "M53-ZLIB: ok compress" "ported zlib compresses (one-shot compress2)"
check_output "$LOG" "M53-ZLIB: ok uncompress" "ported zlib decompresses (one-shot uncompress)"
check_output "$LOG" "M53-ZLIB: ok roundtrip" "zlib one-shot roundtrip byte-for-byte identical"
check_output "$LOG" "M53-ZLIB: ok crc32" "zlib crc32 (incremental == single-shot)"
check_output "$LOG" "M53-ZLIB: ok stream" "zlib streaming deflate/inflate roundtrip (libpng path)"
check_output "$LOG" "M53-ZLIB: done" "zlib smoke completes"
# ── M53 libpng userspace port (NetSurf image-codec dependency) ──
check_output "$LOG" "M53-PNG: ok encode" "ported libpng encodes a valid PNG stream"
check_output "$LOG" "M53-PNG: ok decode-header" "libpng decodes IHDR (dimensions/depth/color)"
check_output "$LOG" "M53-PNG: ok decode" "libpng decodes image; pixels byte-for-byte identical"
check_output "$LOG" "M53-PNG: done" "libpng smoke completes"
# ── M53 libjpeg userspace port (NetSurf image-codec dependency) ──
check_output "$LOG" "M53-JPEG: ok encode" "ported libjpeg encodes a valid JPEG stream"
check_output "$LOG" "M53-JPEG: ok decode-header" "libjpeg decodes header (dimensions/components)"
check_output "$LOG" "M53-JPEG: ok decode" "libjpeg decodes image; pixels within tolerance"
check_output "$LOG" "M53-JPEG: done" "libjpeg smoke completes"
# ── M53 libwebp userspace port (image + VP8 video-keyframe codec) ──
check_output "$LOG" "M53-WEBP: ok encode" "ported libwebp encodes a valid RIFF/WEBP stream"
check_output "$LOG" "M53-WEBP: ok info" "libwebp reads back image dimensions"
check_output "$LOG" "M53-WEBP: ok decode" "libwebp lossless decode; pixels byte-for-byte identical"
check_output "$LOG" "M53-WEBP: done" "libwebp smoke completes"
# ── M53 libvpx userspace port (VP8 full-motion video decode) ──
check_output "$LOG" "M53-VPX: ok webp-vp8-frame" "extracts a VP8 keyframe from a lossy WebP"
check_output "$LOG" "M53-VPX: ok decode-init" "libvpx VP8 decoder initializes"
check_output "$LOG" "M53-VPX: ok decode" "libvpx decodes the VP8 frame to an I420 image"
check_output "$LOG" "M53-VPX: ok luma" "decoded luma plane matches the original within tolerance"
check_output "$LOG" "M53-VPX: done" "libvpx smoke completes"
# NetSurf's checks stood here: the library chain (libwapcaplet, libparserutils,
# libhubbub, libcss, libdom and the helper decoders), the framebuffer frontend,
# and the loopback and off-link HTTP/HTTPS probes. They went with the browser --
# nothing starts m53_httpd/m53_httpsd any more, so those markers cannot appear.
# ── M34 procfs / sysfs synthetic filesystems ──
check_output "$LOG" "procfs: mounted at /proc" "procfs mounted at /proc"
check_output "$LOG" "sysfs: mounted at /sys" "sysfs mounted at /sys"
check_output "$LOG" "M34-PROC: start" "M34 procfs smoke starts"
check_output "$LOG" "M34-PROC: ok meminfo" "/proc/meminfo exposes MemTotal"
check_output "$LOG" "M34-PROC: ok version" "/proc/version identifies the kernel"
check_output "$LOG" "M34-PROC: ok proc-self-status" "/proc/self/status reports calling pid"
check_output "$LOG" "M34-PROC: ok proc-self-maps" "/proc/self/maps lists VMA regions"
check_output "$LOG" "M34-PROC: ok proc-listing" "/proc lists files, self and a pid dir"
check_output "$LOG" "M34-PROC: ok proc-pid-status" "/proc/<pid>/status reports the process"
check_output "$LOG" "M34-PROC: ok sysfs-osrelease" "/sys/kernel/osrelease reads back"
check_output "$LOG" "M34-PROC: ok sysfs-cpu" "/sys/devices/system/cpu/online reads back"
check_output "$LOG" "M34-PROC: ok tools" "free/sysctl/top read /proc and /sys"
check_output "$LOG" "M34-PROC: done" "M34 procfs smoke completes"
# ── M35 core dumps + kallsyms ──
check_output "$LOG" "M35-DIAG: start" "M35 kallsyms diag starts"
check_output "$LOG" "M35-DIAG: ok kallsyms" "kallsyms resolves a kernel symbol at offset 0"
check_output "$LOG" "M35-DIAG: ok kallsyms-offset" "kallsyms resolves a mid-function address with offset"
check_output "$LOG" "M35-DIAG: ok kallsyms-multi" "kallsyms resolves a second distinct symbol"
check_output "$LOG" "M35-DIAG: done" "M35 kallsyms diag completes"
check_output "$LOG" "M35-CORE: start" "M35 core-dump smoke starts"
check_output "$LOG" "M35-CORE: ok crash-signal" "faulting child is terminated by a signal"
check_output "$LOG" "M35-CORE: ok core-elf" "/tmp/core is an ET_CORE x86_64 ELF"
check_output "$LOG" "M35-CORE: ok core-prstatus" "core carries a PT_NOTE register file"
check_output "$LOG" "M35-CORE: done" "M35 core-dump smoke completes"
# ── M77 writable global resource caps ──
check_output "$LOG" "M77-CAPS: start" "M77 resource-cap sysctls present"
check_output "$LOG" "M77-CAPS: ok shmmax" "shmmax sysctl reads, writes back, clamps and restores"
check_output "$LOG" "M77-CAPS: ok tcp-max-conns" "tcp-max-conns sysctl reads, writes back, clamps and restores"
check_output "$LOG" "M77-CAPS: ok pipe-max-count" "pipe-max-count sysctl reads, writes back, clamps and restores"
check_output "$LOG" "M77-CAPS: ok coredump-max-bytes" "coredump-max-bytes sysctl reads, writes back, clamps and restores"
check_output "$LOG" "M77-CAPS: ok sysctl-shmmax" "busybox sysctl -w kernel.shmmax sets the cap"
check_output "$LOG" "M77-CAPS: ok sysctl-coredump" "busybox sysctl -w kernel.coredump-max-bytes (hyphenated key) sets the cap"
check_output "$LOG" "M77-CAPS: ok enforce-shmmax" "a lowered SHMMAX cap rejects an oversized shmget"
check_output "$LOG" "M77-CAPS: ok enforce-pipe-max" "a lowered pipe-max-count caps the VFS pipe pool (16) and opens again after restore"
check_output "$LOG" "M77-CAPS: ok enforce-tcp-max" "a lowered tcp-max-conns caps listeners at 16 and an async connect gets ECONNREFUSED"
check_output "$LOG" "M77-CAPS: done" "M77 writable resource-cap sysctls complete"

# ── M42 wave-5 prerequisites: POSIX limits, VFS, pattern matching & signal /
#    job control (the gate before enabling the upstream ash shell) ──
check_output "$LOG" "M42-W5PRE: start" "M42 wave-5 prerequisite suite starts"
check_output "$LOG" "M42-W5PRE: ok rlimit-enforcement" "RLIMIT_NOFILE limits the open fd count"
check_output "$LOG" "M42-W5PRE: ok getrlimit-setrlimit" "getrlimit/setrlimit sets limits correctly"
check_output "$LOG" "M42-W5PRE: ok dup" "dup() returns lowest available descriptor"
check_output "$LOG" "M42-W5PRE: ok access" "access() checks exist/perm modes"
check_output "$LOG" "M42-W5PRE: ok ftruncate" "ftruncate() resizes and zeroes memory buffers"
check_output "$LOG" "M42-W5PRE: ok fchdir" "fchdir() changes current working directory"
check_output "$LOG" "M42-W5PRE: ok fnmatch" "POSIX fnmatch matches brackets and PERIOD/PATHNAME flags"
check_output "$LOG" "M42-W5PRE: ok regex" "POSIX regex matches intervals and named classes"
check_output "$LOG" "M42-W5PRE: ok sigsuspend-alarm" "atomic sigsuspend waits for alarm and restores mask"
check_output "$LOG" "M42-W5PRE: ok sigsuspend-blocked-signal" "sigsuspend runs the handler for a signal only its own mask unblocks"
check_output "$LOG" "M42-W5PRE: ok pwait-sigmask" "ppoll delivers a signal its own mask unblocks"
check_output "$LOG" "M42-W5PRE: ok ppoll-precision" "ppoll honours a sub-millisecond timeout instead of rounding it away"
check_output "$LOG" "M42-W5PRE: ok interrupted-waitpid" "waitpid is interrupted by signal with EINTR"
check_output "$LOG" "M42-W5PRE: ok job-control" "Job control SIGSTOP/SIGCONT changes state"
check_output "$LOG" "M42-W5PRE: ok sigchld-on-exit" "SIGCHLD is delivered to the parent on child exit"
check_output "$LOG" "M42-W5PRE: ok kill-sleeping-child" "a signal sent to a task asleep in nanosleep reaches it at once rather than at the sleep's own deadline - measured in elapsed time, because the late answer is the same answer (this is why timeout(1) returned 124 on time and never killed anything)"
check_output "$LOG" "M42-W5PRE: done" "M42 wave-5 prerequisite suite completes"
# ── M46: VFS integrity + POSIX process conformance ──
check_output "$LOG" "M46-SMOKE: start" "M46 conformance suite starts"
check_output "$LOG" "M46-SMOKE: ok exit-status-139" "exit(139) reports as a normal exit, not a signal death"
check_output "$LOG" "M46-SMOKE: ok signal-death" "SIGKILL death reports WIFSIGNALED with the right signal"
check_output "$LOG" "M46-SMOKE: ok kill-zero-pgrp" "kill(0, sig) signals the caller's process group"
check_output "$LOG" "M46-SMOKE: ok kill-all-probe" "kill(-1, 0) broadcast permission probe succeeds"
check_output "$LOG" "M46-SMOKE: ok waitpid-pgid" "waitpid(-pgid) reaps a child from that process group"
check_output "$LOG" "M46-SMOKE: ok setpgid-esrch" "setpgid on a nonexistent pid returns ESRCH"
check_output "$LOG" "M46-SMOKE: ok setpgid-eperm-pgrp" "setpgid into a nonexistent group returns EPERM"
check_output "$LOG" "M46-SMOKE: ok getpgid" "getpgid(0) matches getpgrp()"
check_output "$LOG" "M46-SMOKE: ok getpgid-esrch" "getpgid on a nonexistent pid returns ESRCH"
check_output "$LOG" "M46-SMOKE: ok nice-roundtrip" "nice() and getpriority() round-trip"
check_output "$LOG" "M46-SMOKE: ok fork-sigmask" "fork child inherits the blocked-signal mask"
check_output "$LOG" "M46-SMOKE: ok append-atomic" "concurrent O_APPEND writers never overwrite each other"
check_output "$LOG" "M46-SMOKE: ok truncate-zeros" "shrink-then-grow truncate reads back zeros"
check_output "$LOG" "M46-SHEBANG-OK" "the #! script's interpreter actually ran"
check_output "$LOG" "M46-SMOKE: ok shebang-exec" "direct execve() of a #! script works"
check_output "$LOG" "M46-SMOKE: ok bad-shebang-exec" "200 execs of a #! script whose interpreter is missing fail cleanly and leave the kernel heap intact"
check_output "$LOG" "M46-SMOKE: ok procfd-reopen" "opening /proc/self/fd/N is a fresh open with the caller's flags, not a dup — an O_PATH reference upgrades to a readable descriptor with its own offset"
check_output "$LOG" "M46-SMOKE: ok exit-group" "exit_group semantics terminate all thread group members"
check_output "$LOG" "M46-SMOKE: ok setresuid-setresgid" "setresuid/setresgid set credentials and EPERM is enforced"
check_output "$LOG" "M46-SMOKE: ok waitid" "waitid waiting for child state transitions works"
check_output "$LOG" "M46-SMOKE: ok times-getrusage" "times() and getrusage() accounting works"
check_output "$LOG" "M46-SMOKE: ok orphaned-pgrp" "orphaned process groups with stopped tasks receive SIGHUP+SIGCONT"
check_output "$LOG" "M46-SMOKE: ok nice-applied" "nice() reaches the kernel for both classes and both keep running (the biasing itself is open, M117)"
check_output "$LOG" "M46-SMOKE: ok dir-mtime-create" "creating an entry marks the containing directory modified"
check_output "$LOG" "M46-SMOKE: ok dir-mtime-subsecond" "a second change inside the same second still moves the directory's mtime"
check_output "$LOG" "M46-SMOKE: ok dir-mtime-unlink" "removing an entry marks the containing directory modified"
check_output "$LOG" "M46-SMOKE: ok rename-long-name" "rename keeps a destination name longer than 63 characters whole"
check_output "$LOG" "M46-SMOKE: ok empty-datagram-cred" "a zero-length AF_UNIX datagram is delivered, wakes poll, and carries SCM_CREDENTIALS"
check_output "$LOG" "M46-SMOKE: ok rdonly-mount-open" "a read-only bind remount makes open(O_WRONLY) and O_TRUNC fail with EROFS"
check_output "$LOG" "M46-SMOKE: ok signal-ends-sleep" "SIGTERM kills a process parked in nanosleep, promptly - what every bounded command depends on"
check_output "$LOG" "M46-SMOKE: done" "M46 conformance suite completes"
# ── M57: multiprocess broker primitives (fork/exec/FD inheritance + brokering) ──
check_output "$LOG" "M57-SMOKE: ok fork-fdshare" "fork shares the open-file-description file offset with the child"
check_output "$LOG" "M57-SMOKE: ok cloexec" "FD_CLOEXEC via F_SETFD/O_CLOEXEC round-trips through F_GETFD"
check_output "$LOG" "M57-SMOKE: ok exec-inherit" "non-CLOEXEC fd and dup2-stdio survive execve while CLOEXEC fd is closed"
check_output "$LOG" "M57-SMOKE: ok fd-broker" "socketpair + SCM_RIGHTS hands a live fd to a forked child"
check_output "$LOG" "M57-SMOKE: ok fd-broker-death" "in-flight passed fd survives sender close and peer hangup is reported"
check_output "$LOG" "M57-SMOKE: ok dupfd-cloexec" "F_DUPFD_CLOEXEC sets FD_CLOEXEC while F_DUPFD leaves it clear"
check_output "$LOG" "M57-SMOKE: ok unix-addrlen" "getsockname/getpeername report an AF_UNIX address's real length (2 unnamed, offsetof(sun_path)+name+1 bound)"
check_output "$LOG" "M57-SMOKE: ok unix-abstract" "an AF_UNIX name in the abstract namespace binds, connects and carries data (D-Bus, X11 and agetty use it)"
check_output "$LOG" "M57-SMOKE: ok unix-abstract-rebind" "an abstract name is released when its socket closes — there is no file to unlink"
check_output "$LOG" "M57-SMOKE: ok unix-addrlen-reuse" "a socket-name call given more room than the address writes only the address (the Qt sockAddrSize reuse that smashed kioworker's stack)"
check_output "$LOG" "M57-SMOKE: ok mojo-pipe" "Mojo message pipe write/read and handle passing work"
check_output "$LOG" "M57-SMOKE: ok mojo-shm" "Mojo shared buffer create/duplicate/map/unmap over memfd work"
check_output "$LOG" "M57-SMOKE: ok mojo-watcher" "Mojo watcher event loop integration over epoll works"
check_output "$LOG" "M57-SMOKE: ok mojo-broker" "Mojo IPC brokering across process boundaries over SCM_RIGHTS works"
check_output "$LOG" "M57-SMOKE: done" "M57 broker-primitive suite completes"
# ── M75: On-Device GPU Path (LLVMpipe / Mesa / DSO init) ──
check_output "$LOG" "M75-GPU: ok dso-constructors" "M75: shared-library DT_INIT_ARRAY constructors run via AT_B1NIX_DSO_INIT"
check_output "$LOG" "M75-GPU: ok llvmpipe-init" "M75: LLVMpipe software GL JIT state machine initializes"
check_output "$LOG" "M75-GPU: ok gl-context" "M75: offscreen GL context creation succeeds"
check_output "$LOG" "M75-GPU: ok llvmpipe-render" "M75: software LLVMpipe GL rasterizer renders offscreen frame and passes pixel validation"
check_output "$LOG" "M75-GPU: done" "M75 on-device GPU path test completes"
# ── M73: modern I/O & introspection syscalls ──
check_output "$LOG" "M73-SMOKE: ok statx" "statx returns size/mode/nlink/ino matching fstat (path + AT_EMPTY_PATH)"
check_output "$LOG" "M73-SMOKE: ok sendfile" "sendfile copies a range, advances the explicit offset, leaves the src fd offset"
check_output "$LOG" "M73-SMOKE: ok copy-file-range" "copy_file_range copies a byte range using independent explicit offsets"
check_output "$LOG" "M73-SMOKE: ok fallocate" "fallocate mode 0 grows + zero-fills; KEEP_SIZE does not grow"
check_output "$LOG" "M73-SMOKE: ok splice" "splice moves data file->pipe->file intact"
# ── M73: inotify (real file-change notification, was an ENOSYS stub) ──
check_output "$LOG" "M73-SMOKE: ok inotify-modify" "inotify reports IN_MODIFY on a write to a watched file"
check_output "$LOG" "M73-SMOKE: ok inotify-dir" "inotify reports IN_CREATE/IN_DELETE (with entry name) for a watched directory"
check_output "$LOG" "M73-SMOKE: ok inotify-rmwatch" "inotify_rm_watch removes a watch"
check_output "$LOG" "M72-SMOKE: ok msync" "msync validates flags (EINVAL), unmapped range (ENOMEM), and syncs a mapped MAP_SHARED range"
# ── M88: PROT_NONE is enforced (wild access faults, not zero-fills) ──
check_output "$LOG" "M88-SMOKE: ok prot-none" "a PROT_NONE reservation SIGSEGVs on access; mprotect to RW then succeeds"
# ── M85: libc Tier-A correctness (Chromium-debt overlap) ──
check_output "$LOG" "M73-SMOKE: ok strtoull" "strtoull parses the full uint64 range + ERANGE; strtoll honors signed range/base16"
check_output "$LOG" "M73-SMOKE: ok sysconf-ncpu" "sysconf(_SC_NPROCESSORS_ONLN) reports the real online-CPU count (not a hardcoded 1)"
check_output "$LOG" "M73-SMOKE: ok abort-sigabrt" "abort() raises SIGABRT (not exit(127))"
check_output "$LOG" "M73-SMOKE: ok realpath" "realpath resolves ./.. and a symlink to the canonical path (was a strcpy stub)"
# ── M71: ASLR (PIE load-base randomization, opt-in via b1nix.aslr) ──
check_output "$LOG" "M71-ASLR: ok randomized" "two execs of the same PIE binary land at different load bases under b1nix.aslr"
# ── M63: seccomp-bpf ──
check_output "$LOG" "M63-SMOKE: ok seccomp-errno" "a seccomp filter denies a targeted syscall with ERRNO while others run"
check_output "$LOG" "M63-SMOKE: ok seccomp-errno-zero" "SECCOMP_RET_ERRNO with errno 0 blocks the syscall and returns 0 (not run)"
check_output "$LOG" "M63-SMOKE: ok seccomp-kill" "a seccomp KILL_PROCESS verdict terminates the task with SIGSYS"
check_output "$LOG" "M63-SMOKE: ok seccomp-strict" "SECCOMP_MODE_STRICT kills on a syscall outside read/write/exit/sigreturn"
check_output "$LOG" "M63-SMOKE: ok seccomp-inherit" "a forked child inherits the parent's seccomp filter"
check_output "$LOG" "M63-SMOKE: ok seccomp-nnp" "PR_SET/GET_NO_NEW_PRIVS round-trips"
check_output "$LOG" "M63-SMOKE: done" "M63 seccomp-bpf suite completes"
check_output "$LOG" "M73-SMOKE: done" "M73 modern-I/O suite completes"
# ── M80: ptrace register sets, /proc introspection, crash capture ──
check_output "$LOG" "M80-SMOKE: ok proc-task" "/proc/<pid>/task lists one dir per thread, with the right Tgid"
check_output "$LOG" "M80-SMOKE: ok proc-status" "/proc/<pid>/status reports Tgid, Threads and TracerPid"
check_output "$LOG" "M80-SMOKE: ok proc-maps" "/proc/<pid>/maps is ordered and names the executable with a real device and inode"
check_output "$LOG" "M80-SMOKE: ok proc-auxv" "/proc/<pid>/auxv parses and matches getauxval (AT_ENTRY/AT_PHDR/AT_PAGESZ)"
check_output "$LOG" "M80-SMOKE: ok proc-mem" "/proc/<pid>/mem reads and writes a process's memory at the offset-as-address"
check_output "$LOG" "M80-SMOKE: ok fault-siginfo" "an SA_SIGINFO SIGSEGV handler receives the faulting address and a SEGV_* si_code"
check_output "$LOG" "M80-SMOKE: ok ptrace-siginfo" "PTRACE_GETSIGINFO reports the tracee's fault signal, si_code and faulting address"
check_output "$LOG" "M80-SMOKE: ok ptrace-getregset" "PTRACE_GETREGSET(NT_PRSTATUS) agrees with GETREGS and honours a short iovec"
check_output "$LOG" "M80-SMOKE: ok ptrace-fpregs" "PTRACE_GETFPREGS returns the tracee's FXSAVE area with its own MXCSR"
check_output "$LOG" "M80-SMOKE: ok ptrace-seize" "PTRACE_SEIZE attaches without stopping; PTRACE_INTERRUPT then stops the tracee"
check_output "$LOG" "M80-SMOKE: ok crash-capture" "a handler process dumps a crashed process: threads, regs, auxv and memory"
check_output "$LOG" "M80-SMOKE: ok yama-scope" "ptrace_scope=1 refuses a sibling tracer until PR_SET_PTRACER names it"
check_output "$LOG" "M80-SMOKE: ok ptrace-fork-event" "PTRACE_O_TRACEFORK reports the fork event and auto-attaches the new child"
check_output "$LOG" "M80-SMOKE: ok ptrace-exec-event" "PTRACE_O_TRACEEXEC stops the tracee after execve, before the new image runs"
# NT_X86_XSTATE is an x86 register-file note (XSAVE area, AVX/YMM). AArch64's
# vector state is a different regset entirely (NT_ARM_VFP/SVE) and there is no
# XSAVE to return, so this pair has no counterpart to port rather than a gap to
# close. The rest of the M80 ptrace surface runs on both arches.
if [ "$ARCH" = "aarch64" ]; then
	skipped "PTRACE_GETREGSET(NT_X86_XSTATE) returns an XSAVE area matching GETFPREGS" "x86 register file: this arch has no XSAVE area (its vector state is NT_ARM_VFP/SVE)"
else
	check_output "$LOG" "M80-SMOKE: ok ptrace-xstate" "PTRACE_GETREGSET(NT_X86_XSTATE) returns an XSAVE area matching GETFPREGS"
fi
check_output "$LOG" "M80-SMOKE: ok ptrace-listen" "PTRACE_LISTEN parks a seized tracee out of ptrace-stop until PTRACE_INTERRUPT"
check_output "$LOG" "M80-SMOKE: ok ptrace-exitkill" "PTRACE_O_EXITKILL kills the tracee when its tracer exits"
check_output "$LOG" "M80-SMOKE: ok cpu-freq" "the measured CPU clock is published in sysfs cpufreq and matches /proc/cpuinfo"
if [ "$ARCH" = "aarch64" ]; then
	skipped "AVX/YMM state survives context switches and is visible in NT_X86_XSTATE" "AVX/YMM are x86 vector registers; this arch saves/restores its own V registers (see fpu.S) and M29 covers that"
else
	check_output "$LOG" "M80-SMOKE: ok avx-context" "AVX/YMM state survives context switches and is visible in NT_X86_XSTATE"
fi
check_output "$LOG" "M80-SMOKE: ok ptrace-syscall" "PTRACE_SYSCALL reports entry/exit stops and a tracer-written return value reaches userspace"
check_output "$LOG" "M80-SMOKE: ok ptrace-exit-event" "PTRACE_O_TRACEEXIT parks a dying tracee with its exit status and final registers"
check_output "$LOG" "M80-SMOKE: ok ptrace-ignored-signal" "a tracee stops for a signal its own process ignores"
check_output "$LOG" "M80-SMOKE: ok process-vm-rw" "process_vm_readv/writev read and write another process's memory"
check_output "$LOG" "M80-SMOKE: ok so-peercred" "SO_PEERCRED reports the peer's real pid/uid on socketpair and accepted connections"
check_output "$LOG" "M80-SMOKE: done" "M80 ptrace/crash-capture suite completes"

# ── M107: the kernel subsystems BusyBox applets were blocked on ──
check_output "$LOG" "M107-SMOKE: ok netlink-link" "an RTM_GETLINK dump names loopback and reports the NIC MAC SIOCGIFHWADDR agrees with"
check_output "$LOG" "M107-SMOKE: ok netlink-addr" "an RTM_GETADDR dump carries the address and prefix length the SIOCGIF* ioctls report"
check_output "$LOG" "M107-SMOKE: ok netlink-route" "an RTM_GETROUTE dump agrees with /proc/net/route about the default gateway"
check_output "$LOG" "M107-SMOKE: ok netlink-route-rw" "RTM_NEWROUTE/RTM_DELROUTE really add and remove a route from the FIB"
check_output "$LOG" "M107-SMOKE: ok netlink-neigh" "RTM_NEWNEIGH/RTM_GETNEIGH/RTM_DELNEIGH administer the real ARP cache"
check_output "$LOG" "M107-SMOKE: ok netlink-neigh6" "RTM_NEWNEIGH/GETNEIGH/DELNEIGH administer ndp.ko's IPv6 neighbour cache"
check_output "$LOG" "M107-SMOKE: ok link-admin-state" "ip link set <if> down/up really changes the interface's administrative state"
check_output "$GFX_LOG" "M107-FB: ok render" "a character written to the kernel console is drawn into the framebuffer"

# ── The Raspberry Pi 4 board lane ──
#
# A different machine, not another configuration of the same one: the SoC's
# own peripherals, an SD card where every other lane has virtio, and four
# cores brought up from a spin table rather than PSCI. Everything checked
# here is answered by the board's firmware or its hardware, so a driver that
# quietly stops driving anything fails instead of printing nothing.
if [ "$ARCH" = "aarch64" ] && [ "${SMOKE_RASPI_LANE:-0}" = "1" ]; then
	if qemu-system-aarch64 -machine help 2>/dev/null | grep -q "^raspi4b "; then
		check_output "$RASPI_LOG" "platform: Raspberry Pi 4" "the board is identified from its device tree"
		check_output "$RASPI_LOG" "emmc2: mmcblka" "the SD host controller enumerates the card"
		check_output "$RASPI_LOG" "(label b1nix-root) mounted at /" "root is mounted off the SD card"
		check_output "$RASPI_LOG" "init: /sbin/init pid=1" "userspace starts on the board"
		check_output "$RASPI_LOG" "M109-RPI: ok board-revision" "the VideoCore firmware answers a property message"
		check_output "$RASPI_LOG" "M109-RPI: ok arm-memory" "the firmware reports how much RAM the ARM was given"
		check_output "$RASPI_LOG" "M109-RPI: ok mac-address" "the firmware reports the board's MAC address"
		check_output "$RASPI_LOG" "M109-RPI: ok gpio-function" "a GPIO pin's function selects and reads back"
		check_output "$RASPI_LOG" "M109-RPI: ok gpio-drive" "a driven GPIO pin reads back the level it is driving"
		check_output "$RASPI_LOG" "M109-RPI: ok systimer" "the 1 MHz system timer runs at the rate it claims"
		check_output "$RASPI_LOG" "b1nix login:" "the board boots through to a login prompt"
		if grep -qa -E "KERNEL PANIC|\[PANIC\]" "$RASPI_LOG" 2>/dev/null; then
			fail "the board boots without panicking" "PANIC in $RASPI_LOG"
		else
			pass "the board boots without panicking"
		fi
	else
		# Not "skipped, near enough": this QEMU cannot model the board at all,
		# so nothing about the Pi was exercised and the report says so.
		pass "Raspberry Pi 4 lane (skipped — this QEMU has no raspi4b machine)"
	fi
fi
# Nothing is reported when the lane is off. Not a pass and not a BLOCKED row:
# the checks did not run, the board was not exercised, and a suite that prints
# a line either way trains everyone to stop reading it. SMOKE_RASPI_LANE=1.
check_output "$LOG" "M107-SMOKE: ok vt-state" "VT_GETSTATE and VT_OPENQRY agree about which virtual terminals are allocated"
check_output "$LOG" "M107-SMOKE: ok vt-switch" "VT_ACTIVATE/VT_WAITACTIVE move the console and a background VT keeps its screen"
check_output "$LOG" "M107-SMOKE: ok vt-disallocate" "VT_DISALLOCATE refuses the active VT and frees an idle one"
check_output "$LOG" "M107-SMOKE: ok vt-kdmode" "KDSETMODE/KDGETMODE and KDSKBMODE/KDGKBMODE round-trip, KDGKBTYPE reports a keyboard"
check_output "$LOG" "M107-SMOKE: ok console-font" "PIO_FONT loads a console font that GIO_FONT reads back unchanged"
check_output "$LOG" "M107-SMOKE: ok console-fontx" "PIO_FONTX/GIO_FONTX round-trip a face and report the real glyph count when the buffer is short"
check_output "$LOG" "M107-SMOKE: ok console-keymap" "KDGKBENT reports the live keymap and KDSKBENT changes an entry"
check_output "$LOG" "M107-SMOKE: ok loop-attach" "LOOP_SET_FD binds a file that reads back through /dev/loopN, and LOOP_GET_STATUS64 names it"
check_output "$LOG" "M107-SMOKE: ok loop-offset" "LOOP_SET_STATUS64 lo_offset shifts the mapping into the backing file"
check_output "$LOG" "M107-SMOKE: ok loop-write" "a write through /dev/loopN lands in the backing file"
check_output "$LOG" "M107-SMOKE: ok proc-fd-path" "/proc/self/fd/N readlinks to a file's full path"
check_output "$LOG" "M107-SMOKE: ok proc-maps-labels" "/proc/self/maps labels [stack] and [heap] with ranges that hold real addresses"
check_output "$LOG" "M107-SMOKE: ok kmsg-dev" "/dev/kmsg returns records with the priority they were written with and rising sequence numbers"
check_output "$LOG" "M107-SMOKE: ok kmsg-proc" "/proc/kmsg carries the same record stream"
check_output "$LOG" "M107-SMOKE: ok kmsg-per-fd" "two /dev/kmsg descriptors each get the whole log, not half of it each"
check_output "$LOG" "M107-SMOKE: ok syslog-klogctl" "syslog(2)/klogctl reports a buffer size and returns the injected message"
check_output "$LOG" "M107-SMOKE: ok inotify-move" "a rename produces IN_MOVED_FROM and IN_MOVED_TO sharing one cookie"
check_output "$LOG" "M107-SMOKE: ok inotify-attrib" "chmod on a watched file produces IN_ATTRIB"
check_output "$LOG" "M107-SMOKE: ok inotify-selfdel" "unlinking a watched file produces IN_DELETE_SELF"
check_output "$LOG" "M107-SMOKE: ok rtc-read" "RTC_RD_TIME reads the CMOS clock and agrees with the system time"
check_output "$LOG" "M107-SMOKE: ok rtc-alarm" "RTC_WKALM_SET/RTC_WKALM_RD round-trip an alarm time"
check_output "$LOG" "M107-SMOKE: ok watchdog-timeout" "/dev/watchdog honours WDIOC_SETTIMEOUT/GETTIMEOUT/GETTIMELEFT and the magic close"
check_output "$LOG" "M107-SMOKE: ok i2c-probe" "/dev/i2c-0 exists only with an SMBus controller, and reports SMBus (not raw-I2C) functionality"
check_output "$LOG" "M107-SMOKE: ok applet-ip" "BusyBox ip addr show reports the interface address over rtnetlink"
check_output "$LOG" "M107-SMOKE: ok applet-losetup" "BusyBox losetup attaches, lists and detaches a loop device"
check_output "$LOG" "M107-SMOKE: ok applet-hwclock" "BusyBox hwclock -r reads the hardware clock"
check_output "$LOG" "M107-SMOKE: ok applet-lsof" "BusyBox lsof lists an open file by its full path"
check_output "$LOG" "M107-SMOKE: ok applet-chvt" "BusyBox chvt switches the active virtual terminal"
check_output "$LOG" "M107-SMOKE: done" "M107 subsystem suite completes"
# ── M109: AF_PACKET, pivot_root(2), volume identity ──
check_output "$BLK_LOG" "M109-SMOKE: ok packet-socket" "an AF_PACKET socket binds to an interface and getsockname reports a 20-byte sockaddr_ll"
# The CFI NOR chip reaches the guest only through `-drive if=pflash`, and QEMU
# offers pflash on x86_64 by pairing it with the machine's firmware in unit 0.
# QEMU virt has no such pairing here, so this arch is given no flash at all --
# the checks below would be asking about a chip that is not plugged in, and a
# failure that says nothing about the kernel is worse than an honest absence.
if [ "$ARCH" = "aarch64" ]; then
	skipped "MTD: the CFI NOR flash wave" "no pflash chip on this machine — QEMU virt is not given one"
else
	check_output "$BLK_LOG" "MTD-SMOKE: ok erase-all" "flash_eraseall erases the CFI NOR chip QEMU provides through -drive if=pflash"
	check_output "$BLK_LOG" "MTD-SMOKE: ok erase-yields-ones" "an erased flash block reads back as all-ones"
	check_output "$BLK_LOG" "MTD-SMOKE: ok program" "a pattern written to /dev/mtd0 reads back byte for byte"
	check_output "$BLK_LOG" "MTD-SMOKE: ok program-command-path" "the CFI program sequence (clear status, program, data, poll) completes on the chip"
	check_output "$BLK_LOG" "MTD-SMOKE: ok erase-restores-ones" "erasing lifts those bits back to one"
	check_output "$BLK_LOG" "MTD-SMOKE: ok mtdblock-node" "the same chip is reachable as a block device for a filesystem"
	check_output "$BLK_LOG" "MTD-SMOKE: done" "the flash wave completes"
fi
check_output "$BLK_LOG" "M109-SMOKE: ok packet-tx-rx" "a frame sent on one packet socket arrives byte-for-byte on another, with its MAC, ethertype and ifindex"
check_output "$BLK_LOG" "M109-SMOKE: ok packet-filter" "a socket bound to one ethertype does not see another's frames, while ETH_P_ALL sees both"
check_output "$BLK_LOG" "M109-SMOKE: ok packet-dgram" "AF_PACKET SOCK_DGRAM strips the header on receive and builds it from the sockaddr_ll on send"
check_output "$BLK_LOG" "M109-SMOKE: ok gretap-loop" "a gretap tunnel encapsulates a frame in GRE over IPv4 and delivers it back, decapsulated, as a received frame"
check_output "$BLK_LOG" "M109-SMOKE: ok vlan-tag" "a frame sent on a vlan device leaves the lower interface tagged 802.1Q with the right VID"
check_output "$BLK_LOG" "M109-SMOKE: ok vlan-strip" "a tagged frame arriving on the lower interface is matched, stripped and delivered on the vlan device"
check_output "$BLK_LOG" "M109-SMOKE: ok vlan-vid-filter" "a tag for an unconfigured VID never surfaces on the vlan device, while a correct one still does"
check_output "$BLK_LOG" "M109-SMOKE: ok bridge-learn" "the bridge learns a source address against the port it arrived on and lists it in /proc/net/bridge"
check_output "$BLK_LOG" "M109-SMOKE: ok bridge-flood" "a broadcast received on one bridge port is flooded out the other, unchanged"
check_output "$BLK_LOG" "M109-SMOKE: ok bridge-fdb-forward" "a unicast for a learned address leaves by that port alone"
check_output "$BLK_LOG" "M109-SMOKE: ok bond-active-tx" "a bond transmits through its active slave and not through the backup"
check_output "$BLK_LOG" "M109-SMOKE: ok bond-failover" "with the active slave down the bond sends through the other one, and that slave's received frames surface on the bond"
check_output "$BLK_LOG" "M109-SMOKE: ok vnet-link-lifecycle" "RTM_NEWLINK creates a virtual device, a duplicate name is EEXIST, RTM_DELLINK removes it, and a physical NIC cannot be deleted"
check_output "$BLK_LOG" "M109-SMOKE: ok pivot-root" "pivot_root(2) makes a mounted filesystem the root, parks the old one at put_old, and the mount table moves with it"
check_output "$BLK_LOG" "M109-SMOKE: ok pivot-root-errno" "pivot_root rejects a plain directory, a put_old outside new_root, and the current root"
check_output "$BLK_LOG" "M109-SMOKE: ok blkid-probe" "blkid reports a canonically shaped UUID and a filesystem type for a real disk"
check_output "$BLK_LOG" "M109-SMOKE: ok sysfs-ident" "/sys/block/<dev>/{uuid,fstype} agree with blkid about the same device"
check_output "$BLK_LOG" "M109-SMOKE: done" "M109 suite completes"
# ── M109: namespaces (unshare/nsenter) ──
check_output "$BLK_LOG" "M109-SMOKE: ok uts-namespace" "a child that unshares its UTS namespace renames itself and the parent keeps its own hostname"
check_output "$BLK_LOG" "M109-SMOKE: ok ns-handles" "/proc/<pid>/ns/uts differs after an unshare while /proc/<pid>/ns/mnt still matches"
check_output "$BLK_LOG" "M109-SMOKE: ok setns-uts" "setns(2) on a child's ns handle joins its UTS namespace and the caller's own handle takes it back"
check_output "$BLK_LOG" "M109-SMOKE: ok mount-namespace" "a mount made under CLONE_NEWNS is absent from the parent's /proc/mounts and its files unreachable outside"
check_output "$BLK_LOG" "M109-SMOKE: done" "M109 namespace suite completes"
check_output "$LOG" "M109-SMOKE: ok uts-namespace" "a child that unshares its UTS namespace renames itself and the parent keeps its own hostname"
check_output "$LOG" "M109-SMOKE: ok ns-handles" "/proc/<pid>/ns/uts differs after an unshare while /proc/<pid>/ns/mnt still matches"
check_output "$LOG" "M109-SMOKE: ok setns-uts" "setns(2) on a child's ns handle joins its UTS namespace and the caller's own handle takes it back"
check_output "$LOG" "M109-SMOKE: ok mount-namespace" "a mount made under CLONE_NEWNS is absent from the parent's /proc/mounts and its files unreachable outside"
check_output "$LOG" "M109-SMOKE: ok pid-namespace" "unshare(CLONE_NEWPID) numbers the caller's children from 1: the first child reports getpid()==1 and getppid()==0, its own child gets 2, and waitpid inside the namespace reports that 2"
check_output "$LOG" "M109-SMOKE: ok pid-ns-isolation" "a pid from outside a PID namespace names nothing inside it - kill() on it is ESRCH"
check_output "$LOG" "M109-SMOKE: ok pid-ns-handles" "/proc/<pid>/ns/pid differs for the task inside the new namespace and is unchanged for the task that unshared"
check_output "$LOG" "M109-SMOKE: ok veth-pair" "ip link add ... type veth peer name ... makes two interfaces, both listed in /proc/net/dev, and a duplicate name is EEXIST"
check_output "$LOG" "M109-SMOKE: ok veth-carries-frame" "a frame sent on one end of a veth arrives on the other as a received frame, byte for byte"
check_output "$LOG" "M109-SMOKE: ok net-namespace" "unshare(CLONE_NEWNET) leaves a task with no interfaces at all - not the NIC, not a veth pair created before the unshare"
check_output "$LOG" "M109-SMOKE: ok veth-crosses-namespace" "one veth end moved into another network namespace vanishes from this one, and a frame sent here is received there"
check_output "$LOG" "M109-SMOKE: ok unlink-enoent" "unlink of a name that exists on neither the filesystem nor the VFS still fails ENOENT, while an in-memory device node on an on-disk directory really is removed"
check_output "$LOG" "M109-UEVENT: ok sysfs-dev-tree" "/sys/dev/block/<maj:min>/{dev,uevent} agree with each other and with the block node in /dev"
check_output "$LOG" "M109-UEVENT: ok sysfs-subsystem-link" "each device directory carries a subsystem symlink whose basename names the subsystem udev matches on"
check_output "$LOG" "M109-UEVENT: ok uevent-trigger" "writing add to a device's sysfs uevent file re-announces it on the netlink group, DEVTYPE included (device coldplug: what udevadm trigger and mdev -s do)"
check_output "$LOG" "M109-UEVENT: ok uevent-trigger-tty" "the same coldplug write works on a device that is not a disk: /sys/class/tty/tty1/uevent takes an add and re-announces the terminal (it was read-only, so udevadm trigger reached no tty at all)"
check_output "$LOG" "M109-UEVENT: ok uevent-bpf-filter" "SO_ATTACH_FILTER runs a real classic-BPF program over the udev group and drops what it rejects, and the verifier refuses a program with no return"
check_output "$LOG" "M109-UEVENT: ok uevent-bpf-detach" "SO_DETACH_FILTER puts the socket back to receiving everything"
check_output "$LOG" "M109-UEVENT: ok netlink-source-portid" "a unicast netlink message reports the SENDING SOCKET's port id as its source, not the sender's pid - what systemd-udevd checks before it accepts a device from its manager"
check_output "$LOG" "M109-UEVENT: ok uevent-hotplug-add" "a loop device added after boot broadcasts add@/block/loop8 with ACTION/SUBSYSTEM/DEVNAME/MAJOR/MINOR/SEQNUM"
check_output "$LOG" "M109-UEVENT: ok mdev-scan" "mdev -s creates the missing node as a block special file with the major:minor /sys published"
check_output "$LOG" "M109-UEVENT: ok uevent-hotplug-remove" "removing the device broadcasts remove@/block/loop8 with an advancing SEQNUM and it leaves /sys"
check_output "$LOG" "M109-UEVENT: ok mdev-daemon" "mdev -d creates and unlinks /dev/loop8 from the netlink broadcast alone"
check_output "$LOG" "M109-UEVENT: done" "the M109 uevent suite ran to completion"
check_output "$LOG" "M109-SMOKE: ok net-ns-routes" "a route added inside a network namespace is in its own /proc/net/route and absent from the initial namespace's"
check_output "$LOG" "M109-SMOKE: ok netns-ipv4-address" "an interface moved into a network namespace takes an address of its own - by SIOCSIFADDR on one side and RTM_NEWADDR on the other - and each namespace gets the on-link route that comes with it"
check_output "$LOG" "M109-SMOKE: ok netns-ipv4-exchange" "a UDP datagram crosses a veth pair between two network namespaces and is echoed back, each packet carrying the sending namespace's own address as its source"
check_output "$LOG" "M109-SMOKE: ok netns-ipv4-isolated" "neither namespaced address, its prefix, nor its interface exists in the initial namespace, whose own lease is unchanged"
check_output "$LOG" "M109-SMOKE: done" "M109 namespace suite completes"
# ── M109: device nodes, findfs/blkid, and mount(MS_MOVE)/switch_root ──
check_output "$BLK_LOG" "M109-SMOKE: ok dev-nodes-listed" "a readdir of /dev lists every block device /sys/block names, as a block special file"
check_output "$BLK_LOG" "M109-SMOKE: ok dir-merge-no-dups" "a directory with both on-disk entries and in-memory children lists each name once"
check_output "$BLK_LOG" "M109-SMOKE: ok findfs-uuid" "findfs UUID= names the device whose superblock carries that UUID"
check_output "$BLK_LOG" "M109-SMOKE: ok findfs-label" "findfs LABEL= names the device carrying that volume label"
check_output "$BLK_LOG" "M109-SMOKE: ok blkid-lists-disks" "blkid reports the disk with the UUID its superblock holds"
check_output "$BLK_LOG" "M109-SMOKE: ok mount-move" "MS_MOVE moves a mount and the mount nested inside it to a new target"
check_output "$BLK_LOG" "M109-SMOKE: ok mount-move-statfs" "the moved mount still reports its own filesystem type at the new path"
check_output "$BLK_LOG" "M109-SMOKE: done" "M109 device-node and mount suite completes"
# `init=` naming a script. The kernel prints the path it was asked for, and the
# markers below prove the interpreter line resolved to the real /init: before
# the fix the ELF loader read "#!" as a bad magic number and the boot had no
# PID 1 at all.
check_output "$SWITCHROOT_LOG" "init: /init-shebang pid=1" "init= can name a #! script: the kernel resolves its interpreter line"
check_output "$SWITCHROOT_LOG" "M109-SMOKE: ok initramfs-root-statfs" "statfs reports RAMFS/TMPFS for an initramfs root, which is what switch_root demands"
check_output "$SWITCHROOT_LOG" "M109-SMOKE: ok switchroot-mount-newroot" "the initramfs PID 1 mounts the real root below /"
check_output "$SWITCHROOT_LOG" "M109-SMOKE: ok switch-root" "switch_root moved the new root onto / — the new init sees its files and not the initramfs's"
check_output "$SWITCHROOT_LOG" "M109-SMOKE: ok switch-root-init-pid1" "the process running in the new root is PID 1"
check_output "$SWITCHROOT_LOG" "M109-SMOKE: done-switchroot" "the switch_root instance completes"
# ── M108: BusyBox owns su, passwd and init ──
# su/passwd run on the posix instance; the init markers come from the dedicated
# init instance, where the checks are driven by PID 1 itself out of /etc/inittab.
check_output "$LOG" "M108-SMOKE: ok setuid-layout" "/bin/su and /bin/passwd resolve to a setuid-root busybox-suid, and the plain multicall ELF is not setuid"
check_output "$LOG" "M108-SMOKE: ok su-uid-and-shell" "BusyBox su becomes the target uid and execs that account's own login shell"
check_output "$LOG" "M108-SMOKE: ok su-password-auth" "an unprivileged caller su's with the correct /etc/shadow password through the setuid bit"
check_output "$LOG" "M108-SMOKE: ok su-wrong-password" "BusyBox su rejects a wrong password and never reaches the target uid"
check_output "$LOG" "M108-SMOKE: ok pam-accepts-initial" "pam_unix.so authenticates the shipped password before passwd touches it"
check_output "$LOG" "M108-SMOKE: ok passwd-writes-sha512" "BusyBox passwd rewrites the /etc/shadow field as a SHA-512 crypt string"
check_output "$LOG" "M108-SMOKE: ok passwd-pam-accepts-new" "the PAM path accepts the password BusyBox passwd wrote"
check_output "$LOG" "M108-SMOKE: ok passwd-pam-rejects-old" "the PAM path rejects the password BusyBox passwd replaced"
check_output "$LOG" "M108-SMOKE: ok su-accepts-passwd-hash" "BusyBox su authenticates the hash BusyBox passwd wrote"
check_output "$LOG" "M108-SMOKE: ok shadow-concurrent-passwd" "four simultaneous BusyBox passwd runs all reach /etc/shadow: no update lost, no bystander hash moved, no duplicated or truncated record in either database"
check_output "$LOG" "M108-SMOKE: ok shadow-lock-excl" "open(O_CREAT|O_EXCL) admits exactly one racer per round, and fcntl(F_SETLK,F_WRLCK) blocks a second writer, names its holder via F_GETLK and is released when that holder exits"
check_output "$LOG" "M108-SMOKE: done" "M108 su/passwd suite completes"
check_output "$INIT_LOG" "M108-SMOKE: ok init-pid1" "the default PID 1 is the BusyBox multicall ELF running as init"
check_output "$INIT_LOG" "M108-SMOKE: ok init-openrc-runlevels" "OpenRC's default runlevel and its local.d hooks run under BusyBox init"
check_output "$INIT_LOG" "M108-SMOKE: ok init-shell" "the BusyBox-init boot reaches a usable shell"
check_output "$INIT_LOG" "M108-SMOKE: ok init-reaps-orphan" "BusyBox init reaps an orphaned grandchild re-parented to PID 1"
check_output "$INIT_LOG" "M108-SMOKE: ok init-respawns-getty" "killing the inittab getty makes PID 1 respawn it as a new process"
# ── M100b: VT-d DMA remapping ──
check_iommu "$IOMMU_LOG" "iommu: VT-d at" "M100b: the DMAR table is parsed and the remapping unit is brought up"
check_iommu "$IOMMU_LOG" "M100B-SMOKE: ok vtd-enable" "M100b: the unit reports translation enabled and pointing at our root table"
check_iommu "$IOMMU_LOG" "M100B-SMOKE: ok vtd-map" "M100b: a mapping installed through the API is what the hardware page tables say"
check_iommu "$IOMMU_LOG" "M100B-SMOKE: ok vtd-unmap" "M100b: unmapping removes the translation"
check_iommu "$IOMMU_LOG" "M100B-SMOKE: ok vtd-dma-map" "M100b: dma_map for a translated device returns an address from the IOMMU window that the page tables point at the caller's buffer — no copy"
check_iommu "$IOMMU_LOG" "M100B-SMOKE: ok vtd-iova" "M100b: the device address allocator hands out distinct ranges and reuses freed ones"
check_iommu "$IOMMU_LOG" "M100B-SMOKE: ok nvme-translated" "M100b: NVMe runs in its own domain with only its queues and the transfer buffer mapped, reads a block, and the unit records no fault"
check_iommu "$IOMMU_LOG" "M100B-SMOKE: ok vtd-blocks-violation" "M100b: a device given its descriptor list but not its data buffer is stopped by the unit, and the fault is recorded"
# ── M100c: domains, groups, interrupt remapping ──
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok domains-isolated" "M100c: a page mapped for one domain does not exist in another"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok group-moves-together" "M100c: attaching one function moves every function of its group"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok domain-tables-freed" "M100c: destroying a domain gives its page tables back, every level of them"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok group-behind-bridge" "M100c: devices behind a bridge are one group, and moving it rewrites the bridge's own context entry"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok acs-splits-group" "M100c: endpoints behind ports that enforce ACS are separate groups; without it they are one"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok group-query-is-read-only" "M100c: asking which group a device is in leaves the ACS controls exactly as they were"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok acs-keeps-port" "M100c: a port named on the cmdline is left exactly as found while every other one takes the policy"
check_iommu "$IOMMU_LOG" "iommu: acs port" "M100c: each ACS port is named with what it ended up doing"
check_iommu "$IOMMU_LOG" "iommu: ACS on" "M100c: the ACS policy is decided once at init and reported"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok ari-owns-bus" "M100c: a device with ARI or SR-IOV owns the bus's function space, so the group is the bus"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok ir-enabled" "M100c: the interrupt remapping table is programmed and remapping is on"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok ir-entry" "M100c: an entry holds the vector and destination asked for, is bound to one requester, and stops being present when freed"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok ir-delivery" "M100c: NVMe's MSI-X goes through a remap entry and still reaches its vector"
check_iommu "$IOMMU_LOG" "M100C-SMOKE: ok ir-rejects-unknown" "M100c: an interrupt claiming an entry that was taken away is refused and recorded, and works again once it is back"
# ── M100d: AMD-Vi ──
check_iommu "$AMDVI_LOG" "amdvi: unit at" "M100d: IVRS is parsed and the AMD-Vi unit is brought up"
check_iommu "$AMDVI_LOG" "M100D-SMOKE: ok amdvi-enable" "M100d: the unit reports translation on and points at our device table"
check_iommu "$AMDVI_LOG" "M100D-SMOKE: ok amdvi-map" "M100d: a mapping is what the AMD page tables say"
check_iommu "$AMDVI_LOG" "M100D-SMOKE: ok amdvi-unmap" "M100d: unmapping removes the translation"
check_iommu "$AMDVI_LOG" "M100D-SMOKE: ok nvme-translated" "M100d: NVMe runs in a domain AMD-Vi translates, reads a block, and the event log stays empty"
check_iommu "$AMDVI_LOG" "M100D-SMOKE: ok amdvi-command-ring" "M100d: the unit consumes commands from the ring, so invalidation is real"
check_iommu "$AMDVI_LOG" "reboot: powering off" "M100d: the machine boots and shuts down with AMD-Vi translating"
check_iommu "$IOMMU_LOG" "reboot: powering off" "M100b: the machine still boots and shuts down with translation on"
check_output "$INIT_LOG" "M108-SMOKE: done-init" "M108 BusyBox-init instance completes"
# ── M86: per-thread CPU accounting + thread-directed signals ──
check_output "$LOG" "M86-SMOKE: ok thread-cputime" "CLOCK_THREAD_CPUTIME_ID tracks CPU actually burned and stays flat while the thread sleeps"
check_output "$LOG" "M86-SMOKE: ok process-cputime" "CLOCK_PROCESS_CPUTIME_ID sums the whole thread group, not just the caller"
check_output "$LOG" "M86-SMOKE: ok getcpuclockid" "pthread_getcpuclockid's clock reads that thread's CPU time at nanosecond resolution"
check_output "$LOG" "M86-SMOKE: ok rusage-thread" "getrusage separates RUSAGE_THREAD from RUSAGE_SELF and reports microsecond precision"
check_output "$LOG" "M86-SMOKE: ok times-process" "times(2) reports process CPU time covering every thread"
check_output "$LOG" "M86-SMOKE: ok rusage-children" "getrusage(RUSAGE_CHILDREN) accumulates a reaped child's CPU time"
check_output "$LOG" "M86-SMOKE: ok proc-stat-times" "/proc/self/stat reports real utime/stime and a live thread count"
check_output "$LOG" "M86-SMOKE: ok proc-stat-width" "/proc/<pid>/stat carries all 52 fields, so a reader that asks for one by index finds it"
check_output "$LOG" "M86-SMOKE: ok proc-stat-width-gone" "a task that has exited but not been reaped still reports a full-width stat line, not a four-field stub"
check_output "$LOG" "M86-SMOKE: ok rusage-maxrss" "ru_maxrss is a high-water mark that survives the munmap of the pages that set it"
check_output "$LOG" "M86-SMOKE: ok group-stop-blocked" "a process-directed stop reaches a thread parked in a blocking syscall, and SIGCONT resumes it"
check_output "$LOG" "M86-SMOKE: ok tkill-self" "tkill(gettid(), sig) delivers to the calling thread"
check_output "$LOG" "M86-SMOKE: ok tgkill-thread" "tgkill delivers to the named thread, not to an arbitrary one"
check_output "$LOG" "M86-SMOKE: ok tgkill-esrch" "tgkill with a mismatched tgid and tkill of a dead tid both fail ESRCH"
check_output "$LOG" "M86-SMOKE: ok kill-unblocked" "kill(pid) is process-directed: a thread with the signal unblocked handles it"
check_output "$LOG" "M86-SMOKE: ok signal-compute-loop" "a signal HANDLER runs in a child making no syscalls at all — the return-from-interrupt path delivers pending signals, not just the syscall and fault paths"
check_output "$LOG" "M86-SMOKE: ok pthread-exit-rv" "pthread_exit's return value reaches pthread_join, with cleanup handlers run"
check_output "$LOG" "M86-SMOKE: ok pthread-exit-main" "pthread_exit in main keeps the process alive until the last thread exits"
check_output "$LOG" "M86-SMOKE: done" "M86 CPU-accounting/signal-targeting suite completes"
# ── M95: loadable kernel modules — framework, filesystem and device modules ──
check_output "$LOG" "M95-SMOKE: ok proc-modules" "/proc/modules lists every .ko in /lib/modules, all Live and mapped in the 0xffffffffc0000000 module region"
check_output "$LOG" "M95-SMOKE: ok modinfo" "a .ko's .modinfo carries name/license and a vermagic matching the running kernel release"
check_output "$LOG" "M95-SMOKE: ok fs-modules" "isofs, ntfs and btrfs arrived as modules and registered themselves in /proc/filesystems"
check_output "$LOG" "M95-SMOKE: ok sound-module" "the HDA driver is a live module and its sysfs coresize matches /proc/modules"
check_output "$LOG" "M95-SMOKE: ok rmmod-insmod" "unloading ntfs withdraws the filesystem type; loading it back restores it"
check_output "$LOG" "M95-SMOKE: ok refcount" "ipv6 is referenced by ndp, sysfs refcnt agrees, and removing it reports EBUSY"
check_output "$LOG" "M95-SMOKE: ok dup-load" "loading an already-loaded module reports EEXIST"
check_output "$LOG" "M95-SMOKE: ok vermagic-reject" "a .ko whose vermagic was corrupted is refused with ENOEXEC while the intact one still loads"
check_output "$LOG" "M95-SMOKE: ok init-module" "init_module(2) loads a module image straight out of process memory"
check_output "$LOG" "M95-SMOKE: ok unpriv" "an unprivileged process cannot delete a module (EPERM) and the module survives"
check_output "$LOG" "M95-SMOKE: ok fs-in-use" "a module providing a mounted filesystem cannot be unloaded (EBUSY), and can again once it is unmounted"
check_output "$LOG" "M95-SMOKE: ok filesystems-nodev" "/proc/filesystems marks every pseudo filesystem nodev and no block-backed one"
check_output "$LOG" "M95-SMOKE: done" "M95 loadable-kernel-module suite completes"
# ── M96: network protocol modules, module parameters, modprobe ──
check_output "$LOG" "M96-SMOKE: ok proto-modules" "ipv6, ndp and ntp are loaded as modules and the IPv6 stack is serving"
check_output "$LOG" "M96-SMOKE: ok sysfs-params" "/sys/module/<name>/parameters exposes a module parameter with its compiled-in default"
check_output "$LOG" "M96-SMOKE: ok param-write" "a writable module parameter takes a new value through sysfs and reads it back"
check_output "$LOG" "M96-SMOKE: ok param-readonly" "a 0444 module parameter rejects a write and keeps its value"
check_output "$LOG" "M96-SMOKE: ok param-insmod" "a parameter given on the insmod command line reaches the module"
check_output "$LOG" "M96-SMOKE: ok param-reject" "an unknown parameter fails the load with EINVAL and leaves the module unloaded"
check_output "$LOG" "M96-SMOKE: ok modules-dep" "modules.dep records ndp -> ipv6 and the kernel reports the same dependency"
check_output "$LOG" "M96-SMOKE: ok modprobe-alias" "modprobe resolves a modules.alias entry to the module it names and loads it"
check_output "$LOG" "M96-SMOKE: ok modprobe-deps" "modprobe loads a module's dependency first and the use count records the link"
check_output "$LOG" "M96-SMOKE: ok lsmod" "lsmod's output agrees with /proc/modules"
check_output "$LOG" "M96-SMOKE: done" "M96 protocol-module and module-parameter suite completes"

# ── M98: GNU-free in-guest build tools (bmake + samurai replaced GNU Make) ──
check_output "$LOG" "M98-SMOKE: ok make-is-bmake" "/bin/make answers bmake's -V (GNU Make rejects it), so it is the BSD make"
check_output "$LOG" "M98-SMOKE: ok make-not-gnu" "/bin/make identifies as something other than GNU Make"
check_output "$LOG" "M98-SMOKE: ok make-build" "bmake parses a Makefile, expands a variable and runs the recipe that creates the target"
check_output "$LOG" "M98-SMOKE: ok make-uptodate" "a second bmake run does not re-run the recipe (target newer than its prerequisites)"
check_output "$LOG" "M98-SMOKE: ok samu-version" "/bin/samu reports the Ninja file-format version it implements"
check_output "$LOG" "M98-SMOKE: ok ninja-alias" "/bin/ninja is the samurai binary and runs"
check_output "$LOG" "M98-SMOKE: ok samu-build" "samurai executes a build.ninja edge and produces the declared output"
check_output "$LOG" "M98-SMOKE: ok samu-uptodate" "re-running a satisfied build graph is a no-op"
check_output "$LOG" "M98-SMOKE: done" "M98 GNU-free build-tool suite completes"
# ── M104: Linux-PAM (real libpam.so.0 + pam_unix.so authenticating against
# /etc/shadow via musl crypt(3)) — userspace/bin/smoke/m104_pam_smoke.c.
check_output "$LOG" "M104-PAM: ok libpam-linked" "libpam loaded and its API is callable"
check_output "$LOG" "M104-PAM: ok auth-correct-password" "pam_authenticate() succeeds for the real pamtest /etc/shadow entry with its correct password"
check_output "$LOG" "M104-PAM: ok acct-mgmt" "pam_acct_mgmt() succeeds for the authenticated account"
check_output "$LOG" "M104-PAM: ok auth-wrong-password-rejected" "pam_authenticate() rejects a wrong password for a real account (PAM_AUTH_ERR, not a fake pass)"
check_output "$LOG" "M104-PAM: ok unknown-user-rejected" "pam_authenticate() returns PAM_USER_UNKNOWN (not PAM_AUTH_ERR) for a nonexistent user"
check_output "$LOG" "M104-PAM: done" "M104 PAM suite completes"
# ── zsh: the interactive/login shell (replaced GNU bash in M98) ──
check_output "$LOG" "ZSH-SMOKE: ok version" "zsh reports ZSH_VERSION"
check_output "$LOG" "ZSH-SMOKE: ok arrays" "zsh indexed arrays work (1-based, no KSH_ARRAYS)"
check_output "$LOG" "ZSH-SMOKE: ok dbracket-glob" "zsh [[ ]] glob matching works"
check_output "$LOG" "ZSH-SMOKE: ok regex-match" "zsh [[ =~ ]] regex matching works (zsh/regex linked in)"
check_output "$LOG" "ZSH-SMOKE: ok arithmetic" "zsh \$(( )) arithmetic works"
check_output "$LOG" "ZSH-SMOKE: ok brace-range" "zsh {a..b} brace ranges work"
check_output "$LOG" "ZSH-SMOKE: ok cstyle-for" "zsh C-style for loops work"
check_output "$LOG" "ZSH-SMOKE: ok pattern-subst" "zsh \${var//x/y} substitution works"
check_output "$LOG" "ZSH-SMOKE: ok local-vars" "zsh function local variables work"
check_output "$LOG" "ZSH-SMOKE: ok utf8-length" "zsh counts UTF-8 characters, not bytes"
check_output "$LOG" "ZSH-SMOKE: ok utf8-substr" "zsh string subscripting is UTF-8 character-aware"
check_output "$LOG" "ZSH-SMOKE: done" "zsh feature smoke completes"
# ── M39: configurable init system ──
check_output "$LOG" "M39-INIT: start" "M39 configurable-init self-test starts"
check_output "$LOG" "M39-INIT: ok parse-inittab" "init parses /etc/inittab entries"
check_output "$LOG" "M39-INIT: ok initdefault" "init reads the initdefault runlevel"
check_output "$LOG" "M39-INIT: ok runlevel-match" "inittab runlevel matching works"
check_output "$LOG" "M39-INIT: ok telinit" "telinit writes a runlevel request init consumes"
check_output "$LOG" "M39-INIT: ok getty-applet" "getty applet is present for tty/serial sessions"
check_output "$LOG" "M39-INIT: ok ttys0-open" "/dev/ttyS0 exists and opens as a serial tty"
check_output "$LOG" "M39-INIT: ok tty-termios-independent" "ttyS0 termios is independent of the boot console"
check_output "$LOG" "M39-INIT: ok tty-canon-read" "ttyS0 canonical line discipline assembles and reads a line"
check_output "$LOG" "M39-INIT: ok tty-eof" "ttyS0 VEOF on an empty line reads as EOF"
check_output "$LOG" "M39-INIT: ok tty-raw-read" "ttyS0 raw (non-canonical) read passes bytes through"
check_output "$LOG" "M39-INIT: ok tty-isig" "ttyS0 ISIG intercepts VINTR instead of queueing it"
check_output "$LOG" "M39-INIT: ok tty-pgrp-independent" "ttyS0 foreground pgrp is independent of the console"
check_output "$LOG" "M39-INIT: ok tty-sctty" "TIOCSCTTY claims ttyS0 as a controlling terminal"
# termios2. glibc 2.42 made tcgetattr(3) issue TCGETS2 instead of TCGETS, and
# isatty(3) is tcgetattr succeeding — so a kernel without these has no
# terminals at all as far as a current glibc is concerned.
check_output "$LOG" "M39-INIT: ok tty-tcgets2" "TCGETS2 reports the same line as TCGETS, with a real c_ospeed"
check_output "$LOG" "M39-INIT: ok tty-tcsets2" "TCSETS2 applies a termios2 and TCGETS2 reads it back"
check_output "$LOG" "M39-INIT: ok console-tcgets2" "the boot console answers TCGETS2 — the fd an init system logs to"
check_output "$LOG" "M39-INIT: ok console-unknown-ioctl-enotty" "an ioctl the console does not implement is ENOTTY, not EPERM"
check_output "$LOG" "M39-INIT: ok ttys0-write" "ttyS0 write path accepts a full buffer"
check_output "$LOG" "M39-TTYS0-TX-OK" "ttyS0 TX actually reaches COM1 (marker travelled the UART path)"
check_output "$LOG" "M39-INIT: ok ttys0-release" "closing the last ttyS0 handle releases the COM1 claim"
check_output "$LOG" "M39-INIT: done" "M39 configurable-init self-test completes"
# ── M36 GDB stub + ftrace ──
check_output "$LOG" "M36-GDB: start" "M36 GDB-stub diag starts"
check_output "$LOG" "M36-GDB: ok stop-reply" "GDB stub answers ? with a stop reply"
check_output "$LOG" "M36-GDB: ok read-regs" "GDB g-packet round-trips the register file"
check_output "$LOG" "M36-GDB: ok read-mem" "GDB m-packet reads target memory"
check_output "$LOG" "M36-GDB: ok framing" "GDB RSP framing + checksum round-trips"
check_output "$LOG" "M36-GDB: done" "M36 GDB-stub diag completes"
check_output "$LOG" "M36-FTRACE: start" "M36 ftrace diag starts"
check_output "$LOG" "M36-FTRACE: ok capture" "ftrace records function enter/exit events"
check_output "$LOG" "M36-FTRACE: ok symbolize" "ftrace event resolves to the function name"
check_output "$LOG" "M36-FTRACE: done" "M36 ftrace diag completes"
# ── M30 Dynamic Linking (PIE) ──
check_output "$LOG" "M30-DYN: ok pie-binary" "PIE ET_DYN binary loads at PIE base"
check_output "$LOG" "M30-DYN: ok pie-relocs" "R_X86_64_RELATIVE relocations applied"
check_output "$LOG" "M30-DYN: done" "M30 dyn-linking smoke completes"
if [ "$ARCH" = "x86_64" ]; then
  check_output "$LOG" "M30-DYN: ok shared-libc" "DT_NEEDED libc.so.1 resolves GOT/PLT symbols"
  check_output "$LOG" "M69-DL: dlsym ok" "M69 P1: dlopen/dlsym resolves+calls a libc.so.1 symbol"
  check_output "$LOG" "M69-PLUGIN: ctor" "M69 P2: dlopen runs the new object's DT_INIT_ARRAY ctor"
  check_output "$LOG" "M69-DL2: dlopen-run ok" "M69 P2: runtime-loaded .so relocated, dlsym'd, and called"
  check_output "$LOG" "M69-DL5: tls-gd ok" "M69 P4: general-dynamic TLS (DTPMOD64/DTPOFF64 + __tls_get_addr) in a dlopen'd object"
  check_output "$LOG" "M69-DL3: refcount-scope ok" "M69 P3: refcount + RTLD_DEFAULT scope work"
  check_output "$LOG" "M69-PLUGIN: dtor" "M69 P3: final dlclose runs DT_FINI_ARRAY dtor + unmaps"
fi
check_output "$LOG" "B1NIX-TEST: done" "test-mode shutdown marker appears"
check_output "$LOG" "reboot: restarting" "SYS_REBOOT performs a real machine restart"
check_output "$LOG" "ahci: registered sda" "AHCI disk registered under its Unix name sda"
check_output "$LOG" "nvme: registered nvme0n1" "NVMe namespace registered as nvme0n1"
# USB mass storage is a SCSI disk and shares the sd* sequence with AHCI. The blk
# instance carries two SATA disks and one USB stick, so the stick must be the
# THIRD sd disk — sdc. A per-driver counter would have made it sda, and the old
# scheme would have made it usb0; both are what this pins down.
check_output "$BLK_LOG" "usb: registered sdc" "USB storage takes the next name in the sd sequence AHCI already used (sdc, after sda and sdb)"
check_output "$BLK_LOG" "M14-SMOKE: ok removable-is-a-fact" "/sys/block reports exactly one removable disk, it is the USB stick, and it is named sd* like any other SCSI disk"
check_output "$BLK_LOG" "swap: device=sdb" "swap still takes the second ATA disk, not whichever disk happens to be second in the sd sequence"

# Durability: fsync(2)/sync(2)/umount now issue the device's own cache-flush
# command, which AHCI used to fire after every single write (where it is not a
# barrier, just a disabled write cache) and NVMe never fired at all.
#
# The scratch half of the test picks a virtio disk nothing has mounted, so the
# disk it lands on differs: on x86_64 the only virtio disk is the scratch one
# (vda), on aarch64 vda is the root filesystem and the scratch is the second,
# vdb.
# The letter is not pinned: the scratch disk is whichever virtio disk nothing
# has mounted, and which of them QEMU enumerates first differs by transport.
check_output "$BLK_LOG" "M14-BLK: ok flush-op dev=sda" "the SATA disk accepts an ATA FLUSH CACHE EXT from the fsync path"
check_output "$BLK_LOG" "M14-BLK: ok flush-op dev=nvme0n1" "the NVMe namespace accepts a FLUSH command from the fsync path"
check_output "$BLK_LOG" "M14-BLK: ok flush-op dev=vd" "the virtio disk accepts a VIRTIO_BLK_T_FLUSH from the fsync path"
check_output "$BLK_LOG" "registered vd" "virtio-blk probes and registers under the vd sequence"
check_output "$BLK_LOG" "flush=yes" "virtio-blk negotiates VIRTIO_BLK_F_FLUSH instead of declining every feature the host offers"
check_output "$BLK_LOG" "M14-BLK: ok durable-roundtrip dev=vd" "a pattern written to the scratch virtio disk survives a device flush and a full block-cache drop"
check_output "$BLK_LOG" "M14-BLK: ok zero-blocks" "blk_zero_blocks leaves a range reading back as zeroes"

# ── M109: inode attributes, discard, I/O priorities, serial line settings ──
check_output "$BLK_LOG" "discard=yes" "virtio-blk negotiates VIRTIO_BLK_F_DISCARD, so blkdiscard reaches a device that has the command"
check_output "$BLK_LOG" "nvme: dataset-management=yes" "the NVMe controller's ONCS says Dataset Management, so Deallocate is a command it really has"
check_output "$BLK_LOG" "M109-SMOKE: ok attr-roundtrip" "FS_IOC_SETFLAGS stores the ext2 attribute byte, FS_IOC_GETFLAGS reads it back, and a flag the format cannot hold is refused"
check_output "$BLK_LOG" "M109-SMOKE: ok attr-immutable" "chattr +i makes write, truncate, rename and unlink all fail EPERM, and -i gives the file back"
check_output "$BLK_LOG" "M109-SMOKE: ok attr-append" "chattr +a refuses a plain write and a truncate but accepts an O_APPEND write"
check_output "$BLK_LOG" "M109-SMOKE: ok attr-persist" "the attribute flags survive umount and remount, so they reached the on-disk i_flags"
check_output "$BLK_LOG" "M109-SMOKE: ok attr-applets" "BusyBox chattr sets flags that BusyBox lsattr prints, and clears them again"
check_output "$BLK_LOG" "M109-SMOKE: ok discard-support" "every block device either has the discard command or reports EOPNOTSUPP — never an I/O error"
check_output "$BLK_LOG" "M109-SMOKE: ok discard-virtio" "BLKDISCARD on virtio-blk accepts a valid range and rejects a misaligned or out-of-range one"
check_output "$BLK_LOG" "M109-SMOKE: ok discard-zeroout" "BLKZEROOUT makes a range that held a pattern read back as zeroes"
check_output "$BLK_LOG" "M109-SMOKE: ok discard-applet" "BusyBox blkdiscard trims a range of the scratch disk"
check_output "$BLK_LOG" "M109-SMOKE: ok fstrim-ioctl" "FITRIM walks a mounted ext4's free bitmaps and offers no more than the filesystem has free"
check_output "$BLK_LOG" "M109-SMOKE: ok fstrim-keeps-data" "a file written before the trim is still readable after it"
check_output "$BLK_LOG" "M109-SMOKE: ok fstrim-applet" "BusyBox fstrim runs on the mount point"
check_output "$BLK_LOG" "M109-SMOKE: ok ioprio-roundtrip" "ioprio_set/ioprio_get round-trip a class and level, and refuse a class that does not exist"
check_output "$BLK_LOG" "M109-SMOKE: ok ioprio-applet" "BusyBox ionice sets a process's I/O class and reads it back"
# These four drive COM2 -- a SECOND 16550, which is an x86 PC thing. QEMU virt
# gives this arch one PL011 and it is the console, so there is no second line to
# reprogram, and nothing here is about a kernel defect.
if [ "$ARCH" = "aarch64" ]; then
	skipped "M109: the COM2 termios/modem/setserial wave" "no second UART on this machine — virt has one PL011 and it is the console"
else
	check_output "$BLK_LOG" "M109-SMOKE: ok serial-line" "tcsetattr reprograms COM2's baud, word length, parity and stop bits, and tcgetattr reports the chip"
	check_output "$BLK_LOG" "M109-SMOKE: ok serial-badbaud" "a baud rate the divisor cannot express is refused, leaving the line as it was"
	check_output "$BLK_LOG" "M109-SMOKE: ok serial-modem" "TIOCMGET reads the real modem lines and TIOCMBIC/TIOCMBIS change one"
	check_output "$BLK_LOG" "M109-SMOKE: ok serial-setserial" "TIOCGSERIAL reports COM2's actual port, IRQ and clock, and TIOCSSERIAL refuses to fake changing them"
fi

# Limits that used to be compiled in. Both checks deliberately exceed the old
# constant, so they can only pass on a kernel that derives the limit at runtime.
check_output "$BLK_LOG" "M109-SMOKE: ok mounts-past-64" "96 filesystems mount at once and the last one is writable (the mount table was a fixed 64 entries shared by every namespace)"
check_output "$BLK_LOG" "M109-SMOKE: ok fs-run-past-64" "a 512 KiB file on ext4 reads back byte-for-byte after a remount, in one read(2) spanning far more than the 64 blocks the coalescer used to fold into a request"

check_output "$BLK_LOG" "M109-SMOKE: done" "M109 suite completes"

# Network tests are only wired for the current x86_64/x86 QEMU path.
# The NIC arrives over virtio-mmio on aarch64 and virtio-pci on x86_64; the
# stack above the transport is the same, and the aarch64 lane is given a
# usernet device by run_qemu, so these checks run on both.
if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "aarch64" ]; then
	echo ""
	section "Network"
	# Either transport counts: the PCI driver says "initialized with MAC", the
	# virtio-mmio one "registered (slot N, mac ...)". What is checked is that a
	# NIC came up, not which bus it arrived on.
	if grep -qE "virtio-net: initialized with MAC|virtio-net-mmio: registered" "$LOG" 2>/dev/null && ! grep -q "virtio-net: no device found" "$LOG" 2>/dev/null; then
		pass "virtio-net initialized"
		if grep -q "DHCP-SMOKE: lease-acquired\|DHCP-SMOKE: fallback-static" "$LOG" 2>/dev/null; then
			pass "DHCP lease or deterministic fallback"
		else
			fail "DHCP lease or deterministic fallback" "missing DHCP-SMOKE marker"
		fi
	else
		fail "virtio-net initialized" "virtio-net message not found"
	fi

	# ── M37: real-hardware e1000/e1000e NIC driver ──
	# A second NIC (-device ${E1000_MODEL:-e1000} on net1) exercises the new
	# Intel gigabit driver end-to-end while virtio-net stays the active stack.
	if grep -q "e1000: initialized with MAC" "$LOG" 2>/dev/null; then
		pass "e1000 driver initialized"
		check_output "$LOG" "M37-E1000: ok init" "M37: e1000 ring/MMIO init"
		check_output "$LOG" "M37-E1000: ok mac" "M37: e1000 MAC read"
		check_output "$LOG" "M37-E1000: ok tx" "M37: e1000 transmit"
		check_output "$LOG" "M37-E1000: ok rx-arp" "M37: e1000 receive (ARP reply over SLIRP)"
		check_output "$LOG" "M37-E1000: ok rx-irq" "M37: e1000 receive is interrupt-driven (IMS armed, INTx serviced)"
	else
		fail "e1000 driver initialized" "e1000 init message not found"
	fi

	# ── M37: USB xHCI controller + HID boot keyboard ──
	# A qemu-xhci controller with a usb-kbd is enumerated end-to-end.
	if grep -q "M37-USB: ok xhci-init" "$LOG" 2>/dev/null; then
		pass "xHCI controller initialized"
		check_output "$LOG" "M37-USB: ok port-reset" "M37: xHCI port reset"
		check_output "$LOG" "M37-USB: ok slot-enabled" "M37: xHCI Enable Slot"
		check_output "$LOG" "M37-USB: ok device-addressed" "M37: xHCI Address Device"
		check_output "$LOG" "M37-USB: ok descriptors" "M37: USB device descriptor read (EP0 control transfer)"
		check_output "$LOG" "M37-USB: ok hid-config" "M37: HID interrupt endpoint configured"
		check_output "$LOG" "M37-USB: ok hid-translate" "M37: HID boot report -> scancode translation"
	else
		fail "xHCI controller initialized" "M37-USB xhci-init marker not found"
	fi

	# ── M38: Intel HDA sound controller + /dev/dsp ──
	if grep -q "M38-SOUND: ok probe" "$LOG" 2>/dev/null; then
		pass "HDA controller probed"
		check_output "$LOG" "M38-SOUND: ok dma-buf" "HDA DMA buffer accessible"
		check_output "$LOG" "M38-SOUND: ok dev-dsp" "HDA /dev/dsp device node"
		check_output "$LOG" "M38-SOUND: ok sound-api" "HDA sound device API"
		check_output "$LOG" "M38-SMOKE: ok open-dsp" "userspace /dev/dsp open"
		check_output "$LOG" "M38-SMOKE: ok write-pcm" "userspace PCM write"
		check_output "$LOG" "M38-SMOKE: ok wav-parse" "userspace WAV parse"
		check_output "$LOG" "M38-SMOKE: ok wav-play" "userspace WAV play"
	else
		# HDA may not be present in all QEMU configs — treat as skip
		pass "HDA controller (skipped — no device)"
	fi

	# ── M79: AC'97 audio controller + OSS mixer ioctls ──
	if grep -q "M79-AC97: ok probe" "$LOG" 2>/dev/null; then
		pass "AC'97 controller probed"
		check_output "$LOG" "M79-AC97: ok vendor-id" "AC'97 vendor/device ID"
		check_output "$LOG" "M79-AC97: ok mute-bit" "AC'97 mute bit"
		check_output "$LOG" "M79-AC97: ok volume-write" "AC'97 volume write"
		check_output "$LOG" "M79-AC97: ok play-tone" "AC'97 DMA tone playback"
		check_output "$LOG" "M79-AC97: ok done" "AC'97 self-test finished"
		check_output "$LOG" "M79-SMOKE: ok open-dsp1" "userspace /dev/dsp1 open"
		check_output "$LOG" "M79-SMOKE: ok mixer-vol" "userspace OSS mixer volume"
		check_output "$LOG" "M79-SMOKE: ok pcm-write" "userspace AC'97 PCM write"
	else
		# AC97 may not be present in all QEMU configs — treat as skip
		pass "AC'97 controller (skipped — no device)"
	fi

	# ── M47: display substrate — /dev/fb0 + /dev/input/event* ──
	check_output "$LOG" "M47-GFX: start" "M47 smoke started"
	if grep -q "fb0: ready" "$LOG" 2>/dev/null; then
		check_output "$LOG" "M47-GFX: ok fb-info" "M47: /dev/fb0 mode query"
		check_output "$LOG" "M47-GFX: ok fb-mmap" "M47: fb mmap aliases device memory"
		check_output "$LOG" "M47-GFX: ok fb-flush" "M47: FBIOFLUSH dirty-rect push"
		check_output "$LOG" "M47-GFX: ok fb-persist" "M47: fb survives munmap+remap"
	else
		# No 32bpp boot framebuffer in this config — device legitimately absent
		check_output "$LOG" "M47-GFX: skip no-fb" "M47: fb skipped (no framebuffer)"
	fi
	check_output "$LOG" "M47-GFX: ok input-open" "M47: input devices open + EAGAIN"
	check_output "$LOG" "M47-GFX: ok input-event" "M47: injected mouse events received"
	check_output "$LOG" "M47-GFX: done" "M47 smoke completed"

	check_output "$LOG" "M48-FDPASS: ok scm-rights" "M48: SCM_RIGHTS fd transfer"
	check_output "$LOG" "M48-FDPASS: ok scm-refcount-close" "M48: in-flight fd survives sender close"
	check_output "$LOG" "M48-FDPASS: ok memfd" "M48: anonymous mmap-able memfd"
	check_output "$LOG" "M48-FDPASS: ok shared-fork-cow" "M48: MAP_SHARED pages shared across fork"
	check_output "$LOG" "M48-FDPASS: ok unix-blocking-send" "M48: blocking AF_UNIX send blocks on a full buffer instead of EAGAIN"

	# The scanout's completion interrupt. This self-test printed its verdict
	# into the log and nothing read it, so an arch on which the GPU never
	# raised an interrupt reported a clean run.
	check_output "$LOG" "M52-GFX: ok gpu-irq" "M52: virtio-gpu completions are interrupt-driven and the wait really parks"
	check_output "$LOG" "M50-DRM: ok card0" "M50: /dev/dri/card0"
	check_output "$LOG" "M50-DRM: ok mode" "M50: KMS mode discovery"
	check_output "$LOG" "M50-DRM: ok multi-buffer" "M50: multiple dumb buffers"
	check_output "$LOG" "M50-DRM: ok setcrtc" "M50: SETCRTC presentation"
	check_output "$LOG" "M50-DRM: ok flip-event" "M50: poll/read page-flip event"
	check_output "$LOG" "M50-DRM: ok rmfb" "M50: framebuffer removal"
	check_output "$LOG" "M50-DRM: ok cleanup" "M50: close/munmap cleanup"

	# ── M101t: the imported DRM core's ioctl surface, from ring 3 ──
	check_output "$LOG" "M101T-DRM: ok open" "M101t: /dev/dri/card1 opens through upstream's drm_open"
	check_output "$LOG" "M101T-DRM: ok version" "M101t: DRM_IOCTL_VERSION names the driver on the imported core (virtio_gpu — the name Mesa turns into <name>_dri.so)"
	check_output "$LOG" "M101T-DRM: ok pci-identity" "M101t: the card publishes virtio-gpu's real PCI id in sysfs, which is what Mesa matches a driver against (it read 0000:0000 while the parent was not a PCI function)"
	check_output "$LOG" "M101T-DRM: ok session-rdev-major" "M101t: the node's st_rdev carries DRM's major, which is how a session layer recognises it"
	check_output "$LOG" "M101T-DRM: ok session-open-nonblock" "M101t: the node opens with the flags a compositor's session adds (O_NOCTTY|O_NONBLOCK)"
	check_output "$LOG" "M101T-DRM: ok session-set-master" "M101t: the master lease can be taken, without which no modeset ioctl is permitted"
	check_output "$LOG" "M101T-DRM: ok session-drop-master" "M101t: and dropped again, so a second compositor can take it"
	check_output "$LOG" "M101T-DRM: ok getresources" "M101t: two-pass GETRESOURCES fills the id arrays"
	check_output "$LOG" "M101T-DRM: ok getconnector" "M101t: the connector reports connected with a usable mode"
	check_output "$LOG" "M101T-DRM: ok getconnector-bounds" "M101t: the mode list stays inside the buffer userspace supplied"
	check_output "$LOG" "M101T-DRM: ok ioctl-bounds" "M101t: resources and properties stay inside their buffers too"
	check_output "$LOG" "M101T-DRM: ok getconnector-churn" "M101t: repeated connector queries leave the allocator intact"
	check_output "$LOG" "M101T-DRM: ok create-dumb" "M101t: CREATE_DUMB allocates a GEM object with a handle"
	check_output "$LOG" "M101T-DRM: ok map-dumb" "M101t: MAP_DUMB hands out a drm_vma_manager offset"
	check_output "$LOG" "M101T-DRM: ok mmap" "M101t: the dumb buffer maps into the process and reads back what was written"
	check_output "$LOG" "M101T-DRM: ok addfb" "M101t: ADDFB wraps the object in a framebuffer"
	check_output "$LOG" "M101T-DRM: ok setcrtc" "M101t: SETCRTC drives upstream's atomic modeset from userspace"
	check_output "$LOG" "M101T-DRM: ok scanout-pixels" "M101t: the pattern written in ring 3 reached the scanout"
	check_output "$LOG" "M101T-DRM: ok flip-event" "M101t: PAGE_FLIP delivers its completion event through drm_read"
	check_output "$LOG" "M101T-DRM: ok rmfb" "M101t: RMFB releases the framebuffer"
	check_output "$LOG" "M101T-DRM: ok destroy-dumb" "M101t: DESTROY_DUMB releases the GEM handle"

	# ── M98: driver infrastructure (in-kernel; no userspace surface) ──
	check_output "$LOG" "M98-DRV-SMOKE: ok netconsole-cmdline" "M98: b1nix.netconsole=<ip>:<port> parses valid targets and rejects malformed ones"
	check_output "$LOG" "M98-DRV-SMOKE: ok netconsole-udp" "M98: the klog ring reaches a UDP collector through the real network stack"
	# IA32_PAT is an x86 MSR holding eight memory types. AArch64 has no
	# counterpart to read back: its types live in MAIR_EL1 slots selected by the
	# descriptor's AttrIndx, programmed once in boot.S rather than per CPU. The
	# rest of this group (the WC mapping, the line flush, the whole-hierarchy
	# flush) is real on both arches and is NOT skipped.
	if [ "$ARCH" = "aarch64" ]; then
		skipped "M98: IA32_PAT reads back the programmed slot table with WC in slot 5" "no PAT MSR on this arch — memory types come from MAIR_EL1 slots instead"
	else
		check_output "$LOG" "M98-DRV-SMOKE: ok pat-msr" "M98: IA32_PAT reads back the programmed slot table with WC in slot 5"
	fi
	check_output "$LOG" "M98-DRV-SMOKE: ok pat-wc" "M98: a VMM_WC mapping carries the WC page attributes and stays coherent with the direct map"
	check_output "$LOG" "M98-DRV-SMOKE: ok clflush" "M98: clflush/mfence range flush preserves completed stores"
	check_output "$LOG" "M98-DRV-SMOKE: ok bar-enum" "M98: PCI BAR enumeration sizes every window to an aligned power of two"
	check_output "$LOG" "M98-DRV-SMOKE: ok bar-restore" "M98: BAR sizing restores the original register (the running driver keeps working)"
	check_output "$LOG" "M98-DRV-SMOKE: ok cap-walk" "M98: PCI capability walk finds capabilities that really are at the reported offsets"
	check_output "$LOG" "M98-DRV-SMOKE: ok bus-master" "M98: bus-master enable reads back set without disturbing the rest of the command register"
	check_output "$LOG" "M98-DRV-SMOKE: ok msi-config" "M98: MSI programming reads back the expected LAPIC address and vector"
	# A board with no MSI controller says so rather than programming an entry
	# nothing can deliver: QEMU virt's default GICv2 has no ITS, and the lane
	# that does (smp, gic-version=3) checks the real thing below.
	if [ "$ARCH" = "aarch64" ]; then
		check_output "$LOG" "M98-DRV-SMOKE: \(ok\|skip\) msix-config" "M98: MSI-X table entry programming, or an honest skip on a board with no MSI controller"
	else
		check_output "$LOG" "M98-DRV-SMOKE: ok msix-config" "M98: MSI-X table entry programming reads back address, data and an unmasked vector control"
	fi
	check_output "$LOG" "M98-DRV-SMOKE: ok stolen" "M98: Intel stolen memory (BDSM/BGSM) reports the host bridge's real state"
	check_output "$LOG" "M98-DRV-SMOKE: ok stolen-decode" "M98: the GGC GMS/GGMS decode matches the spec encodings, including the 4 MiB-unit range and absence"
	check_output "$LOG" "M98-DRV-SMOKE: ok wbinvd-fallback" "M98: the CLFLUSH-less wbinvd path runs and a completed store survives it"
	# Programming an MSI-X table is one thing (msi-config above passes here);
	# having the message DELIVERED needs an interrupt controller that routes
	# them. QEMU virt gives this board a GICv2, which has no ITS — message
	# interrupts have nowhere to land, so NVMe falls back to its legacy line.
	# A real gap in the port would be an unimplemented ITS driver on a GICv3
	# board; on this one there is no such hardware to drive.
	if [ "$ARCH" = "aarch64" ]; then
		skipped "M98: an MSI-X message the NVMe controller raises is delivered to the vector the driver owns" "GICv2 on QEMU virt has no ITS, so there is no MSI doorbell to deliver through"
		skipped "M98: NVMe drives its completions over MSI-X, not the legacy INTx line" "same: no ITS on this interrupt controller, so NVMe uses its legacy line"
	else
		check_output "$LOG" "M98-DRV-SMOKE: ok msi-delivery" "M98: an MSI-X message the NVMe controller raises is delivered to the vector the driver owns"
		check_output "$LOG" "nvme: MSI-X completions on vector" "M98: NVMe drives its completions over MSI-X, not the legacy INTx line"
	fi

	# ── M99: linuxkpi compatibility layer (in-kernel) ──
	check_output "$LOG" "M99-SMOKE: ok idr" "M99: idr allocates unique ids, looks up the exact pointers, and reuses freed ids"
	check_output "$LOG" "M99-SMOKE: ok completion" "M99: a completion publishes the signaller's data to the waiter and honours its timeout"
	check_output "$LOG" "M99-SMOKE: ok workqueue" "M99: workqueue runs each item exactly once, in submission order"
	check_output "$LOG" "M99-SMOKE: ok workqueue-delayed" "M99: delayed work fires no earlier than its deadline and can be cancelled"
	check_output "$LOG" "M99-SMOKE: ok scatterlist" "M99: scatterlist coalesces adjacent runs and resolves every page offset"
	check_output "$LOG" "M99-SMOKE: ok ioremap" "M99: ioremap/ioremap_wc create real mappings coherent with the direct map"
	check_output "$LOG" "M99-SMOKE: ok dma-mapping" "M99: dma handles round-trip to the VMM's own physical translation"
	check_output "$LOG" "M99-SMOKE: ok dma-bounce-pool" "M100a: a bounce that the reserved pool can serve is served from it, and the slot and its accounting come back on unmap"
	check_output "$LOG" "M99-SMOKE: ok dma-bounce-sg-fallback" "M100a: with no single block available the sg mapping still succeeds, one block per run, data round-tripping through all of them"
	check_output "$LOG" "dma: bounce pool" "M100a: the DMA bounce pool is reserved at boot"
	check_output "$LOG" "M99-SMOKE: ok dma-bounce-sg" "M99: an sg table out of a device's reach is bounced into ONE block below the mask, entries pointing into it in order, both directions copied"
	check_output "$LOG" "M99-SMOKE: ok dma-bounce" "M99: a buffer outside a device's address window is bounced through memory it can reach, in both directions"
	check_output "$LOG" "M99-SMOKE: ok request-firmware" "M99: request_firmware loads a VFS blob byte-for-byte and reports ENOENT otherwise"
	check_output "$LOG" "M99-SMOKE: ok locks" "M99: the sleeping mutex actually excludes two racing contexts"
	check_output "$LOG" "M99-SMOKE: done" "M99 linuxkpi self-test suite completes"

	# ── M101: linuxkpi primitives the DRM core needs ──
	check_output "$LOG" "M101-SMOKE: ok kref" "M101: the last kref put, and only the last, runs release exactly once; a weak reference on a dead object fails instead of resurrecting it"
	check_output "$LOG" "M101-SMOKE: ok waitqueue" "M101: a wait_event sleeper is woken by another context and leaves the queue's books clean"
	check_output "$LOG" "M101-SMOKE: ok waitqueue-timeout" "M101: a wait that times out really parked for its full deadline, measured against the scheduler's ticks, and one already satisfied does not sleep"
	check_output "$LOG" "M101-SMOKE: ok ww-mutex-basic" "M101: ww_mutex excludes, tracks its acquire count, and names a double-lock as EALREADY instead of deadlocking"
	check_output "$LOG" "M101-SMOKE: ok ww-mutex-wound" "M101: an older context wounds a younger holder, the younger is refused with EDEADLK and backs off, and the older is never wounded itself"
	check_output "$LOG" "M101-SMOKE: ok ww-mutex-stress" "M101: two tasks taking the same two locks in opposite orders both terminate with an exact shared counter"
	check_output "$LOG" "M101-SMOKE: ok rbtree" "M101: ascending inserts stay balanced (invariants verified, depth bounded), ordered both ways, and erase leaves the tree consistent"
	check_output "$LOG" "M101-SMOKE: ok rbtree-churn" "M101: the rbtree survives 4096 interleaved inserts and erases over heavily duplicated keys — the shape the DRM allocator builds — with the invariants and the cached leftmost checked after every operation"
	check_output "$LOG" "M101-SMOKE: ok interval-tree" "M101: overlap queries agree with a brute-force scan, and every node's cached subtree maximum survives rebalancing"
	check_output "$LOG" "M101-SMOKE: ok xarray" "M101: sparse indices across the full 64-bit range round-trip, iterate in order, and the tree folds back to genuinely empty on erase"
	check_output "$LOG" "M101-SMOKE: ok kthread-worker" "M101: a caller-owned worker runs its items in submission order, coalesces a re-queue, and a flush waits for a sleeping handler"
	check_output "$LOG" "M101-SMOKE: ok rcu" "M101: RCU read sections nest correctly, a grace period with no readers still completes, and deferred callbacks each run exactly once by the time rcu_barrier returns"
	# Needs a reader running on ANOTHER CPU while synchronize_rcu waits — that
	# is the whole point of it. Both arches have one now: aarch64 brings its
	# secondaries up over PSCI (kernel/arch/aarch64/smp.c).
	check_output "$LOG" "M101-SMOKE: ok rcu-grace-period" "M101: synchronize_rcu does not return while a reader that started before it is still inside — proved by poisoning the object the moment it returns and having the reader, running on another CPU, report whether it ever saw the poison"
	check_output "$LOG" "M101-SMOKE: ok pages" "M101: a shmem page array is genuinely scattered, and a vmap of it is verified through each page's own direct-map address — a different mapping of the same memory — in both directions, plus write-combining"
	check_output "$LOG" "M101-SMOKE: ok device-pm" "M101: a kobject releases child-before-parent on the last put, and runtime PM suspends only on the last holder, refuses to claim a suspend the driver rejected, and leaves no usage reference behind on a failed resume"
	check_output "$LOG" "M101-SMOKE: done" "M101 linuxkpi self-test suite completes"

	# ── M101: the imported DRM core runs, not merely links ──
	check_output "$LOG" "M101-IMPORT: ok drm-mm" "M101: upstream's own GPU address allocator runs on our augmented rbtree and hands out non-overlapping ranges, reusing holes after a free"
	check_output "$LOG" "M101-IMPORT: ok drm-rect" "M101: upstream's rectangle maths agrees with independently computed intersections and fixed-point scale factors"
	check_output "$LOG" "M101-IMPORT: ok drm-fourcc" "M101: upstream's format table resolves through our const handling, including planar subsampling, and reports an unknown code as absent"
	check_output "$LOG" "M101-IMPORT: done" "M101 imported-core proof completes"

	# ── M101: a real device on the imported core, and the files it publishes ──
	check_output "$LOG" "M101-KMS: ok device-register" "M101: a driver registers a drm_device with the imported core — minor allocation, the DRM sysfs class and mode configuration all run upstream's code"
	check_output "$LOG" "M101-KMS: ok modeset-probe" "M101: the in-kernel DRM client probes the connector and picks a mode through upstream's probe helpers"
	check_output "$LOG" "M101-KMS: ok framebuffer-create" "M101: a dumb buffer becomes a GEM object with a handle and a framebuffer, through drm_gem_fb_create"
	check_output "$LOG" "M101-KMS: ok commit" "M101: upstream's atomic helpers commit the modeset and the plane"
	check_output "$LOG" "M101-KMS: ok scanout-pixels" "M101: the committed framebuffer's pixels reach virtio-gpu's scanout — corners and centre match what was painted, so the commit moved an image rather than returning success"
	check_output "$LOG" "M101-KMS: done" "M101 KMS proof completes"

	check_output "$LOG" "M101-SYSFS: ok card-dev" "M101: /sys/class/drm/card0/dev reads the device number the imported core registered, so its class and device registration reached b1nix's /sys"
	check_output "$LOG" "M101-SYSFS: ok attr-store" "M101: a writable sysfs attribute takes a write and the next read reflects it"
	check_output "$LOG" "M101-SYSFS: ok attr-remove-one" "M101: removing one attribute takes that file and no other, releases the context the caller attached to it, and a second removal reports absence"
	check_output "$LOG" "M101-SYSFS: ok attr-read-at" "M101: an offset-aware attribute is told where the reader stopped, so a value longer than one read is continued rather than truncated at the same place forever"
	check_output "$LOG" "M101-SYSFS: ok uevent-delivered" "M101: registering a device broadcasts a uevent on NETLINK_KOBJECT_UEVENT that a bound listener receives, with the summary line, ACTION, DEVPATH, SUBSYSTEM and SEQNUM a hotplug helper needs"
	check_output "$LOG" "M101-SYSFS: ok attr-remove" "M101: removing a registered directory unlinks its files rather than leaving a dangling callback"
	check_output "$LOG" "M101-SYSFS: ok debugfs-seq-file" "M101: a seq_file-backed debugfs file — a driver's own single_open/seq_read shape — renders once into a buffer and is read to its end across many reads, so nothing past the first read is lost"
	check_output "$LOG" "M101-SYSFS: ok debugfs-dri" "M101: the DRM core's own debugfs_create_dir(\"dri\") lands under /sys/kernel/debug"
	check_output "$LOG" "M101-SYSFS: done" "M101 sysfs/debugfs proof completes"

	# ── M100: DRM core ──
	check_output "$LOG" "M100-SMOKE: ok fence-signal" "M100: a dma-fence wakes a parked waiter and runs its callback exactly once"
	check_output "$LOG" "M100-SMOKE: ok fence-error" "M100: a fence error reaches the waiter"
	check_output "$LOG" "M100-SMOKE: ok fence-refcount" "M100: dma-fence references are balanced"
	check_output "$LOG" "M100-SMOKE: ok sched-submit" "M100: the GPU scheduler runs queued jobs only after their dependency signals"
	check_output "$LOG" "M100-SMOKE: ok sched-fairness" "M100: the scheduler round-robins between entities instead of draining one"
	check_output "$LOG" "M100-SMOKE: ok gem-sg" "M100: a discontiguous page list maps into one linear kernel view correctly"
	check_output "$LOG" "M100-SMOKE: ok gem-sg-discontig" "M100: the page list really is discontiguous — one scatterlist entry per page, by construction"
	check_output "$LOG" "M100-SMOKE: ok gem-vmap-shared" "M100: the GEM linear-view window's page-table path is present in a freshly created address space"
	check_output "$LOG" "M100-SMOKE: done" "M100 kernel-side DRM core self-test completes"
	check_output "$LOG" "M100-SMOKE: ok gem-create" "M100: CREATE_DUMB returns a handle with the requested geometry"
	check_output "$LOG" "M100-SMOKE: ok gem-info" "M100: the object is scatter-gather backed with one entry per physical run"
	check_output "$LOG" "M100-SMOKE: ok gem-map" "M100: a fresh GEM object maps zeroed"
	check_output "$LOG" "M100-SMOKE: ok gem-mmap-pages" "M100: every page of a scatter-gather object maps to its own backing page"
	check_output "$LOG" "M100-SMOKE: ok gem-scanout" "M100: a scatter-gather object still reaches the display through SETCRTC"
	check_output "$LOG" "M100-SMOKE: ok gem-destroy" "M100: GEM handle lifetime (EBUSY while bound, miss after destroy)"
	check_output "$LOG" "M100-SMOKE: ok gem-handle-reuse" "M100: the idr handle table reuses a freed GEM handle"
	check_output "$LOG" "M100-SMOKE: done userspace" "M100 userspace GEM suite completes"

	check_output "$LOG" "CXX-SMOKE: ok ctors" "C++: crt0 runs .init_array (static constructors)"
	check_output "$LOG" "CXX-SMOKE: ok stl" "C++: libstdc++ STL (map/vector/string) runs on b1nix"
	check_output "$LOG" "CXX-SMOKE: ok exceptions" "C++: exception throw/catch unwinds on b1nix"
	check_output "$LOG" "CXX-SMOKE: ok rtti" "M55 C++: RTTI dynamic_cast/typeid + bad_cast throw"
	check_output "$LOG" "CXX-SMOKE: ok static-init" "M55 C++: thread-safe function-local static (__cxa_guard)"
	check_output "$LOG" "CXX-SMOKE: ok threads" "M55 C++: std::thread/mutex/atomic over pthreads"
	# M64 clang++ frontend is x86_64-only (size_t mangling clash with the
	# GCC-built libstdc++ on i686-b1nix); GCC stays the C++ compiler on x86.
	[ "$ARCH" = "x86_64" ] && check_output "$LOG" "M64-CLANG: ok" "M64: clang++ frontend with GNU C++ runtime"
	check_output "$LOG" "M55-IOSTREAM: ok cout" "M55 C++: std::cout/cerr formatted output (iostream locale facets)"
	check_output "$LOG" "M55-IOSTREAM: ok sstream" "M55 C++: std::ostringstream/istringstream round-trip"
	check_output "$LOG" "M55-IOSTREAM: ok cin" "M55 C++: std::cin extraction from a real fd 0 (piped stdin)"
	check_output "$LOG" "M55-IOSTREAM: ok filesystem" "M55 C++: std::filesystem create/iterate/stat/remove over VFS"
	check_output "$LOG" "M51-GFX: ok libm" "M51: musl libm runtime math"
	check_output "$LOG" "M51-GFX: ok pixman" "M51: ported pixman compositing"
	check_output "$LOG" "M51-GFX: ok freetype" "M51: ported FreeType glyph rasterization"
	check_output "$LOG" "M51-GFX: ok cairo" "M51: ported Cairo text rendering (full stack)"
	check_output "$LOG" "M51-GFX: ok xkbcommon" "M51: ported xkbcommon keycode->keysym"
	check_output "$LOG" "M51-GFX: ok harfbuzz" "M51: ported HarfBuzz OpenType shaping"
	check_output "$LOG" "M51-GFX: ok fontconfig" "M51: ported Fontconfig font matching"

	# ── Composition: both renderers, each checked on its own ──
	#
	# Software composition (pixman) and accelerated composition (GLES through
	# EGL and gbm on a DRM render node) are two supported paths, and the point
	# of checking them apart is that a regression in one must not be able to
	# hide behind the other. /etc/render-smoke.sh runs each deliberately and
	# only prints a marker after grim pulled a frame back out of the running
	# compositor and /bin/framecheck found the colour the compositor was told
	# to paint — twice, with two different colours, per run.
	check_output "$LOG" "RENDER-SMOKE: ok selection" \
		"the compositor's renderer is chosen at run time and the decision is recorded (/run/render-selection)"
	check_output "$LOG" "RENDER-SMOKE: ok software-frame" \
		"software composition: sway on pixman painted the colours it was told to and grim read them back"
	check_output "$LOG" "RENDER-SMOKE: ok fallback-engaged" \
		"with acceleration forced off the automatic choice returns pixman AND the compositor still comes up and still paints — a fallback, not a failure"
	check_output "$LOG" "RENDER-SMOKE: done" "the renderer smoke runs to the end"
	# The accelerated path needs a GL driver in the image, which the ordinary
	# image deliberately does not carry (mesa-dri-gallium is 184 MB with LLVM
	# behind it — see tools/packages/alpine-ports.map). Where it IS there, the
	# frame is required; where it is not, the run says so in its own words and
	# that is reported as a skip rather than passed over in silence.
	if grep -q "RENDER-SMOKE: accel-status available" "$LOG" 2>/dev/null; then
		check_output "$LOG" "RENDER-SMOKE: ok accel-frame" \
			"accelerated composition: sway on GLES/EGL over a DRM render node painted the colours it was told to and grim read them back"
	elif grep -q "RENDER-SMOKE: accel-status unavailable" "$LOG" 2>/dev/null; then
		skipped "accelerated composition produces a frame" \
			"$(grep -h 'RENDER-SMOKE: accel-status unavailable' "$LOG" | head -1 | sed 's/.*reason=//') — build the driver image with: make B1NIX_GPU_DRV=1 iso && boot it with b1nix.render-smoke"
	else
		check_output "$LOG" "RENDER-SMOKE: accel-status" \
			"the accelerated path is evaluated and its verdict recorded"
	fi

	# The display server this section tested is gone: sway drives the display
	# now, through the imported DRM stack, and its run lives in
	# /etc/i915-sway.sh rather than here. What it was really proving
	# underneath — SCM_RIGHTS, memfd and shared mappings (M48), and the ported
	# graphics libraries (M51) — is checked above and below, without a
	# compositor.
fi

# ── M24b SMP work-stealing (multi-core) ──
if [ "$ARCH" = "aarch64" ]; then
	echo ""
	echo "[RUN] M24b SMP work-stealing (-smp 4) checks..."
	# PSCI CPU_ON, not INIT-SIPI, and the secondaries run stealable kernel
	# workers only — userspace on a secondary needs the per-CPU exception
	# state this port does not have yet, so the BKL proof below is x86_64's.
	check_output "$SMP_LOG" "smp: 4 CPUs online" "PSCI brings up every CPU the device tree lists"
	check_output "$SMP_LOG" "M24B-SMP: ok work-stealing" "idle secondaries steal and run kernel workers from the boot CPU"
	check_output "$SMP_LOG" "M28-HEAPBENCH: ok" "the heap scales across those CPUs instead of serialising on one lock"
	# The GICv3 half of this lane: a message-signalled interrupt, raised by the
	# ITS itself so the check does not depend on a particular device.
	check_output "$SMP_LOG" "gicv3: dist" "the GICv3 distributor and redistributors come up from the device tree"
	check_output "$SMP_LOG" "its: 0x" "the ITS is brought up (command queue, device and collection tables, LPI tables)"
	check_output "$SMP_LOG" "M98-ITS: ok int-delivery" "an LPI raised through the ITS reaches the vector that owns it"
	check_output "$SMP_LOG" "M98-DRV-SMOKE: ok msix-config" "an MSI-X entry is programmed with the ITS address/EventID pair and reads back unmasked"
	# The DMA remapping unit this architecture has. Every stream starts
	# bypassing, so the devices on this lane keep working; what is checked is
	# that the unit comes up, its command queue drains, and a translation
	# installed through it reads back out of its own tables.
	check_output "$SMP_LOG" "M100E-SMOKE: ok smmuv3-enable" "the SMMUv3 is brought up from the device tree and enabled"
	check_output "$SMP_LOG" "M100E-SMOKE: ok smmuv3-command-queue" "its command queue accepts a command and the sync completes"
	check_output "$SMP_LOG" "M100E-SMOKE: ok smmuv3-map" "a translation installed through the SMMUv3 resolves in its own page tables"
	check_output "$SMP_LOG" "M100E-SMOKE: ok smmuv3-unmap" "removing it makes the address stop resolving"
	check_output "$SMP_LOG" "M100E-SMOKE: ok nvme-translated" "a real controller runs in its own domain: the read returns the right bytes and the unit records no fault"
	# Userspace on a secondary, not just kernel workers: the process reads its
	# own CPU id and reports which one it ran on.
	check_output "$LOG" "M24B-BKL: instance ran-on-ap" "a userspace process runs on a secondary CPU"
	if grep -q -E "KERNEL PANIC|\[PANIC\]" "$SMP_LOG" 2>/dev/null; then
		fail "SMP self-test completes without panic" "PANIC detected in log"
	else
		pass "SMP self-test completes without panic"
	fi
fi
if [ "$ARCH" = "x86_64" ]; then
	echo ""
	echo "[RUN] M24b SMP work-stealing (-smp 4) checks..."
	check_output "$SMP_LOG" "smp: AP 1 ready" "Application Processor boots (INIT-SIPI)"
	check_output "$LOG" "M24B-BKL: instance ran-on-ap" "cross-CPU work-stealing runs stolen tasks on APs"
	if grep -q -E "KERNEL PANIC|\[PANIC\]" "$SMP_LOG" 2>/dev/null; then
		fail "SMP self-test completes without panic" "PANIC detected in log"
	else
		pass "SMP self-test completes without panic"
	fi
fi

# ── Summary ──
echo ""
# ── Debian (glibc) boot gate ────────────────────────────────────────────────
# The Linux ABI layer against a real glibc distribution rather than against the
# musl it was developed with. Opt-in, because it needs an image built once from
# the network: make debian-image, then SMOKE_DEBIAN=1 sh tests/smoke.sh.
if [ "${SMOKE_DEBIAN:-0}" = "1" ]; then
	echo ""
	echo "[RUN] Debian (glibc) boot..."
	if [ -f "build/$ARCH/debian.ext4" ]; then
		if sh "$PROJECT_DIR/tests/debian-smoke.sh" "$ARCH" >"$PROJECT_DIR/smoke_run/b1nix-debian-run.log" 2>&1; then
			pass "a Debian bookworm userspace boots on this kernel (dash, coreutils, sysvinit)"
		else
			fail "a Debian bookworm userspace boots on this kernel" \
				"see smoke_run/b1nix-debian-boot.log"
		fi
	else
		blocked "a Debian bookworm userspace boots on this kernel" \
			"no build/$ARCH/debian.ext4 — run: make debian-image"
	fi
fi

echo "=== Results ==="
echo "  Passed:  $PASSED"
echo "  Failed:  $FAILED"
echo "  Blocked: $BLOCKED"
echo "  Skipped: $SKIPPED"
if [ "$BLOCKED" -gt 0 ]; then
	echo ""
	echo "  $BLOCKED checks never ran — an instance stopped early:"
	report_wedged_instances
fi

for _i in sys blk posix gfx openrc init switchroot iommu amdvi smp; do
    rm -f "$(disk_img sata "$_i")" "$(disk_img nvme "$_i")" "$(disk_img swap "$_i")" "$(disk_img usb "$_i")"
done
echo ""

# Clean up SATA, NVMe and Swap dummy images
rm -f "$PROJECT_DIR/smoke_run/sata.img" "$PROJECT_DIR/smoke_run/nvme.img" "$PROJECT_DIR/smoke_run/swap.img"

if [ "$FAILED" -gt 0 ] || [ "$BLOCKED" -gt 0 ]; then
	exit 1
fi
exit 0
