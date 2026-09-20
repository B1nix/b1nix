#!/bin/sh
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
if [ "${SKIP_BUILD:-0}" = "1" ]; then
	[ -f "$ISO" ] || { printf "  ${RED}no prebuilt %s${NC}\n" "$ISO"; exit 1; }
	echo "  (SKIP_BUILD=1 — reusing $ISO)"
else
	echo "[BUILD] Building kernel ISO for the Debian boot..."
	if ! (cd "$PROJECT_DIR" && make -j"$NPROC" ARCH="$ARCH" ${SMOKE_MAKE_ARGS:-} \
		KERNEL_CMDLINE="$CMDLINE" iso) >"$BUILD_LOG" 2>&1; then
		printf "  ${RED}BUILD FAILED${NC} (log: %s)\n" "$BUILD_LOG"
		tail -60 "$BUILD_LOG"
		exit 1
	fi
	# `iso` always writes b1nix.iso; keep a stable copy for SKIP_BUILD reruns.
	cp "$BUILD_DIR/b1nix.iso" "$ISO"
	# The image is 550 MB and QEMU mmaps it: hand the copy to the page cache
	# before booting from it, or the first run can read a half-written ISO.
	sync
	pass "kernel builds without errors"
fi

# ── Per-run scratch copy of the image ──────────────────────────────────────
# The pristine image is never written by a test run.
RUN_IMG="$PROJECT_DIR/smoke_run/debian-root-$$.img"
cp "$IMG" "$RUN_IMG"
# Put the current harness into the copy.
#
# The script inside the image is whatever the image was built with, which may
# be months old; debugfs writes the working-tree version into the scratch copy
# without root and without rebuilding the image, so the test always runs the
# harness that sits beside it in the repository.
STAGE="$PROJECT_DIR/tools/image/debian-stage.sh"
if [ -f "$STAGE" ] && command -v debugfs >/dev/null 2>&1; then
	if debugfs -w -R "rm /b1nix-stage.sh" "$RUN_IMG" >/dev/null 2>&1 &&
		debugfs -w -R "write $STAGE b1nix-stage.sh" "$RUN_IMG" >/dev/null 2>&1; then
		echo "  (harness injected from tools/image/debian-stage.sh)"
	else
		printf "  ${YELLOW}note${NC}: could not inject the harness; using the one in the image\n"
	fi
fi
QEMU_PID=""
cleanup() {
	[ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null || true
	rm -f "$RUN_IMG"
}
trap cleanup EXIT INT TERM

# ── Run ────────────────────────────────────────────────────────────────────
ACCEL_ARGS=""
if [ -w /dev/kvm ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw kvm; then
	ACCEL_ARGS="-accel kvm -cpu host,+invtsc"
elif [ "$(uname)" = "Darwin" ] && qemu-system-x86_64 -accel help 2>/dev/null | grep -qw hvf; then
	ACCEL_ARGS="-accel hvf -cpu host"
fi

echo "[RUN] Booting QEMU with $RUN_IMG as root (label $IMG_LABEL)..."
: >"$LOG"
qemu-system-x86_64 $ACCEL_ARGS -m "${DEBIAN_MEM_MB:-1024}" -smp "${DEBIAN_SMP:-2}" \
	-cdrom "$ISO" \
	-serial stdio -serial null -display none -monitor none -no-reboot \
	-drive file="$RUN_IMG",if=none,id=debroot,format=raw \
	-device virtio-blk-pci,drive=debroot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	${EXTRA_QEMU_ARGS:-} >"$LOG" 2>&1 &
QEMU_PID=$!

DONE_PATTERN="${DEBIAN_DONE_PATTERN:-DEBIAN-SMOKE: done|KERNEL PANIC|\[PANIC\]}"
start_ts=$(date +%s)
reported=0
while :; do
	lines=$(wc -l <"$LOG" | tr -d ' ')
	if [ "$lines" -gt "$reported" ]; then
		sed -n "$((reported + 1)),${lines}p" "$LOG" | grep -a "DEBIAN-SMOKE:" || true
		reported=$lines
	fi
	if grep -qa -E "$DONE_PATTERN" "$LOG" 2>/dev/null; then
		break
	fi
	if ! kill -0 "$QEMU_PID" 2>/dev/null; then
		sleep 1
		echo "[debian-smoke] QEMU exited before the done marker" >>"$LOG"
		break
	fi
	now_ts=$(date +%s)
	if [ $((now_ts - start_ts)) -ge "$TIMEOUT" ]; then
		echo "[debian-smoke] timeout after ${TIMEOUT}s" >>"$LOG"
		break
	fi
	sleep 1
done
# Kill BY PID — never pkill -f, which would match this script's own command line.
kill -9 "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

# ── Check ──────────────────────────────────────────────────────────────────
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
	"vdso-glibc:glibc time() and date(1) read the clock through the vDSO, under a seccomp filter that fails the clock system calls" \
	"nspawn:systemd-nspawn runs a command as PID 1 of new namespaces, with the machine name as hostname and a /proc of its own"; do
	check_output "DEBIAN-SMOKE: ok ${probe%%:*}" "${probe#*:}"
done

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
		grep -qa "DEBIAN-SMOKE: liburing-pass $t\$" "$LOG" 2>/dev/null &&
			pass "liburing $t" ||
			fail "liburing $t" "the test did not pass"
	done
	sed -n 's/.*DEBIAN-SMOKE: \(liburing totals .*\)/  \1/p' "$LOG" | tail -1
fi
if grep -qa "fio-io_uring" "$LOG" 2>/dev/null; then
	check_output "DEBIAN-SMOKE: ok fio-io_uring" "fio writes 8 MiB through its own io_uring engine and the file is that size"
	check_output "DEBIAN-SMOKE: ok fio-io_uring-fixed" "fio reads it back with registered files and registered buffers"
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
