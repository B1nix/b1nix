#!/bin/sh
# sway-probe.sh — does sway actually put pixels anywhere?
#
# The prior work got sway as far as committing a headless output: with /dev/shm
# present wlroots' allocator stopped failing, and `swaymsg -t get_outputs`
# returned a real 1280x720 HEADLESS-1. What never arrived was a screenshot, so
# "it renders" was still an assumption.
#
# This finishes that. sway runs on the headless backend with the pixman
# renderer, is told what colour to paint its background, and a frame is pulled
# back out through wlr-screencopy (grim). Two shots with two different colours:
# one uniform image proves only that something filled a buffer, two that follow
# the colour asked for prove the compositor rendered what it was told.
#
# Run from /etc/inittab, and only when the kernel cmdline carries
# b1nix.swayprobe: it fetches packages over the network and takes minutes, so
# every ordinary boot — and every smoke instance — must skip it. It cannot be
# init= itself: the init loader takes an ELF, and a "#!" script is not one.
grep -q "b1nix.swayprobe" /proc/cmdline 2>/dev/null || exit 0

echo "SWAY-PROBE: start"

export HOME=/root
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mkdir -p /run /tmp /root

# The real Alpine repositories: sway, wlroots and grim are theirs, unmodified
# x86_64 binaries (Alpine v3.20 and b1nix both build musl 1.2.5).
cat > /etc/bpkg.conf <<'EOF'
INDEX_URL=https://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64/APKINDEX.tar.gz https://dl-cdn.alpinelinux.org/alpine/v3.20/community/x86_64/APKINDEX.tar.gz
EOF

# Before anything else: is a shared mapping shared? A compositor and its
# clients are built on that, and if it is broken everything downstream still
# reports success while the screen stays blank.
if [ -x /bin/shmshare_smoke ]; then
	/bin/shmshare_smoke
fi

echo "SWAY-PROBE: updating index"
if bpkg update; then
	echo "SWAY-PROBE: ok update"
else
	echo "SWAY-PROBE: fail update"
	echo "SWAY-PROBE: done"
	exit 0
fi

# swaybg matters as much as sway here: sway does not paint a solid background
# itself, it spawns swaybg as an ordinary Wayland client to do it. Without it
# the compositor logs "failed to execute 'swaybg'" and the output stays black —
# which reads exactly like "nothing renders" and is not. With it, the check is
# stronger anyway: a real client renders, sway composites, screencopy delivers.
for pkg in sway swaybg grim; do
	echo "SWAY-PROBE: installing $pkg"
	if bpkg install "$pkg"; then
		echo "SWAY-PROBE: ok install-$pkg"
	else
		echo "SWAY-PROBE: fail install-$pkg"
	fi
done

if [ ! -x /usr/bin/sway ]; then
	echo "SWAY-PROBE: fail no-sway-binary"
	echo "SWAY-PROBE: done"
	exit 0
fi
echo "SWAY-PROBE: ok sway-present"

mkdir -p /run/user/0 /etc/sway
cat > /etc/sway/config <<'EOF'
# Nothing but an output: no bar, no keybindings, no clients to start. The point
# is the compositor's own rendering, not a session.
output HEADLESS-1 mode 1280x720
EOF

export XDG_RUNTIME_DIR=/run/user/0
export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_HEADLESS_OUTPUTS=1
export WAYLAND_DISPLAY=wayland-1

echo "SWAY-PROBE: starting sway"
sway -d -c /etc/sway/config > /tmp/sway.log 2>&1 &
SWAY_PID=$!

# Wait for the socket rather than sleeping blind: how long this takes depends
# on the machine, and a fixed sleep is either wasted time or a false failure.
i=0
while [ $i -lt 60 ]; do
	[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && break
	i=$((i + 1))
	sleep 1
done
if [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
	echo "SWAY-PROBE: fail no-socket"
	echo "--- sway log ---"
	tail -40 /tmp/sway.log
	echo "SWAY-PROBE: done"
	exit 0
fi
echo "SWAY-PROBE: ok socket"

# swaymsg finds the IPC socket through SWAYSOCK, and falls back to asking a
# running sway — which needs the socket it does not have yet. Without it every
# command fails with "Unable to retrieve socket path" and the colour never
# reaches the compositor, which is what made the first run's two screenshots
# identical and meaningless.
if [ -z "$SWAYSOCK" ]; then
	SWAYSOCK=$(ls -1 "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
	export SWAYSOCK
fi
echo "SWAY-PROBE: swaysock=${SWAYSOCK:-none}"
echo "--- runtime dir ---"
ls -l "$XDG_RUNTIME_DIR" 2>&1
echo "--- end runtime dir ---"

echo "--- outputs ---"
swaymsg -t get_outputs 2>&1
echo "--- end outputs ---"

shoot() {
	colour="$1"
	name="$2"
	echo "SWAY-PROBE: setting background $colour"
	swaymsg output '*' background "$colour" solid_color 2>&1
	# The compositor renders on its own frame clock; give it a few frames
	# rather than assuming the change is on screen the instant swaymsg returns.
	sleep 3
	if grim "/tmp/$name.png" 2>>/tmp/sway.log; then
		echo "SWAY-PROBE: ok grim-$name $(wc -c < /tmp/$name.png) bytes"
		echo "---PNG-$name---"
		base64 "/tmp/$name.png"
		echo "---PNG-$name-END---"
	else
		echo "SWAY-PROBE: fail grim-$name"
	fi
}

shoot '#ff0000' red
shoot '#00ff00' green

echo "--- sway log ---"
tail -40 /tmp/sway.log
echo "SWAY-PROBE: done"
exit 0
