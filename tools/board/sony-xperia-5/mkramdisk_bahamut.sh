#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the ext4 ramdisk that ships inside the Xperia 5 boot image.
#
# The phone has no block device this kernel can drive (UFS is unsupported), so
# the ext4 rootfs the smoke lanes mount off a virtio disk has nowhere to come
# from — and the initramfs compiled into the kernel carries only smoke-test
# binaries. Without this image there is no /bin/sh on the device at all.
#
# The Android boot image has a ramdisk slot for exactly this. boot_a is 64 MiB
# and the kernel takes ~4.5 of it, so the budget here is about 55 MiB — far
# less than the 154 MiB staged rootfs, which is mostly development headers,
# locale/font data and a browser. This stages a working shell environment
# instead: BusyBox and every applet it multicalls, musl and its loader, /etc,
# and the project's standalone ELFs. Widen ROOTFS_KEEP when something specific
# turns out to be missing — there is room.
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$(dirname "$(dirname "$DIR")")")"
SRC="${1:-$ROOT/build/aarch64/rootfs}"
OUT="${2:-$ROOT/build/aarch64/bahamut-ramdisk.ext4}"
SIZE_MB="${RAMDISK_MB:-52}"
# FULL_ROOTFS=1 stages the whole tree instead of the trimmed shell environment:
# that is the rootfs flashed to the phone's UFS (see mkrootfs_bahamut.sh), which
# has gigabytes to spend instead of the boot image's 55 MiB.
FULL_ROOTFS="${FULL_ROOTFS:-0}"
# The label decides who becomes /. The kernel mounts the first device labelled
# b1nix-root and only then falls back to ram0 by name — so a ramdisk riding
# along with a UFS root must NOT carry that label, or it may win the race.
LABEL="${RAMDISK_LABEL:-b1nix-root}"
STAGE="$OUT.stage"

if [ -f "$OUT" ] && [ "${FORCE_RAMDISK:-0}" != "1" ]; then
    echo "[*] Reusing existing ramdisk $OUT"
    exit 0
fi

# Homebrew keeps e2fsprogs keg-only: only mke2fs is linked onto PATH, and on a
# machine with Android platform-tools installed even that one is theirs, not
# e2fsprogs'. debugfs is never linked — so a bare `debugfs` was not found and
# the ownership pass below (the thing that makes the image root-owned) silently
# did nothing. Prefer the keg's sbin for both tools.
E2FSDIR=""
for d in /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin /sbin /usr/sbin; do
    [ -x "$d/debugfs" ] && [ -x "$d/mke2fs" ] && { E2FSDIR="$d"; break; }
done
MKE2FS="${MKE2FS:-${E2FSDIR:+$E2FSDIR/}mke2fs}"
DEBUGFS="${DEBUGFS:-${E2FSDIR:+$E2FSDIR/}debugfs}"
command -v "$MKE2FS" >/dev/null 2>&1 || { echo "[!] mke2fs not found"; exit 1; }
command -v "$DEBUGFS" >/dev/null 2>&1 || { echo "[!] debugfs not found (brew install e2fsprogs)"; exit 1; }

[ -d "$SRC" ] || { echo "[!] rootfs not staged at $SRC — run 'make root-image' first"; exit 1; }

echo "[*] Staging from $SRC"
rm -rf "$STAGE"
mkdir -p "$STAGE"

if [ "$FULL_ROOTFS" = "1" ]; then
    cp -a "$SRC/." "$STAGE/"
    # Nothing in OpenRC's runlevels starts sshd — the image's /etc/rc does,
    # and BusyBox init never runs that. On the phone SSH over the USB gadget
    # is the way in, so start it from the default runlevel's local.d.
    mkdir -p "$STAGE/etc/local.d"
    printf '#!/bin/sh\n# Xperia 5: SSH over the USB Ethernet gadget (usb0).\n/bin/sh /etc/init.d/sshd start\n' \
        > "$STAGE/etc/local.d/50-sshd.start"
    chmod 755 "$STAGE/etc/local.d/50-sshd.start"
    # The phone has no serial port and a forced power-off clears RAM (and with
    # it the ramoops log), so keep the kernel log on the flash too: Android on
    # slot B reads it back out of system_a.
    printf '#!/bin/sh\n# Xperia 5: keep the kernel log on flash (/var/log/b1nix-boot.log).\n(\n  for t in 0 20 60; do\n    sleep $t\n    dmesg > /var/log/b1nix-boot.log\n    sync\n  done\n) &\n' \
        > "$STAGE/etc/local.d/10-bootlog.start"
    chmod 755 "$STAGE/etc/local.d/10-bootlog.start"
else

# Directories copied whole. /lib is NOT among them: it is 64 MiB of shared
# objects for a graphics and browser stack that cannot run here anyway.
# /bin alone is 26 MiB of ported programs; the image has to fit in the gap
# between the kernel's own allocations and the first firmware carveout, so this
# is a shell environment, not the full tree.
# libexec carries OpenRC's helper scripts (gendepends.sh and friends). Without
# it PID 1 comes up but every runlevel fails to build its dependency cache, so
# nothing in /etc/local.d — the smoke suite included — ever runs.
for d in bin sbin libexec etc opt var run tmp root proc sys dev mnt persist home; do
    [ -e "$SRC/$d" ] && cp -R "$SRC/$d" "$STAGE/" 2>/dev/null || true
done
mkdir -p "$STAGE/proc" "$STAGE/sys" "$STAGE/dev" "$STAGE/tmp" "$STAGE/run" "$STAGE/mnt"

# /usr, minus the 34 MiB of locale, font and documentation data under
# usr/share and the development headers under usr/include — neither of which
# anything on this board reads, and together they are most of the tree. The
# size trim below already named $STAGE/usr, which did nothing while /usr was
# never staged at all.
if [ -d "$SRC/usr" ]; then
    mkdir -p "$STAGE/usr"
    for d in bin sbin libexec local; do
        [ -e "$SRC/usr/$d" ] && cp -R "$SRC/usr/$d" "$STAGE/usr/" 2>/dev/null || true
    done
fi

# musl and its dynamic loader: every binary above is a dynamic PIE and none of
# them start without these two.
mkdir -p "$STAGE/lib"
# Shared objects the shell environment actually needs. Copying every small
# .so instead pulled in 64 MiB of graphics and browser stack — the staged tree
# came to 74 MiB and mke2fs ran out of blocks. Name them.
for f in libz.so* libcrypto.so* libssl.so* libncurses*.so* libtinfo*.so* \
         libreadline*.so* libstdc++.so* libc++.so* libc++abi.so* \
         libgcc_s.so* libunwind.so* libcrypt.so* libpam*.so*; do
    cp -a "$SRC/lib/"$f "$STAGE/lib/" 2>/dev/null || true
done
[ -d "$SRC/lib/security" ] && cp -R "$SRC/lib/security" "$STAGE/lib/" 2>/dev/null || true

# Trim the ported programs, never the C library. An earlier cut here applied
# the size cap to /lib as well and deleted libc.so itself (922 KiB) — with it
# gone not one dynamically linked binary in the image can start, which is all
# of them.
find "$STAGE/bin" "$STAGE/sbin" "$STAGE/usr" -type f -size +900k -print -delete \
    2>/dev/null | sed 's|^|    dropped (too big): |' || true

# musl and its loader, copied last so no size rule can reach them: every
# binary in the image is a dynamic PIE and none of them start without these.
for f in ld-musl-aarch64.so.1 libc.so; do
    [ -e "$SRC/lib/$f" ] && cp -a "$SRC/lib/$f" "$STAGE/lib/" || \
        { echo "[!] missing $SRC/lib/$f — nothing would run"; exit 1; }
done
fi

echo "[*] Staged size: $(du -sm "$STAGE" | cut -f1) MiB (budget ${SIZE_MB})"

rm -f "$OUT"
truncate -s "${SIZE_MB}M" "$OUT"
# The kernel's ext4 driver does not implement these features — see CLAUDE.md.
"$MKE2FS" -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -q \
    -L "$LABEL" -E root_owner=0:0 -d "$STAGE" "$OUT"

# mke2fs -d stamps the BUILD HOST's uid/gid on every file; a Unix root
# filesystem belongs to root, and things that check ownership break otherwise.
( cd "$STAGE" && find . \( -type f -o -type d -o -type l \) -print |
  sed -e 's|^\.||' -e '/^$/d' |
  awk '{ printf "sif \"%s\" uid 0\nsif \"%s\" gid 0\n", $0, $0 }' ) > "$OUT.own"
"$DEBUGFS" -w -f "$OUT.own" "$OUT" >/dev/null 2>&1 || \
    { echo "[!] debugfs ownership pass failed"; exit 1; }
rm -f "$OUT.own"
rm -rf "$STAGE"

echo "[OK] $OUT ($(du -m "$OUT" | cut -f1) MiB)"
