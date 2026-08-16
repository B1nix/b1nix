#!/bin/sh
# try-alpine-rootfs.sh — boot the b1nix kernel with a stock Alpine Linux
# userspace, to check the M94 claim (foreign userspace independence) against a
# real distribution instead of our own rootfs.
#
# Nothing here is b1nix-flavoured: the tarball is Alpine's published
# x86_64 minirootfs, the binaries are Alpine's dynamic PIEs, and they resolve
# through ALPINE's /lib/ld-musl-x86_64.so.1 — not ours. The only thing added to
# the image is /etc/inittab plus a probe script, so BusyBox init has something
# to run and the machine powers itself off at the end.
#
# Needs network on the host (one download, cached under build/dist/) plus
# mke2fs, limine/xorriso and qemu-system-x86_64. Not part of `make iso` or the
# smoke suite: those must stay offline and deterministic.
#
# Usage: sh tools/try-alpine-rootfs.sh [alpine-version]

set -eu

SELF="$(readlink -f "$0" 2>/dev/null || readlink "$0" 2>/dev/null || echo "$0")"
SCRIPT_DIR="$(cd "$(dirname "$SELF")" && pwd)"
PROJECT_DIR=""
cur="$SCRIPT_DIR"
while [ "$cur" != "/" ]; do
  if [ -f "$cur/Makefile" ] && [ -d "$cur/kernel" ]; then
    PROJECT_DIR="$cur"
    break
  fi
  cur="$(dirname "$cur")"
done
[ -n "$PROJECT_DIR" ] || PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARCH=x86_64
ALPINE_VER="${1:-3.20.3}"
ALPINE_BRANCH="v$(echo "$ALPINE_VER" | cut -d. -f1,2)"
TARBALL_URL="https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/releases/$ARCH/alpine-minirootfs-$ALPINE_VER-$ARCH.tar.gz"

WORK="$PROJECT_DIR/build/$ARCH/alpine"
DIST="$PROJECT_DIR/build/dist"
TARBALL="$DIST/alpine-minirootfs-$ALPINE_VER-$ARCH.tar.gz"
ROOT="$WORK/root"
IMG="$WORK/alpine.ext4"
ISO_DIR="$WORK/iso"
ISO="$WORK/alpine.iso"
LOG="$PROJECT_DIR/smoke_run/alpine-$ARCH.log"

KERNEL="$PROJECT_DIR/build/$ARCH/kernel.elf"
[ -f "$KERNEL" ] || { echo "no $KERNEL — run: make ARCH=$ARCH" >&2; exit 1; }

MKE2FS="$(command -v mke2fs || echo /sbin/mke2fs)"
[ -x "$MKE2FS" ] || { echo "mke2fs not found (install e2fsprogs)" >&2; exit 1; }

mkdir -p "$DIST" "$WORK" "$PROJECT_DIR/smoke_run"

# ── 1. Fetch + unpack the stock minirootfs ─────────────────────────────────
if [ ! -f "$TARBALL" ]; then
	echo "[alpine] fetching $ALPINE_VER minirootfs..."
	if command -v curl >/dev/null 2>&1; then
		curl -sSL -o "$TARBALL" "$TARBALL_URL"
	else
		wget -q -O "$TARBALL" "$TARBALL_URL"
	fi
fi
rm -rf "$ROOT"
mkdir -p "$ROOT"
tar xzf "$TARBALL" -C "$ROOT"

# ── 2. Give BusyBox init something to do ───────────────────────────────────
# The minirootfs ships no inittab, so BusyBox falls back to its built-in one:
# /sbin/openrc (not installed in a minirootfs) and gettys on /dev/tty1..6
# (b1nix has no VTs). Replace it with a single sysinit script on the console.
cat > "$ROOT/etc/inittab" <<'EOF'
::sysinit:/bin/sh /alpine-probe.sh
EOF

cat > "$ROOT/alpine-probe.sh" <<'EOF'
#!/bin/sh
# Every command below is an Alpine binary running against Alpine's musl.
echo "ALPINE: start"
echo "ALPINE: release=$(cat /etc/alpine-release 2>/dev/null)"
echo "ALPINE: uname=$(uname -srm)"
echo "ALPINE: id=$(id -u):$(id -g)"
echo "ALPINE: applets=$(busybox --list | wc -l)"
mkdir -p /tmp/probe && echo hello > /tmp/probe/f && echo "ALPINE: file=$(cat /tmp/probe/f)"
echo "ALPINE: pipe=$(echo one two three | tr ' ' '\n' | sort | tail -1)"
echo "ALPINE: awk=$(echo 21 | awk '{print $1*2}')"
echo "ALPINE: sed=$(echo b1nix | sed 's/1/one/')"
echo "ALPINE: done"
sync
poweroff -f
EOF
chmod +x "$ROOT/alpine-probe.sh"

# ── 3. ext4 image + ISO, same shape as the b1nix root ──────────────────────
# The kernel's ext4 driver does not implement metadata_csum/64bit/flex_bg.
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=96 status=none
"$MKE2FS" -F -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file \
	-L b1nix-root -d "$ROOT" -q "$IMG"

rm -rf "$ISO_DIR"
"$PROJECT_DIR/tools/mkiso.sh" --stage "$ISO_DIR" --out "$ISO" --arch "$ARCH" \
    --kernel "$KERNEL" --timeout 0 --cmdline "init=/sbin/init" \
    --module "$IMG:rootfs.img" >/dev/null

# ── 4. Boot it ─────────────────────────────────────────────────────────────
echo "[alpine] booting (log: $LOG)"
timeout 120 qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -cdrom "$ISO" \
	-serial stdio -display none -no-reboot >"$LOG" 2>&1 || true

# ── 5. Verdict ─────────────────────────────────────────────────────────────
fail=0
for marker in "ALPINE: start" "ALPINE: release=$ALPINE_VER" "ALPINE: applets=" \
              "ALPINE: file=hello" "ALPINE: pipe=two" "ALPINE: awk=42" \
              "ALPINE: sed=bonenix" "ALPINE: done" "reboot: powering off"; do
	if grep -qa "$marker" "$LOG"; then
		echo "  ok   $marker"
	else
		echo "  FAIL $marker"
		fail=1
	fi
done

if [ "$fail" -eq 0 ]; then
	echo "[alpine] a stock Alpine $ALPINE_VER userspace boots on b1nix and powers itself off"
else
	echo "[alpine] FAILED — see $LOG" >&2
fi
exit "$fail"
