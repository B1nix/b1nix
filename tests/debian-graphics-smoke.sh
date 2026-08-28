#!/bin/sh
# debian-graphics-smoke.sh — a stock Debian userspace drawing a desktop on a
# DRM card, judged from outside the guest.
#
# The systemd image reaches graphical.target with nothing behind it: no display
# server is installed, so the target is a name and the scanout is whatever the
# firmware left. This image installs Debian's Weston and starts it on the real
# DRM path, and this script decides whether a run reproduced a picture.
#
# Four things are asserted, and each one is a thing the guest cannot forge:
#
#   1. The frame.  QEMU's `screendump` reads the framebuffer the guest
#      programmed into the virtual GPU; no process inside the guest takes part
#      in producing the file.  A display that never drew dumps a solid
#      rectangle -- two or three distinct colours.  A desktop with a shell
#      background, a panel and a terminal on it has thousands.  The threshold
#      is on that count.
#
#   2. The device open.  The kernel's own trace records every open under
#      /dev/dri with the name of the task that made it.  A compositor that drew
#      opened a card; a harness that printed a marker did not.
#
#   3. No compositor underneath.  The run must have taken Weston's DRM backend
#      on bare QEMU, with no Wayland or X server beneath it -- a compositor
#      nested in another one proves its clients work, not that it drives a
#      display.
#
#   4. Debian's own init got there.  systemd is PID 1 and the session is
#      started by a unit, not by a shell the harness sshed into.
#
# Usage: sh tests/debian-graphics-smoke.sh [tag]
#   GFX_NO_BUILD=1     use the ISO and the image as they stand
#   GFX_MIN_COLOURS=N  frame threshold (default 1000)
set -u

DIR=$(cd "$(dirname "$0")/.." && pwd)
TAG=${1:-debian-graphics}
LOG=$DIR/smoke_run/$TAG.log
FRAMES=$DIR/smoke_run/$TAG-frames
MIN_COLOURS=${GFX_MIN_COLOURS:-1000}
IMG=$DIR/build/x86_64/debian-graphics.ext4
ISO=$DIR/build/x86_64/b1nix-graphics.iso
IMG_LABEL=${IMG_LABEL:-b1nix-graphics}

pass=0
fail=0
ok() {
	pass=$((pass + 1))
	echo "GFX-SMOKE: ok $1"
}
bad() {
	fail=$((fail + 1))
	echo "GFX-SMOKE: FAIL $1"
}

mkdir -p "$DIR/smoke_run"

if [ ! -f "$IMG" ]; then
	echo "GFX-SMOKE: skipped — $IMG not built."
	echo "GFX-SMOKE: build it with: PROFILE=graphics sh tools/images/mk-debian-image.sh"
	exit 0
fi

# b1nix.trace-sysfs is not a debugging aid here, it is check 2: it is the only
# record of who opened the card that does not come from the process claiming to
# have opened it.
CMDLINE="root=LABEL=$IMG_LABEL init=/sbin/init \
systemd.unit=${GFX_TARGET:-multi-user.target} b1nix.trace-sysfs \
${GFX_EXTRA_CMDLINE:-}"

if [ "${GFX_NO_BUILD:-0}" != "1" ]; then
	echo "GFX-SMOKE: building the kernel ISO for the graphics boot"
	if ! (cd "$DIR" && make -j"${JOBS:-6}" KERNEL_CMDLINE="$CMDLINE" iso) \
		> "$DIR/smoke_run/$TAG-build.log" 2>&1; then
		echo "GFX-SMOKE: FAIL build (log: $DIR/smoke_run/$TAG-build.log)"
		tail -40 "$DIR/smoke_run/$TAG-build.log"
		exit 1
	fi
	cp "$DIR/build/x86_64/b1nix.iso" "$ISO"
	sync
fi
[ -f "$ISO" ] || {
	echo "GFX-SMOKE: FAIL no ISO at $ISO"
	exit 1
}

flock "$DIR/smoke_run/.qemu.lock" sh "$DIR/tools/run-debian-graphics.sh" "$TAG" || true

if [ ! -f "$LOG" ]; then
	echo "GFX-SMOKE: FAIL no log at $LOG"
	exit 1
fi

# 4. Debian's own init, and the session started as a unit.
if grep -aq "GFX-SMOKE: ok pid1-systemd" "$LOG"; then
	ok "debian-systemd-pid1"
else
	bad "debian-systemd-pid1 (/proc/1/comm is not Debian's systemd)"
	grep -a "GFX-SMOKE: FAIL pid1-systemd" "$LOG" | head -1
fi
if grep -aq "GFX-SMOKE: start pid=" "$LOG"; then
	ok "session-unit-ran"
else
	bad "session-unit-ran (b1nix-graphics.service never started the harness)"
	grep -a "b1nix-graphics" "$LOG" | head -5
fi

# The card, and the udev database entry that gives it a seat.
if grep -aq "GFX-SMOKE: ok drm-card" "$LOG"; then
	ok "drm-card"
	grep -a "GFX-SMOKE: ok drm-card" "$LOG" | head -1
else
	bad "drm-card (no /dev/dri/card* that /sys/class/drm also describes)"
fi
if grep -aq "GFX-SMOKE: ok udev-db-drm" "$LOG"; then
	ok "udev-db-drm"
else
	bad "udev-db-drm (no /run/udev/data entry for a DRM card)"
fi

# 3. The compositor came up on the DRM backend, with nothing underneath.
if grep -aq "GFX-SMOKE: ok weston-socket" "$LOG"; then
	ok "weston-socket"
else
	bad "weston-socket (weston never created a Wayland socket)"
	grep -a "GFX-SMOKE: FAIL weston\|weston:" "$LOG" | head -8
fi
if grep -aq "GFX-SMOKE: ok weston-drm-backend" "$LOG"; then
	ok "weston-drm-backend"
else
	bad "weston-drm-backend (the run did not take the real DRM path)"
fi
# Weston names every backend module it loads. The DRM one talks to the card;
# the wayland and x11 ones talk to a compositor or an X server that is already
# running, which is the thing this must rule out. Asking which module was
# loaded is the direct question -- an earlier version of this check looked for
# WAYLAND_DISPLAY anywhere in the log and reported a nested compositor because
# the harness printed its own environment.
if grep -aqE "Loading module .*(wayland|x11|headless)-backend\.so" "$LOG"; then
	bad "no-nested-compositor (weston loaded a nested or headless backend)"
	grep -a "Loading module .*backend" "$LOG" | head -3
else
	ok "no-nested-compositor"
fi
if grep -aq "GFX-SMOKE: ok weston-alive" "$LOG"; then
	ok "weston-alive"
else
	bad "weston-alive (weston did not survive the run)"
fi
# A client that is still running is one that connected, allocated a buffer and
# had it accepted. It is not the same claim as the frame: the frame says
# something was drawn, this says a separate process drew part of it.
if grep -aq "GFX-SMOKE: ok client-drawing" "$LOG"; then
	ok "client-drawing"
else
	bad "client-drawing (no wayland client survived its first frame)"
	grep -a "GFX-SMOKE: client\|simple-shm rc=" "$LOG" | head -8
fi

# 2. The kernel saw the compositor open the card.
#
# b1nix.trace-sysfs prints every /dev/dri open with the task that made it, from
# inside the kernel. The harness's own probes open nothing under /dev/dri, so
# the task name is what matters: weston, or logind on its behalf.
if grep -aiE "(sysfs-open|drm): (open )?/dev/dri/card" "$LOG" \
	| grep -aiqE "weston|logind"; then
	ok "compositor-opened-card"
	grep -aiE "(sysfs-open|drm): (open )?/dev/dri/card" "$LOG" \
		| grep -aiE "weston|logind" | head -3
else
	bad "compositor-opened-card (no weston/logind open of /dev/dri in the kernel trace)"
	echo "GFX-SMOKE: --- every /dev/dri open the kernel saw ---"
	grep -aiE "(sysfs-open|drm): (open )?/dev/dri" "$LOG" | head -12
fi

# 1. The picture has contents.
best=0
bestf=
if [ -d "$FRAMES" ]; then
	for f in "$FRAMES"/frame-*.ppm; do
		[ -f "$f" ] || continue
		line=$(python3 "$DIR/tools/ppm-colours.py" "$f" 2> /dev/null) || continue
		n=$(echo "$line" | sed -n 's/.*unique=\([0-9]*\).*/\1/p')
		[ -n "$n" ] || continue
		if [ "$n" -gt "$best" ]; then
			best=$n
			bestf=$f
		fi
	done
fi
echo "GFX-SMOKE: richest frame: ${bestf:-none} unique=$best (threshold $MIN_COLOURS)"
if [ "$best" -ge "$MIN_COLOURS" ]; then
	ok "desktop-drawn"
	png=$DIR/smoke_run/$TAG-shot.png
	if command -v convert > /dev/null 2>&1 && [ -n "$bestf" ]; then
		convert "$bestf" "$png" 2> /dev/null && echo "GFX-SMOKE: picture: $png"
	fi
else
	bad "desktop-drawn (the scanout never carried a desktop)"
fi

if grep -aqE "KERNEL PANIC|\[PANIC\]" "$LOG"; then
	bad "no-kernel-panic (the log contains a panic)"
else
	ok "no-kernel-panic"
fi

echo "GFX-SMOKE: $pass passed, $fail failed"
echo "GFX-SMOKE: log: $LOG"
[ "$fail" -eq 0 ] || exit 1
echo "GFX-SMOKE: done"
