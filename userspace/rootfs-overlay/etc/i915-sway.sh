#!/bin/sh
# i915-sway.sh — sway on the real display, through the passed-through GPU.
#
# The headless probe (sway-probe.sh) proved sway renders; this asks whether it
# can drive a physical monitor. The difference is the backend: instead of an
# invented output, wlroots opens a DRM device, finds a connector with something
# plugged into it, and page-flips. That path is the imported DRM core plus i915
# from the guest's side, exercised by a compositor rather than by our own
# in-kernel client.
#
# No GPU rendering is involved or expected: the pixman renderer composites in
# software into dumb buffers, which is what a KMS driver has to provide before
# anything harder is worth attempting.
#
# Run from /etc/inittab, and only when the kernel cmdline carries
# b1nix.i915sway: it fetches packages over the network and takes minutes.
grep -q "b1nix.i915sway" /proc/cmdline 2>/dev/null || exit 0

echo "I915-SWAY: start"

export HOME=/root
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mkdir -p /run /tmp /root /run/user/0

echo "--- /dev/dri ---"
ls -l /dev/dri 2>&1
echo "--- end /dev/dri ---"

CARD=/dev/dri/card1
[ -e "$CARD" ] || CARD=/dev/dri/card0
if [ ! -e "$CARD" ]; then
	echo "I915-SWAY: fail no-card"
	echo "I915-SWAY: done"
	exit 0
fi
echo "I915-SWAY: ok card $CARD"

# What libdrm looks at before it will call this a primary node: the device
# numbers on the node itself, and the sysfs entry that says the numbers belong
# to DRM. A missing entry here is what "non-primary DRM FD" actually means.
echo "--- drm identity ---"
ls -l /sys/class/drm/ 2>&1
readlink /sys/class/drm/card1 2>&1
cat /sys/class/drm/card1/uevent 2>&1
cat /sys/devices/gpu1/drm/card1/uevent 2>&1
readlink /sys/devices/gpu1/drm/card1/subsystem 2>&1
ls /sys/devices/gpu1/drm/card1/ 2>&1
ls -l /sys/dev/char/ 2>&1
ls -l /sys/dev/char/226:1/device/ 2>&1
ls -l /sys/class/drm/ 2>&1
cat /sys/class/drm/card1/dev 2>&1
echo "--- end drm identity ---"

# What userspace sees of the card before a compositor touches it: how many
# CRTCs, encoders and connectors the core reports, and whether any connector is
# connected. A compositor with no connectors has no output to enable, which
# looks exactly like a compositor that renders nothing.
if [ -x /bin/m101t_smoke ] && grep -q "b1nix.drm-enumerate" /proc/cmdline 2>/dev/null; then
	echo "--- drm resources ---"
	# No pipe: its output is block-buffered through one, and a test that
	# takes a while then looks like a test that hung.
	B1NIX_CONNECTOR=111 /bin/m101t_smoke 2>&1
	echo "--- end drm resources ---"
fi

# Card-only mode: enumerate the display and stop.
#
# Fetching a compositor and its dependencies costs four minutes of every run,
# and a question about registers, EDID or connectors does not need one. With
# b1nix.drm-probe-only the probe above is the whole boot.
if grep -q "b1nix.drm-probe-only" /proc/cmdline 2>/dev/null; then
	echo "I915-SWAY: probe only, done"
	exit 0
fi

# The package cache disk, when the runner attached one.
#
# Downloading a compositor and its dependencies is the slowest part of a test
# boot by a wide margin, and none of those bytes change between runs. bpkg keeps
# what it fetches here; mounting the disk makes that survive a reboot.
if [ -e /dev/virtio-blk0 ]; then
	mkdir -p /var/cache/bpkg
	if mount -t ext4 /dev/virtio-blk0 /var/cache/bpkg 2>/dev/null; then
		echo "I915-SWAY: package cache mounted ($(ls /var/cache/bpkg | wc -l) files)"
		# The package index too — 2.3 MB of it, downloaded on every boot
		# otherwise. Only the index: the installed-package metadata beside it
		# must not survive, or bpkg would believe packages are present that
		# this boot's ramdisk has never seen.
		mkdir -p /var/lib/bpkg
		ln -sf /var/cache/bpkg/index /var/lib/bpkg/index
	else
		echo "I915-SWAY: package cache not mounted"
	fi
fi

cat > /etc/bpkg.conf <<'EOF'
INDEX_URL=https://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64/APKINDEX.tar.gz https://dl-cdn.alpinelinux.org/alpine/v3.20/community/x86_64/APKINDEX.tar.gz
EOF

# What the image is missing, if anything.
#
# The compositor and its dependencies are installed into the root filesystem at
# build time, from the same Alpine packages this would otherwise fetch. When
# nothing is missing the network is not touched at all — no index, no
# downloads — which is the difference between reaching the compositor in a
# minute and not reaching it inside half an hour.
NEED=""
for pkg in seatd sway swaybg grim foot cage; do
	command -v "$pkg" >/dev/null 2>&1 || NEED="$NEED $pkg"
done
[ -d /usr/share/fonts/dejavu ] || NEED="$NEED font-dejavu"

if [ -n "$NEED" ]; then
	echo "I915-SWAY: missing from the image:$NEED"
	echo "I915-SWAY: updating index"
	if bpkg update; then
		echo "I915-SWAY: ok update"
	else
		echo "I915-SWAY: fail update"
		echo "I915-SWAY: done"
		exit 0
	fi
else
	echo "I915-SWAY: ok update (nothing to fetch)"
fi

# cage is the control: a kiosk compositor on the same wlroots, a fraction of
# sway's code. If it drives the display and sway does not, the fault is sway's
# rather than the platform's.
# font-dejavu because a terminal with no font renders nothing and exits: foot
# connected to the compositor and then died on "failed to match font", which
# reads like a compositor failure and is not one.
for pkg in $NEED; do
	echo "I915-SWAY: installing $pkg"
	if bpkg install "$pkg"; then
		echo "I915-SWAY: ok install-$pkg"
	else
		echo "I915-SWAY: fail install-$pkg"
	fi
	# After each package, not once at the end: a run that is cut off part-way
	# through the list — which is what happens when the download budget runs
	# out — would otherwise leave every byte it fetched dirty in the block
	# cache, and the next run starts from nothing again.
	sync
done

# Push the cache to the disk while there is still something to push it with.
#
# The block cache is write-back and the run ends by killing the machine, so
# everything downloaded above was still dirty in memory when the power went
# out — which is why a cache that had seen thirty packages came back holding
# none of them. Nothing after this point installs anything, so the filesystem
# can be flushed and unmounted here rather than at an exit that never runs.
if mountpoint -q /var/cache/bpkg 2>/dev/null || grep -q " /var/cache/bpkg " /proc/mounts 2>/dev/null; then
	sync
	if umount /var/cache/bpkg 2>/dev/null; then
		echo "I915-SWAY: package cache flushed"
	else
		echo "I915-SWAY: package cache still busy, synced only"
	fi
fi

# Fontconfig has no cache on a freshly built image, and a compositor that
# cannot resolve a font does not draw text.
if command -v fc-cache >/dev/null 2>&1; then
	fc-cache -f >/dev/null 2>&1 || echo "I915-SWAY: fc-cache failed"
fi

# The font path, step by step, when a compositor complains about a font.
#
# "file not found" from cairo can mean a pattern that matched nothing, a path
# that cannot be opened, or a pango object that was never given the pattern at
# all — three different faults with one message. b1nix.font-debug walks the
# same three steps in a process of its own, so the answer is a comparison
# rather than a guess. Off by default: it costs a second and prints a lot.
if grep -q "b1nix.font-debug" /proc/cmdline 2>/dev/null; then
	echo "--- fonts ---"
	fc-match monospace 2>&1
	ls -l "$(fc-match -f '%{file}' monospace 2>/dev/null)" 2>&1
	[ -x /bin/m51_freetype_smoke ] &&
		/bin/m51_freetype_smoke "$(fc-match -f '%{file}' monospace 2>/dev/null)" 2>&1
	[ -x /bin/font_probe ] && /bin/font_probe monospace "monospace 10" 2>&1
	echo "--- end fonts ---"
fi

[ -x /usr/bin/sway ] || { echo "I915-SWAY: fail no-sway-binary"; echo "I915-SWAY: done"; exit 0; }

mkdir -p /etc/sway
# Clients for the desktop. Our own gclock and gpaint speak b1gui, this system's
# native display protocol, and cannot talk to a Wayland compositor at all — so
# the ones that go on screen are the Wayland programs we do have, plus Alpine's
# terminal. What matters is that something other than the compositor renders.
# Clients are launched only when the cmdline asks for them. sway spawns every
# one of these through posix_spawn, and a compositor that dies while starting a
# client tells us nothing about whether it can drive a display — the two
# questions get separated by b1nix.sway-clients.
: > /etc/sway/config
if grep -q "b1nix.sway-clients" /proc/cmdline 2>/dev/null; then
	{
		# Saturated colour and large text, for the same reason cage's run
		# uses them: the proof that the picture is on the panel is a
		# photograph, and a photograph of dark grey at an angle proves
		# nothing. b1nix.bright asks for the readable version.
		if grep -q "b1nix.bright" /proc/cmdline 2>/dev/null; then
			echo 'output * bg #00cc44 solid_color'
			echo 'exec /usr/bin/foot -o colors.background=00cc44 -o colors.foreground=101010 -o main.font=monospace:size=28 top'
		else
			echo 'output * bg #2060c0 solid_color'
			for client in /bin/m51_cairo_wayland /bin/m49_libwayland /usr/bin/foot; do
				[ -x "$client" ] && echo "exec $client"
			done
		fi
	} > /etc/sway/config
fi
echo "--- sway config ---"
cat /etc/sway/config
echo "--- end sway config ---"

export XDG_RUNTIME_DIR=/run/user/0
# WAYLAND_DISPLAY is deliberately NOT set. It names a display to connect to, and
# wlroots reads it to decide it is nested — with it exported, the compositor
# ignored the GPU entirely and tried to open a remote display that does not
# exist. The socket sway creates is discovered below instead.
unset WAYLAND_DISPLAY
# No WLR_BACKENDS, and no WLR_DRM_DEVICES: the compositor is expected to find
# the card the way it does on any other system — by enumerating /sys/class/drm.
# Naming the device by hand would hide a broken discovery path, and the next
# compositor would not have the same escape hatch.
#
# The renderer is the one thing still chosen here: there is no GL driver for
# this hardware yet, so pixman composites in software.
export WLR_RENDERER=pixman
# Naming the card by hand, for the one experiment that needs both GPUs present:
# the kernel's own client mirrors onto the emulated one, and the compositor must
# still be pointed at the assigned one. Ordinary runs leave this unset and rely
# on discovery.
if grep -q "b1nix.wlr-card" /proc/cmdline 2>/dev/null; then
	export WLR_DRM_DEVICES=$CARD
	export WLR_BACKENDS=drm
fi
# The atomic path is the modern one and the one we want; b1nix.legacy-modeset
# asks wlroots for the older sequence instead, which programs the hardware
# through a different set of ioctls. A picture under one and not the other says
# which half of our implementation is at fault.
if grep -q "b1nix.legacy-modeset" /proc/cmdline 2>/dev/null; then
	export WLR_DRM_NO_ATOMIC=1
fi
# No input devices are wired through to the guest, and wlroots refuses to start
# a session that found none unless told that is expected.
export WLR_LIBINPUT_NO_DEVICES=1
# Alpine builds libseat without its "builtin" backend, so the only way a
# compositor gets a device opened is through seatd. Run it detached from the
# virtual terminals: SEATD_VTBOUND=0 drops the VT_ACTIVATE/KDSETMODE dance,
# which belongs to a machine where a user switches consoles and has nothing to
# do with a headless boot driving one output.
export LIBSEAT_BACKEND=seatd
export SEATD_VTBOUND=0
SEATD_VTBOUND=0 seatd -g root > /tmp/seatd.log 2>&1 &
i=0
while [ $i -lt 20 ] && [ ! -S /run/seatd.sock ]; do i=$((i + 1)); sleep 1; done
if [ -S /run/seatd.sock ]; then
	echo "I915-SWAY: ok seatd"
else
	echo "I915-SWAY: fail seatd"
	tail -20 /tmp/seatd.log
fi
export XDG_SESSION_TYPE=wayland

# Straight to the console, and through no pipe: when the compositor takes the
# machine down with it a log printed afterwards never appears, and a pipe holds
# whole blocks of it back — which is how the connector scan came to be missing
# from a run that had certainly performed it.
if grep -q "b1nix.use-cage" /proc/cmdline 2>/dev/null && [ -x /usr/bin/cage ]; then
	# A frame a camera can actually read, when asked for one.
	#
	# The default client is a terminal at a shell prompt: light grey text on
	# near-black, a few hundred pixels of it. Photographed in a dark room off a
	# panel that is out of focus, that is indistinguishable from a monitor
	# showing nothing at all — which is exactly the question these runs exist to
	# answer. b1nix.bright fills the screen with saturated colour and runs top,
	# so the photograph shows a large solid field that also changes between
	# frames, and neither ambiguity survives.
	CLIENT="/usr/bin/foot"
	if grep -q "b1nix.bright" /proc/cmdline 2>/dev/null; then
		set -- -o colors.background=00cc44 -o colors.foreground=101010 \
		       -o main.font=monospace:size=28 top
	else
		set --
	fi
	echo "I915-SWAY: starting cage on $CARD"
	# Into the same file sway writes, rather than inherited stdout: a
	# background job started from inittab does not have the serial console on
	# its descriptors, and the whole of the compositor's debug output — which
	# is where it says which modes it saw and which one it chose — was going
	# nowhere. The interesting part is dumped below, the rest at the end.
	cage -d -- "$CLIENT" "$@" > /tmp/sway.log 2>&1 &
else
	# Without the GPU, on request.
	#
	# The headless backend skips DRM, the seat and input entirely, which is how
	# a fault in the compositor gets separated from a fault in the display
	# path — and it runs under plain QEMU, so the loop is minutes rather than
	# a passthrough boot.
	if grep -q "b1nix.sway-headless" /proc/cmdline 2>/dev/null; then
		export WLR_BACKENDS=headless
		echo "I915-SWAY: headless isolation run"
	fi
	echo "I915-SWAY: starting sway on $CARD"
	sway -d -c /etc/sway/config > /tmp/sway.log 2>&1 &
fi
SWAY_PID=$!

# The compositor faults inside a shared library, and a fault address alone says
# nothing: the loader places everything above 0x700000000000, so the same number
# could be libc or wlroots. The map turns the address into a file and an offset,
# which the host can resolve to a function.
sleep 3
echo "--- sway maps ---"
cat "/proc/$SWAY_PID/maps" 2>&1
echo "--- end sway maps ---"

# What the compositor made of the connector: the modes it kept and the one it
# picked. The kernel's side of that conversation is already in this log, and
# the two together are what say whether a wrong mode was chosen or offered.
echo "--- compositor log ($(wc -c < /tmp/sway.log 2>/dev/null) bytes) ---"
head -120 /tmp/sway.log 2>&1
echo "--- end compositor log ---"

i=0
while [ $i -lt 90 ]; do
	WAYLAND_DISPLAY=$(ls -1 "$XDG_RUNTIME_DIR" 2>/dev/null |
	                  grep '^wayland-[0-9]*$' | head -1)
	[ -n "$WAYLAND_DISPLAY" ] && [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && break
	i=$((i + 1))
	sleep 1
done
export WAYLAND_DISPLAY
if [ -z "$WAYLAND_DISPLAY" ] || [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
	echo "I915-SWAY: fail no-socket"
	echo "--- sway log ---"
	tail -200 /tmp/sway.log
	echo "I915-SWAY: done"
	exit 0
fi
echo "I915-SWAY: ok socket"

if [ -z "$SWAYSOCK" ]; then
	SWAYSOCK=$(ls -1 "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
	export SWAYSOCK
fi

echo "--- runtime dir ---"
ls -l "$XDG_RUNTIME_DIR" 2>&1
echo "--- end runtime dir ---"

echo "--- outputs ---"
swaymsg -t get_outputs 2>&1
echo "--- end outputs ---"

# The monitor is watched by a camera on the host, so hold the picture long
# enough to be photographed rather than only screenshotted.
echo "I915-SWAY: holding the desktop"
sleep 60

echo "--- clients ---"
swaymsg -t get_tree 2>&1 | head -60
echo "--- end clients ---"

if grim /tmp/desktop.png 2>>/tmp/sway.log; then
	echo "I915-SWAY: ok grim $(wc -c < /tmp/desktop.png) bytes"
	echo "---PNG-desktop---"
	base64 /tmp/desktop.png
	echo "---PNG-desktop-END---"
else
	echo "I915-SWAY: fail grim"
fi

echo "--- sway log ---"
tail -200 /tmp/sway.log
echo "I915-SWAY: done"
exit 0
