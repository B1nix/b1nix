#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Debian (glibc) boot test: boot b1nix with a real debian:bookworm root
# filesystem attached as a virtio-blk disk and let the distro's own binaries
# exercise the Linux-ABI layer.
#
#   sh tools/image/mk-debian-image.sh     # once, builds build/$ARCH/debian.ext4
#   sh tests/debian-smoke.sh [x86_64]
#
# Skips cleanly (exit 0) when the image has not been built, so it can be wired
# into CI on a host with no network.
set -e

ARCH="${1:-x86_64}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build/$ARCH}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PROJECT_DIR/$BUILD_DIR" ;; esac

IMG="${DEBIAN_IMG:-$BUILD_DIR/debian.ext4}"
# The image has been built under two names; take whichever exists so the lane
# runs instead of skipping. The label travels with the image -- the kernel
# finds its root by label, and the systemd image carries its own -- so read it
# from the filesystem rather than assuming.
[ -f "$IMG" ] || IMG="$BUILD_DIR/debian-systemd.ext4"
# The systemd image boots systemd, whose units are the systemd lane's own; the
# harness these checks look for is never started from it, and every probe then
# reports FAIL for a reason that has nothing to do with the kernel. Run the
# harness as PID 1 there instead -- it is injected into the scratch copy below,
# so it works on whichever image is present.
case "$IMG" in
*debian-systemd.ext4) DEBIAN_INIT="${DEBIAN_INIT:-/b1nix-stage.sh}" ;;
esac
IMG_LABEL="${IMG_LABEL:-}"
if [ -z "$IMG_LABEL" ] && command -v dumpe2fs >/dev/null 2>&1 && [ -f "$IMG" ]; then
	IMG_LABEL=$(dumpe2fs -h "$IMG" 2>/dev/null |
		sed -n 's/^Filesystem volume name:[[:space:]]*//p')
fi
[ -n "$IMG_LABEL" ] || IMG_LABEL="b1nix-debian"
LOG="$PROJECT_DIR/smoke_run/b1nix-debian-boot.log"
BUILD_LOG="$PROJECT_DIR/smoke_run/b1nix-debian-build.log"
ISO="$BUILD_DIR/${B1NIX_ISO_NAME:-b1nix-debian.iso}"
TIMEOUT="${TIMEOUT:-240}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0

# Where the minutes go: every host-side step and every boot's milestones are
# printed as "[TIME] <what> <seconds>", so a slow lane is measured, not guessed.
now() { date +%s.%N; }
since() { awk -v a="$1" -v b="$(now)" 'BEGIN { printf "%.1f", b - a }'; }
LANE_T0=$(now)

pass() {
	printf "  ${GREEN}PASS${NC} %s\n" "$1"
	PASSED=$((PASSED + 1))
}
fail() {
	printf "  ${RED}FAIL${NC} %s - %s\n" "$1" "$2"
	FAILED=$((FAILED + 1))
}

mkdir -p "$PROJECT_DIR/smoke_run"

echo "=== B1NIX Debian (glibc) Boot Test ($ARCH) ==="

if [ ! -f "$IMG" ]; then
	printf "  ${YELLOW}skipped${NC}: %s not built — run tools/image/mk-debian-image.sh first\n" "$IMG"
	exit 0
fi

if [ "$(uname -s)" = "Darwin" ]; then
	NPROC=$(sysctl -n hw.ncpu)
else
	NPROC=$(nproc)
fi

# ── Kernel command line ────────────────────────────────────────────────────
# The Debian filesystem is a DISK, not the Multiboot rootfs module: the kernel
# picks it as root from its ext4 label, then runs our harness as PID 1.
# Debian's own sysvinit is PID 1; it runs our harness from /etc/inittab.
# DEBIAN_INIT=/b1nix-stage.sh runs the harness directly as PID 1 instead.
CMDLINE="root=LABEL=$IMG_LABEL init=${DEBIAN_INIT:-/sbin/init} ${DEBIAN_EXTRA_CMDLINE:-}"

# ── Build ──────────────────────────────────────────────────────────────────
# The kernel is built once. Each boot below needs its own command line, and on
# this boot path the command line lives in the ISO's boot-loader config, so each
# boot gets an ISO of its own from mkiso.sh -- a second, not a `make iso`.
KERNEL_ELF="$BUILD_DIR/kernel.elf"
if [ "${SKIP_BUILD:-0}" = "1" ]; then
	[ -f "$KERNEL_ELF" ] || { printf "  ${RED}no prebuilt %s${NC}\n" "$KERNEL_ELF"; exit 1; }
	echo "  (SKIP_BUILD=1 — reusing $KERNEL_ELF)"
else
	echo "[BUILD] Building kernel ISO for the Debian boot..."
	_t=$(now)
	if ! (cd "$PROJECT_DIR" && make -j"$NPROC" ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} \
		ISO_NO_ROOT_MODULE="${ISO_NO_ROOT_MODULE:-1}" \
		KERNEL_CMDLINE="$CMDLINE" iso) >"$BUILD_LOG" 2>&1; then
		printf "  ${RED}BUILD FAILED${NC} (log: %s)\n" "$BUILD_LOG"
		tail -60 "$BUILD_LOG"
		exit 1
	fi
	# `iso` always writes b1nix.iso; keep a stable copy under the lane's name.
	cp "$BUILD_DIR/b1nix.iso" "$ISO"
	echo "[TIME] build $(since "$_t")s"
	pass "kernel builds without errors"
fi

# ── Per-run scratch copy of the image ──────────────────────────────────────
# The pristine image is never written by a test run. One scratch copy carries
# the current harness, and every boot runs on a copy-on-write overlay of it, so
# the boots are independent of each other and none of them copies the image.
RUN_DIR="$PROJECT_DIR/smoke_run/debian-run-$$"
RUN_IMG="$RUN_DIR/root.img"
mkdir -p "$RUN_DIR"
# Put the current harness into the copy.
#
# The script inside the image is whatever the image was built with, which may
# be months old; debugfs writes the working-tree version into the scratch copy
# without root and without rebuilding the image, so the test always runs the
# harness that sits beside it in the repository.
#
# The injected image is kept beside the pristine one and reused while neither
# the image nor the harness changes: two debugfs writes into an 800 MB image
# measured 5 to 73 s from run to run (the disk they go through is shared with
# builds), against 0.1 s for a reflink copy of an image already injected.
STAGE="$PROJECT_DIR/tools/image/debian-stage.sh"
INJECTED="${IMG%.ext4}-injected.ext4"
INJECT_KEY=""
if [ -f "$STAGE" ] && command -v debugfs >/dev/null 2>&1; then
	INJECT_KEY="$(stat -c '%s %Y' "$IMG") $(sha256sum "$STAGE" | cut -c1-16)"
fi
_t=$(now)
if [ -n "$INJECT_KEY" ] && [ -f "$INJECTED" ] &&
	[ "$(cat "$INJECTED.key" 2>/dev/null)" = "$INJECT_KEY" ]; then
	cp --reflink=auto "$INJECTED" "$RUN_IMG" 2>/dev/null || cp "$INJECTED" "$RUN_IMG"
	echo "  (harness from the cached injected image)"
	echo "[TIME] image-copy $(since "$_t")s"
else
	cp --reflink=auto "$IMG" "$RUN_IMG" 2>/dev/null || cp "$IMG" "$RUN_IMG"
	echo "[TIME] image-copy $(since "$_t")s"
	if [ -n "$INJECT_KEY" ]; then
		_t=$(now)
		if debugfs -w -R "rm /b1nix-stage.sh" "$RUN_IMG" >/dev/null 2>&1 &&
			debugfs -w -R "write $STAGE b1nix-stage.sh" "$RUN_IMG" >/dev/null 2>&1; then
			echo "  (harness injected from tools/image/debian-stage.sh)"
			echo "[TIME] harness-inject $(since "$_t")s"
			rm -f "$INJECTED" "$INJECTED.key"
			if cp --reflink=auto "$RUN_IMG" "$INJECTED" 2>/dev/null; then
				echo "$INJECT_KEY" >"$INJECTED.key"
			else
				rm -f "$INJECTED"
			fi
		else
			printf "  ${YELLOW}note${NC}: could not inject the harness; using the one in the image\n"
		fi
	fi
fi
cleanup() {
	for _pf in "$RUN_DIR"/*.pid; do
		[ -f "$_pf" ] && kill -9 "$(cat "$_pf")" 2>/dev/null
	done
	rm -rf "$RUN_DIR"
}
trap cleanup EXIT
# A signal must end the run too: a trap that only cleans up leaves the loop
# polling on, booting the next part.
trap 'exit 130' INT TERM

# ── Run ────────────────────────────────────────────────────────────────────
ACCEL_ARGS=""
# DEBIAN_ACCEL=tcg forces the emulator: slower, but QEMU's -d int can then log
# the exceptions that end in a silent triple-fault reset, which it cannot see
# under KVM.
if [ "${DEBIAN_ACCEL:-}" = "tcg" ]; then
	ACCEL_ARGS="-accel tcg"
elif [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
	ACCEL_ARGS="-accel kvm -cpu host,+invtsc"
elif [ "$(uname)" = "Darwin" ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw hvf; then
	ACCEL_ARGS="-accel hvf -cpu host"
fi

# The boots. "base" runs everything but liburing; pN runs the Nth slice of
# liburing's suite, which is split across boots because two hundred programs do
# not fit in one boot's deadline, and a run that is cut off reports every test
# it never reached as a failure -- thirty of them, none of them about the kernel.
# LIBURING_PARTS=0 runs the whole harness, suite included, in one boot.
LIBURING_PARTS="${LIBURING_PARTS:-12}"
LIBURING_TIMEOUT="${LIBURING_TIMEOUT:-20}"
# DEBIAN_BOOTS="base 3 5" runs only those boots ("base" is the one without
# liburing, numbers are liburing parts) -- for working on one of them. The
# checks below then report what the skipped boots would have printed as FAIL.
DEBIAN_BOOTS="${DEBIAN_BOOTS:-}"
want_boot() {
	[ -z "$DEBIAN_BOOTS" ] && return 0
	case " $DEBIAN_BOOTS " in *" $1 "*) return 0 ;; esac
	return 1
}
BOOTS=""
if [ "$LIBURING_PARTS" -gt 0 ]; then
	want_boot base && BOOTS="base"
	_p=1
	while [ "$_p" -le "$LIBURING_PARTS" ]; do
		want_boot "$_p" && BOOTS="$BOOTS p$_p"
		_p=$((_p + 1))
	done
else
	BOOTS="all"
fi
boot_extra() {
	case "$1" in
	base) echo "b1nix.liburing=__none__" ;;
	all) echo "" ;;
	p*) echo "b1nix.liburing-part=${1#p}/$LIBURING_PARTS b1nix.liburing-timeout=$LIBURING_TIMEOUT" ;;
	esac
}
# A part carrying thirty-odd programs that may each burn their kill timeout
# needs a longer deadline than the plain boot does.
boot_deadline() {
	case "$1" in
	base) echo "$TIMEOUT" ;;
	*) echo "${LIBURING_PART_TIMEOUT:-600}" ;;
	esac
}

# Boots run side by side, DEBIAN_JOBS at a time. The guests spend most of
# their time waiting -- on a test's kill timeout, on a sleep inside a test -- so
# one and a half guest CPUs per host CPU cost nothing measurable: on 8 host CPUs
# 6 boots at once gave the same passes and timeouts as 4, in 98 s instead of
# 150. Past that liburing's timing-sensitive tests are what would pay for it.
DEBIAN_SMP="${DEBIAN_SMP:-2}"
if [ -z "${DEBIAN_JOBS:-}" ]; then
	DEBIAN_JOBS=$((NPROC * 3 / (2 * DEBIAN_SMP)))
	[ "$DEBIAN_JOBS" -le 6 ] || DEBIAN_JOBS=6
	[ "$DEBIAN_JOBS" -ge 1 ] || DEBIAN_JOBS=1
fi
# A guest that prints nothing for this long is stuck, whatever its deadline.
DEBIAN_SILENCE="${DEBIAN_SILENCE:-120}"
DONE_PATTERN="${DEBIAN_DONE_PATTERN:-DEBIAN-SMOKE: done|KERNEL PANIC|\[PANIC\]}"

boot_start() { # name
	_b="$1"
	_extra=$(boot_extra "$_b")
	_iso="${ISO%.iso}-$_b.iso"
	sh "$PROJECT_DIR/tools/image/mkiso.sh" --stage "$BUILD_DIR/iso-debian-$_b" \
		--out "$_iso" --arch "$ARCH" --kernel "$KERNEL_ELF" --timeout 0 \
		--cmdline "$CMDLINE $_extra" >>"$BUILD_LOG" 2>&1 || {
		printf "  ${RED}ISO FAILED${NC} for %s (log: %s)\n" "$_b" "$BUILD_LOG"
		return 1
	}
	# cache=unsafe below: the overlay is thrown away after the run, so a guest
	# flush has nothing to protect. Honoured, one guest's sync became an
	# fdatasync on the host that took over a hundred seconds with six guests
	# writing, and the boot sat that long after its done marker.
	qemu-img create -q -f qcow2 -F raw -b "$RUN_IMG" "$RUN_DIR/$_b.qcow2" || return 1
	: >"$RUN_DIR/$_b.log"
	echo "[RUN] $_b: booting${_extra:+ [$_extra]}"
	qemu-system-x86_64 $ACCEL_ARGS -m "${DEBIAN_MEM_MB:-1024}" -smp "$DEBIAN_SMP" \
		-cdrom "$_iso" \
		-serial stdio -serial null -display none -monitor none -no-reboot \
		-drive file="$RUN_DIR/$_b.qcow2",if=none,id=debroot,format=qcow2,cache=unsafe \
		-device virtio-blk-pci,drive=debroot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		${EXTRA_QEMU_ARGS:-} >"$RUN_DIR/$_b.log" 2>&1 &
	echo $! >"$RUN_DIR/$_b.pid"
	now >"$RUN_DIR/$_b.t0"
	echo 0 >"$RUN_DIR/$_b.seen"
	echo "0 $(date +%s)" >"$RUN_DIR/$_b.quiet"
}

boot_stop() { # name why
	_pid=$(cat "$RUN_DIR/$1.pid")
	[ -z "$2" ] || echo "[debian-smoke] $2" >>"$RUN_DIR/$1.log"
	# Kill BY PID — never pkill -f, which would match this script's own command line.
	kill -9 "$_pid" 2>/dev/null || true
	wait "$_pid" 2>/dev/null || true
	rm -f "$RUN_DIR/$1.pid" "$RUN_DIR/$1.qcow2"
	_dur=$(since "$(cat "$RUN_DIR/$1.t0")")
	echo "[TIME] $1 qemu-run ${_dur}s"
	echo "$1 $_dur" >>"$RUN_DIR/times"
}

# One look at a running boot: print its new harness lines, and stop it when it
# is done, dead, past its deadline or silent. Returns 0 while it still runs.
boot_poll() { # name
	_b="$1"
	_log="$RUN_DIR/$_b.log"
	_seen=$(cat "$RUN_DIR/$_b.seen")
	_lines=$(wc -l <"$_log" | tr -d ' ')
	if [ "$_lines" -gt "$_seen" ]; then
		sed -n "$((_seen + 1)),${_lines}p" "$_log" | tr -d '\r' |
			grep -a "DEBIAN-SMOKE:" | sed "s/^/[$_b] /" || true
		echo "$_lines" >"$RUN_DIR/$_b.seen"
	fi
	if grep -qa -E "$DONE_PATTERN" "$_log" 2>/dev/null; then
		boot_stop "$_b" ""
		return 1
	fi
	if ! kill -0 "$(cat "$RUN_DIR/$_b.pid")" 2>/dev/null; then
		boot_stop "$_b" "QEMU exited before the done marker"
		return 1
	fi
	_size=$(wc -c <"$_log" | tr -d ' ')
	read -r _qsize _qts <"$RUN_DIR/$_b.quiet"
	_now=$(date +%s)
	if [ "$_size" != "$_qsize" ]; then
		echo "$_size $_now" >"$RUN_DIR/$_b.quiet"
	elif [ $((_now - _qts)) -ge "$DEBIAN_SILENCE" ]; then
		boot_stop "$_b" "silent for ${DEBIAN_SILENCE}s"
		return 1
	fi
	_el=$(awk -v a="$(cat "$RUN_DIR/$_b.t0")" -v b="$(now)" 'BEGIN { printf "%d", b - a }')
	if [ "$_el" -ge "$(boot_deadline "$_b")" ]; then
		boot_stop "$_b" "timeout after $(boot_deadline "$_b")s"
		return 1
	fi
	return 0
}

echo "[RUN] $(echo $BOOTS | wc -w | tr -d ' ') boot(s), $DEBIAN_JOBS at a time"
# Longest first. With the boots two at a time, the lane ends when the last
# one does, and the order decides how much of that is one boot running alone:
# measured, the 60 s part started at the 59th second and finished alone at the
# 120th. The durations are the last run's, per boot; one never timed goes
# first, so that it is timed.
BOOT_TIMES="$PROJECT_DIR/smoke_run/debian-boot-times-$ARCH"
_queue=$(for _b in $BOOTS; do
	_d=$(awk -v b="$_b" '$1 == b { d = $2 } END { print (d == "" ? 99999 : d) }' \
		"$BOOT_TIMES" 2>/dev/null || true)
	echo "${_d:-99999} $_b"
done | sort -rn | awk '{ print $2 }' | tr '\n' ' ')
_running=""
while [ -n "$_queue" ] || [ -n "$_running" ]; do
	for _b in $_running; do
		boot_poll "$_b" || _running=$(echo " $_running " | sed "s/ $_b / /;s/^ *//;s/ *\$//")
	done
	while [ -n "$_queue" ] && [ "$(echo $_running | wc -w)" -lt "$DEBIAN_JOBS" ]; do
		set -- $_queue
		_b="$1"
		shift
		_queue="$*"
		boot_start "$_b" || exit 1
		_running="$_running $_b"
	done
	[ -z "$_running" ] || sleep 1
done
: >"$LOG"
for _b in $BOOTS; do
	cat "$RUN_DIR/$_b.log" >>"$LOG"
done
# Keep this run's durations for the next run's order; boots this run did not
# start keep theirs.
if [ -f "$RUN_DIR/times" ]; then
	{ awk 'NR == FNR { new[$1] = 1; next } !($1 in new)' "$RUN_DIR/times" \
		"$BOOT_TIMES" 2>/dev/null || true; cat "$RUN_DIR/times"; } >"$BOOT_TIMES.new" &&
		mv "$BOOT_TIMES.new" "$BOOT_TIMES"
fi

# ── Check ──────────────────────────────────────────────────────────────────
echo "[TIME] lane-total $(since "$LANE_T0")s"
echo ""
echo "[CHECK] $LOG"
check_output() {
	if grep -qa "$1" "$LOG" 2>/dev/null; then
		pass "$2"
	else
		fail "$2" "missing expected output: $1"
	fi
}

check_output "DEBIAN-SMOKE: ok stage1-dash" "stage1: Debian /bin/dash (glibc dynamic ELF) ran"
check_output "DEBIAN-SMOKE: ok stage2-coreutils" "stage2: ls/cat/mount/ps from the distro"
check_output "DEBIAN-SMOKE: ok stage3-init" "stage3: running under a real init"

# The kernel surfaces our own userspace binaries cover, asserted by Debian's
# own bash, perl and util-linux instead. Each name matches what it replaces:
# proc-* and sig-* stand in for m12/m15, fd-* for m12/m13, errno-* and rename-* for m17,
# ipc-* for m15, job-* for m13_job_control.
for probe in \
	"proc-exit-status:exit status reaches the parent" \
	"proc-signal-status:a killed child reports its signal" \
	"proc-zombie-reaped:a waited-for child leaves no zombie" \
	"proc-setsid:setsid gives a new session" \
	"proc-waitpid-wnohang:waitpid(WNOHANG) before and after the child exits" \
	"sig-handler:a signal handler runs" \
	"sig-mask:a blocked signal arrives only after the unblock" \
	"fd-dup2:dup2 onto a chosen descriptor" \
	"fd-inherit-exec:a descriptor survives exec" \
	"fd-cloexec:close-on-exec takes it away" \
	"errno-eloop:ELOOP on a symlink loop" \
	"errno-enametoolong:ENAMETOOLONG on a long path" \
	"errno-enotdir:ENOTDIR through a file" \
	"errno-eisdir:EISDIR opening a directory for write" \
	"errno-ebadf:EBADF on a closed descriptor" \
	"mem-large-alloc:a 64 MiB allocation is written and read back" \
	"mem-proc-maps:/proc/self/maps describes the address space" \
	"ipc-shm:System V shared memory" \
	"ipc-sem:System V semaphores" \
	"ipc-msg:System V message queues" \
	"job-stop:SIGSTOP really stops a job" \
	"job-cont:SIGCONT resumes it" \
	"clock-advances:the clock moves" \
	"timeout-fires:a timeout kills its child" \
	"exec-many-args:execve carries 5000 arguments" \
	"exec-e2big:an oversized argument is E2BIG" \
	"mq-posix:POSIX message queue send and receive" \
	"sig-ignore:an ignored signal is dropped" \
	"fd-o-path:an O_PATH descriptor cannot be read" \
	"errno-o-nofollow:O_NOFOLLOW on a symlink is ELOOP" \
	"rename-noreplace:renameat2(RENAME_NOREPLACE) refuses an existing target" \
	"rename-einval:renameat2 with conflicting flags is EINVAL" \
	"mem-mremap:mremap grows a mapping" \
	"perm-eacces:nobody cannot open a root-only file" \
	"errno-erofs:a read-only mount refuses writes" \
	"mempolicy-mbind:mbind binds to the one node and refuses a node that does not exist" \
	"mempolicy-get-set:set_mempolicy is read back by get_mempolicy" \
	"mempolicy-mems-allowed:get_mempolicy(MPOL_F_MEMS_ALLOWED) reports node 0" \
	"pkey:protection keys answer as on a CPU without them" \
	"sched-attr:sched_setattr sets nice, sched_getattr reads it, SCHED_FIFO is refused" \
	"kcmp:kcmp tells a dup from another file and two address spaces apart" \
	"pidfd-getfd:pidfd_getfd copies a descriptor, close-on-exec" \
	"process-madvise:process_madvise applies reclaim advice and refuses the rest" \
	"process-mrelease:process_mrelease refuses a live process and accepts a killed one" \
	"cachestat:cachestat counts the cached pages of a file just read" \
	"futex2-wait:futex_wait answers EAGAIN and times out on an absolute deadline" \
	"futex2-wake:futex_wake wakes a waiter in another process" \
	"futex2-waitv:futex_waitv reports which futex woke it" \
	"openat2-no-symlinks:openat2(RESOLVE_NO_SYMLINKS) refuses a symlink" \
	"openat2-beneath:openat2(RESOLVE_BENEATH) refuses .. and absolute escapes" \
	"openat2-in-root:openat2(RESOLVE_IN_ROOT) resolves / at the directory" \
	"openat2-magiclinks-size:openat2 refuses magic links and a short open_how" \
	"statmount-listmount:listmount finds the mounts and statmount describes them by unique id" \
	"remap-file-pages:remap_file_pages shows another file page in the range" \
	"keys:add_key, keyctl read/describe/update/revoke and request_key behave as on Linux" \
	"landlock:a Landlock ruleset confines a process to one directory, symlinks included" \
	"quotactl:quotactl names its target as Linux does: ESRCH with quotas off, ENOTBLK, EINVAL, ENOSYS without quota operations" \
	"memfd-secret:memfd_secret maps shared only, its owner uses it, /proc/self/mem cannot read it" \
	"vdso-glibc:glibc time() and date(1) read the clock through the vDSO, under a seccomp filter that fails the clock system calls"; do
	check_output "DEBIAN-SMOKE: ok ${probe%%:*}" "${probe#*:}"
done

# systemd-nspawn is in the systemd profile's image and not in the sysv one, so
# it is graded only when the image carried it -- an absent container manager is
# not a kernel failure.
if grep -qa "DEBIAN-SMOKE: \(ok\|FAIL\) nspawn" "$LOG" 2>/dev/null; then
	check_output "DEBIAN-SMOKE: ok nspawn" "systemd-nspawn runs a command as PID 1 of new namespaces, with the machine name as hostname and a /proc of its own"
fi

# M125. These two only run when the image was built to hold them, so they are
# checked only when the harness says it got that far — an absent liburing
# directory or an image without fio is not a failure of the kernel.
if grep -qa "DEBIAN-SMOKE: liburing suite starts" "$LOG" 2>/dev/null; then
	check_output "DEBIAN-SMOKE: ok liburing-suite-ran" "liburing's own test suite ran to the end"
	# A fixed list, not a count. These are the tests that cover what this
	# kernel implements — the rings, the queue accounting, the operations, the
	# registrations — and every one of them passes. The total, printed below
	# for information, includes the tests for features that are refused, and a
	# number that moves whenever liburing adds a test for something absent is
	# not a check.
	for t in nop io_uring_setup io_uring_enter probe cq-full cq-ready cq-size \
		cq-peek-batch sq-full sq-space_left short-read pipe-reuse pipe-eof \
		fixed-link file-update poll-ring poll-cancel poll-v-poll eventfd \
		eventfd-reg eventfd-disable drop-submit link_drain connect socket \
		submit-and-wait submit-reuse truncate rename symlink thread-exit \
		teardowns fixed-buf-iter fixed-buf-merge fpos; do
		# The guest's console ends its lines with CR, so anchoring on `$`
		# alone matched nothing and every test in this list was reported as
		# failed however well it had done. Allow the carriage return.
		grep -qa "DEBIAN-SMOKE: liburing-pass $t[[:space:]]*\$" "$LOG" 2>/dev/null &&
			pass "liburing $t" ||
			fail "liburing $t" "the test did not pass"
	done
	sed -n 's/.*DEBIAN-SMOKE: \(liburing totals .*\)/  \1/p' "$LOG" | tail -1
fi
# M133. The kernel's BTF, through the distribution's bpftool (libbpf).
check_output "DEBIAN-SMOKE: ok bpftool-btf-vmlinux" "libbpf parses /sys/kernel/btf/vmlinux: the kernel's BTF, over a thousand types"
check_output "DEBIAN-SMOKE: ok bpftool-btf-c" "bpftool renders the kernel's types as C from its BTF"
check_output "DEBIAN-SMOKE: ok bpftool-btf-list" "bpftool btf list finds the kernel's BTF by id, named vmlinux"
# M133. bpftrace, when the image was built with BPFTRACE=1: a kprobe program
# counting into a map, a perf-event program ending the run through the output
# channel, attached through the kprobe PMU.
if grep -qa "DEBIAN-SMOKE: bpftrace is present" "$LOG" 2>/dev/null; then
	check_output "DEBIAN-SMOKE: ok bpftrace-kprobe-count" "bpftrace counts a kprobe's hits into a map and exits from a timer probe"
	check_output "DEBIAN-SMOKE: ok bpftrace-begin" "bpftrace runs a BEGIN probe and prints through its output ring"
	check_output "DEBIAN-SMOKE: ok bpftrace-uprobe-count" "bpftrace counts a uprobe on a libc function in every ls"
fi

# M126. The distribution's own perf, when the image was built with PERF=1.
if grep -qa "DEBIAN-SMOKE: perf is" "$LOG" 2>/dev/null; then
	check_output "DEBIAN-SMOKE: ok perf-version" "the distribution's perf runs on this kernel"
	check_output "DEBIAN-SMOKE: ok perf-stat" "perf stat counts task-clock and page-faults for a command it runs"
	check_output "DEBIAN-SMOKE: ok perf-stat-hw" "perf stat counts cycles and instructions on the CPU's own counters"
	check_output "DEBIAN-SMOKE: ok perf-record" "perf record samples a command through the mmap'd ring buffer and writes a perf.data"
	check_output "DEBIAN-SMOKE: ok perf-report" "perf report reads its own perf.data back and attributes the samples to symbols"
	sed -n 's/.*DEBIAN-SMOKE: \(perf counted .*\)/  \1/p' "$LOG" | tail -1
	sed -n 's/.*DEBIAN-SMOKE: \(perf record took .*\)/  \1/p' "$LOG" | tail -1
	sed -n 's/.*DEBIAN-SMOKE: \(perf report: .*\)/  \1/p' "$LOG" | tail -1
fi

if grep -qa "fio-io_uring" "$LOG" 2>/dev/null; then
	check_output "DEBIAN-SMOKE: ok fio-io_uring" "fio writes 8 MiB through its own io_uring engine and the file is that size"
	check_output "DEBIAN-SMOKE: ok fio-io_uring-fixed" "fio reads it back with registered files and registered buffers"
fi

# M129. Suspend through util-linux's rtcwake, when the image has it.
#
# The kernel's own suspend is proved on the Alpine lane (M129-SUSPEND, eleven
# checks: the freezer holds userspace, the RTC alarm wakes the machine, the
# frozen child's timeline has a hole of the right length). This is the other
# half: a Debian userspace, util-linux's own tool, arming /dev/rtc0 and
# writing /sys/power/state. Nothing here is a b1nix interface.
if grep -qa "DEBIAN-SMOKE: rtcwake rc=" "$LOG" 2>/dev/null; then
	check_output "DEBIAN-SMOKE: ok suspend-rtcwake" "util-linux's rtcwake suspends this kernel and the RTC alarm wakes it — the distribution's own suspend path, not a b1nix interface"
	check_output "DEBIAN-SMOKE: ok suspend-alive" "the machine writes files and reads /proc after coming back from suspend"
	sed -n 's/.*DEBIAN-SMOKE: \(rtcwake rc=.*\)/  \1/p' "$LOG" | tail -1
else
	printf "  ${YELLOW}SKIP${NC} suspend-rtcwake - this image has no rtcwake, or no /sys/power/state was found\n"
fi
check_output "DEBIAN-SMOKE: done" "harness reached the end"

if grep -qa -E "KERNEL PANIC|\[PANIC\]" "$LOG" 2>/dev/null; then
	fail "no kernel panic" "the log contains a panic"
else
	pass "no kernel panic"
fi

if grep -qa "DEBIAN-SMOKE: FAIL" "$LOG" 2>/dev/null; then
	echo ""
	echo "  in-guest failures reported by the harness:"
	grep -a "DEBIAN-SMOKE: FAIL" "$LOG" | sed 's/^/    /'
fi

echo ""
printf "Results: ${GREEN}%d passed${NC}, ${RED}%d failed${NC}\n" "$PASSED" "$FAILED"
echo "Log: $LOG"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
