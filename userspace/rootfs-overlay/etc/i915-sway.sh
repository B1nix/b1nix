#!/bin/sh

# i915-sway.sh — sway on the real display, through the passed-through GPU.
#
# The headless probe (sway-probe.sh) proved sway renders; this asks whether it
# can drive a physical monitor. Instead of an invented output, wlroots opens a
# DRM device, finds a connector with something plugged into it, and page-flips
# -- the imported DRM core plus i915 from the guest's side, driven by a
# compositor rather than by our own in-kernel client.
#
# No GPU rendering is expected: pixman composites in software into dumb buffers,
# which is what a KMS driver has to provide before anything harder is worth
# attempting.
#
# Run from /etc/inittab and only under b1nix.i915sway: it may fetch packages
# over the network and takes minutes.

# Whole-word flag matching, with an optional value.
#
# `grep -q b1nix.chromium /proc/cmdline` also matches b1nix.chromium-min (the
# dot is a wildcard too), and an exact-token test misses b1nix.glprobe=virtio_gpu
# -- both have silently run the wrong branch here.
has_flag() {
	for tok in $(cat /proc/cmdline 2>/dev/null); do
		[ "$tok" = "$1" ] && return 0
		case "$tok" in "$1"=*) return 0 ;; esac
	done
	return 1
}
# Silence is indistinguishable from "the flag was not passed", so say which of
# the two happened and show what the read returned.
if ! grep -q "b1nix.i915sway" /proc/cmdline 2>/dev/null; then
	echo "I915-SWAY: not requested; /proc/cmdline = [$(cat /proc/cmdline 2>&1)]"
	exit 0
fi

# Every milestone carries the guest's uptime, so a slow boot can be split into
# its parts instead of guessed at.
up() { cut -d" " -f1 /proc/uptime 2>/dev/null || echo "?"; }
echo "I915-SWAY: start t=$(up)"

# A heap-churn check, when asked for one, instead of the compositor: the same
# shape of work with none of the graphics, so a run costs seconds.
if has_flag b1nix.memstress; then
	echo "I915-SWAY: running memstress instead of a compositor"
	/usr/bin/memstress
	echo "I915-SWAY: memstress exit $?"
	[ -r /proc/b1nix-prof ] && cat /proc/b1nix-prof 2>/dev/null
	/usr/bin/shmstress
	echo "I915-SWAY: shmstress exit $?"
	/usr/bin/fdstress
	echo "I915-SWAY: fdstress exit $?"
	/usr/bin/spawnstress
	echo "I915-SWAY: spawnstress exit $?"
	exit 0
fi

export HOME=/root
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mkdir -p /run /tmp /root /run/user/0

# Where a graphics stack looks for a render node, checked rather than assumed.
echo "--- render node visibility ---"
echo "/sys/class/drm:      $(ls /sys/class/drm 2>&1 | tr '\n' ' ')"
echo "/sys/dev/char/226:128: $(ls -l /sys/dev/char/226:128 2>&1 | head -1)"
echo "renderD128 dev attr: $(cat /sys/class/drm/renderD128/dev 2>&1)"
echo "renderD128 uevent:   $(cat /sys/class/drm/renderD128/uevent 2>&1 | tr '\n' ' ')"
echo "open test:           $(dd if=/dev/dri/renderD128 bs=1 count=0 2>&1 | tail -1)"
echo "--- end render node visibility ---"

echo "--- /dev/dri ---"
ls -l /dev/dri 2>&1
echo "--- end /dev/dri ---"

# card1 is the passed-through GPU; card0 is b1nix's own small DRM device.
CARD=/dev/dri/card1
[ -e "$CARD" ] || CARD=/dev/dri/card0
if [ ! -e "$CARD" ]; then
	echo "I915-SWAY: fail no-card"
	echo "I915-SWAY: done"
	exit 0
fi
echo "I915-SWAY: ok card $CARD t=$(up)"

# What userspace sees of the card before a compositor touches it: how many
# CRTCs, encoders and connectors, and whether any connector is connected. A
# compositor with no connectors has no output to enable, which looks exactly
# like a compositor that renders nothing.
# No pipe: m101t_smoke's output is block-buffered through one, and a test that
# takes a while then looks like a test that hung.
if [ -x /bin/m101t_smoke ] && grep -q "b1nix.drm-enumerate" /proc/cmdline 2>/dev/null; then
	echo "--- drm resources ---"
	B1NIX_CONNECTOR=111 /bin/m101t_smoke 2>&1
	echo "--- end drm resources ---"
fi

# Render-only mode: draw off-screen on the GPU and check the pixels. Answers a
# different question from the compositor's -- does the render engine execute
# what Mesa submits? "iris initialised" is printed by a driver that never
# successfully submits a batch too.
#
# Placed before the package stage because it needs nothing from the network:
# Mesa is in the image whenever B1NIX_GPU_DRV=1 built it.
if has_flag b1nix.glprobe; then
	iris=/usr/lib/xorg/modules/dri/iris_dri.so
	if ! grep -q 'b1nix\.glprobe=' /proc/cmdline 2>/dev/null && [ ! -e "$iris" ]; then
		echo "I915-SWAY: fail no-iris (build with B1NIX_GPU_DRV=1)"
		echo "I915-SWAY: done"
		exit 0
	fi
	# Name the driver rather than let the loader search, or Mesa may answer with
	# a software rasteriser and a green run would say nothing about the GPU.
	# b1nix.glprobe=virtio_gpu probes virgl; a bare b1nix.glprobe keeps iris for
	# the passed-through Intel part -- which is why the iris check above is
	# skipped when another driver is named.
	# MESA_DEBUG/LIBGL_DEBUG because without them a client reports one line,
	# "failed to create dri2 screen", for every possible cause.
	drv=$(sed -n 's/.*b1nix\.glprobe=\([A-Za-z0-9_]*\).*/\1/p' /proc/cmdline)
	export MESA_LOADER_DRIVER_OVERRIDE="${drv:-iris}"
	export LIBGL_DRIVERS_PATH=/usr/lib/xorg/modules/dri
	export EGL_LOG_LEVEL=debug
	export MESA_DEBUG=1
	export VIRGL_DEBUG=verbose
	export LIBGL_DEBUG=verbose
	# What Mesa reads to identify the device: drmGetDevice2 parses these, and a
	# device it cannot identify is one pipe_loader will not make a screen for --
	# which looks from outside exactly like a driver that refused.
	for f in vendor device revision subsystem_vendor subsystem_device; do
		echo "I915-SWAY: pci $f = [$(cat /sys/class/drm/card1/device/$f 2>&1)]"
	done
	echo "I915-SWAY: pci link = [$(readlink /sys/class/drm/card1/device 2>&1)]"
	echo "I915-SWAY: bus dir = [$(ls /sys/bus/pci/devices 2>&1 | tr '\n' ' ')]"
	echo "I915-SWAY: gl probe on $CARD t=$(up)"
	/bin/gl_probe /dev/dri/renderD128
	echo "I915-SWAY: gl_probe exit $? t=$(up)"
	echo "I915-SWAY: done"
	exit 0
fi

# Card-only mode: enumerate the display and stop. Fetching a compositor costs
# four minutes of every run, and a question about registers, EDID or connectors
# does not need one.
if grep -q "b1nix.drm-probe-only" /proc/cmdline 2>/dev/null; then
	echo "I915-SWAY: probe only, done"
	exit 0
fi

# The package cache disk, when the runner attached one. None of these bytes
# change between runs, and downloading them is the slowest part of a test boot.
if [ -e /dev/vda ]; then
	mkdir -p /var/cache/bpkg
	if mount -t ext4 /dev/vda /var/cache/bpkg 2>/dev/null; then
		echo "I915-SWAY: package cache mounted ($(ls /var/cache/bpkg | wc -l) files)"
		# The index too -- 2.3 MB otherwise fetched every boot. Only the index:
		# the installed-package metadata beside it must not survive, or bpkg would
		# believe packages are present that this boot's ramdisk has never seen.
		mkdir -p /var/lib/bpkg
		ln -sf /var/cache/bpkg/index /var/lib/bpkg/index
	else
		echo "I915-SWAY: package cache not mounted"
	fi
fi

# http, not https -- the same choice apk makes. Every package carries an RSA
# signature over a control block carrying the payload's sha256, so transport
# encryption adds nothing and costs a great deal: the cipher runs in software on
# one core in front of a quarter-gigabyte download.
cat > /etc/bpkg.conf <<'EOF'
INDEX_URL=http://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64/APKINDEX.tar.gz http://dl-cdn.alpinelinux.org/alpine/v3.20/community/x86_64/APKINDEX.tar.gz
EOF

# What the image is missing, if anything. The compositor and its dependencies
# are installed into the root filesystem at build time from the same Alpine
# packages; when nothing is missing the network is not touched at all, which is
# the difference between a minute and half an hour.
#
# cage is the control: a kiosk compositor on the same wlroots, a fraction of
# sway's code. font-dejavu because foot dies on "failed to match font", which
# reads like a compositor failure and is not one.
NEED=""
for pkg in seatd sway swaybg grim foot cage; do
	command -v "$pkg" >/dev/null 2>&1 || NEED="$NEED $pkg"
done
# A real browser, when asked for one: 113 MB to download and 248 MB installed,
# so it is never in the image unless b1nix.chromium says so.
if has_flag b1nix.chromium; then
	command -v chromium >/dev/null 2>&1 || NEED="$NEED chromium"
fi
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
	echo "I915-SWAY: ok update (nothing to fetch) t=$(up)"
fi

for pkg in $NEED; do
	echo "I915-SWAY: installing $pkg"
	if bpkg install "$pkg"; then
		echo "I915-SWAY: ok install-$pkg"
	else
		echo "I915-SWAY: fail install-$pkg"
	fi
	# Flush every eighth package, not every one: a run cut off part-way must not
	# lose what it fetched, but each sync writes the whole dirty block cache.
	pkg_n=$((${pkg_n:-0} + 1))
	[ $((pkg_n % 8)) -eq 0 ] && sync
done

# Push the cache to the disk while there is still something to push it with:
# the block cache is write-back and the run ends by killing the machine, so a
# cache that had seen thirty packages came back holding none of them. Nothing
# after this point installs anything.
if mountpoint -q /var/cache/bpkg 2>/dev/null || grep -q " /var/cache/bpkg " /proc/mounts 2>/dev/null; then
	sync
	if umount /var/cache/bpkg 2>/dev/null; then
		echo "I915-SWAY: package cache flushed"
	else
		echo "I915-SWAY: package cache still busy, synced only"
	fi
fi

# Fontconfig has no cache on a freshly built image, and a compositor that cannot
# resolve a font does not draw text.
if command -v fc-cache >/dev/null 2>&1; then
	fc-cache -f >/dev/null 2>&1 || echo "I915-SWAY: fc-cache failed"
fi

# The font path, step by step. "file not found" from cairo can mean a pattern
# that matched nothing, a path that cannot be opened, or a pango object that was
# never given the pattern -- three faults with one message. Off by default: it
# costs a second and prints a lot.
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
# Clients are launched only when the cmdline asks for them: sway spawns each one
# through posix_spawn, and a compositor that dies while starting a client says
# nothing about whether it can drive a display.
: > /etc/sway/config
	# Window appearance taken from a working desktop's sway config and cut down to
	# what plain sway understands -- a visible border and a named colour make a
	# photograph of the panel say which window is which. The browser is started
	# below by this script, so its output is ours rather than sway's.
if has_flag b1nix.chromium && [ -x /usr/bin/chromium ]; then
	{
		echo 'output * bg #202020 solid_color'
		echo 'default_border pixel 2'
		echo 'client.focused          #7aa2f7 #1a1b26 #c0caf5 #bb9af7 #7aa2f7'
		echo 'client.unfocused        #101014 #16161e #a9b1d6 #101014 #101014'
		echo 'gaps inner 5'
		: # the browser is started below, by this script, so its output is ours
	} > /etc/sway/config
	# Saturated colour and large text under b1nix.bright: the proof is a
	# photograph, and a photograph of dark grey at an angle proves nothing.
	# b1nix.client-nopty asks for a client that opens no pseudo-terminal, and
	# b1nix.client-quiet for the same terminal writing nothing -- which separates
	# the pty path from everything else.
elif has_flag b1nix.sway-clients; then
	{
		if grep -q "b1nix.bright" /proc/cmdline 2>/dev/null; then
			echo 'output * bg #00cc44 solid_color'
			if has_flag b1nix.client-nopty; then
				echo 'exec /usr/bin/swaybg -c "#00cc44"'
			elif has_flag b1nix.client-quiet; then
				echo 'exec /usr/bin/foot -o colors.background=00cc44 -o main.font=monospace:size=28 sleep 600'
			else
				echo 'exec /usr/bin/foot -o colors.background=00cc44 -o colors.foreground=101010 -o main.font=monospace:size=28 top'
			fi
		else
			echo 'output * bg #2060c0 solid_color'
			echo 'exec /usr/bin/foot'
		fi
	} > /etc/sway/config
fi
echo "--- sway config ---"
cat /etc/sway/config
echo "--- end sway config ---"

export XDG_RUNTIME_DIR=/run/user/0
# WAYLAND_DISPLAY is deliberately NOT set: wlroots reads it to decide it is
# nested, and with it exported the compositor ignored the GPU entirely. The
# socket sway creates is discovered below instead.
unset WAYLAND_DISPLAY
# No WLR_BACKENDS and no WLR_DRM_DEVICES: the compositor finds the card the way
# it does on any other system, by enumerating /sys/class/drm. Naming the device
# by hand would hide a broken discovery path.
#
# The renderer comes from /etc/render-select.sh, which every compositor launcher
# here shares. Software unless b1nix.i915gl asks otherwise: a run that fails on
# pixman says something about KMS, one that fails on GLES says something about
# Mesa, and the default keeps those apart.
. /etc/render-select.sh
if grep -q "b1nix.i915gl" /proc/cmdline 2>/dev/null; then
	B1NIX_MESA_DRIVER=iris
	export EGL_LOG_LEVEL=debug
	export MESA_DEBUG=1
else
	B1NIX_RENDERER=pixman
fi
render_select
unset B1NIX_MESA_DRIVER B1NIX_RENDERER
echo "I915-SWAY: renderer $WLR_RENDERER ($RENDER_REASON)"
if [ "$WLR_RENDERER" != gles2 ] && grep -q "b1nix.i915gl" /proc/cmdline 2>/dev/null; then
	echo "I915-SWAY: fail no-accel ($RENDER_REASON; build with B1NIX_GPU_DRV=1)"
fi
# Naming the card by hand, for the one experiment that needs both GPUs present:
# the kernel's own client mirrors onto the emulated one and the compositor must
# still be pointed at the assigned one. Ordinary runs rely on discovery.
if grep -q "b1nix.wlr-card" /proc/cmdline 2>/dev/null; then
	export WLR_DRM_DEVICES=$CARD
	export WLR_BACKENDS=drm
fi
# The atomic path is the one we want; b1nix.legacy-modeset asks wlroots for the
# older ioctl sequence. A picture under one and not the other says which half of
# our implementation is at fault.
if grep -q "b1nix.legacy-modeset" /proc/cmdline 2>/dev/null; then
	export WLR_DRM_NO_ATOMIC=1
fi
# No input devices are passed to the guest, and wlroots refuses to start a
# session that found none unless told that is expected.
export WLR_LIBINPUT_NO_DEVICES=1
# Alpine builds libseat without its "builtin" backend, so seatd is the only way
# a compositor gets a device opened. SEATD_VTBOUND=0 drops the
# VT_ACTIVATE/KDSETMODE dance, which belongs to a machine where a user switches
# consoles.
export LIBSEAT_BACKEND=seatd
export SEATD_VTBOUND=0
SEATD_VTBOUND=0 seatd -g root > /tmp/seatd.log 2>&1 &
i=0
while [ $i -lt 20 ] && [ ! -S /run/seatd.sock ]; do i=$((i + 1)); sleep 1; done
if [ -S /run/seatd.sock ]; then
	echo "I915-SWAY: ok seatd t=$(up)"
else
	echo "I915-SWAY: fail seatd"
	tail -20 /tmp/seatd.log
fi
export XDG_SESSION_TYPE=wayland
# What a desktop session tells its clients about itself: Chromium reads
# XDG_CURRENT_DESKTOP when deciding how to behave on Wayland, and a session that
# names none is not a case it is written for.
export XDG_CURRENT_DESKTOP=sway
export XDG_SESSION_DESKTOP=sway
export GDK_BACKEND=wayland
export QT_QPA_PLATFORM=wayland

# The compositor's own output goes to a file, not to inherited stdout: a
# background job started from inittab does not have the serial console on its
# descriptors, and the whole of the debug output -- which modes it saw and which
# it chose -- was going nowhere. The interesting part is dumped below.
if grep -q "b1nix.use-cage" /proc/cmdline 2>/dev/null && [ -x /usr/bin/cage ]; then
	CLIENT="/usr/bin/foot"
	# b1nix.bright fills the screen with saturated colour and runs top, so the
	# photograph shows a large solid field that also changes between frames.
	# The default client is grey-on-black text, which photographed off a panel in
	# a dark room is indistinguishable from a monitor showing nothing.
	if grep -q "b1nix.bright" /proc/cmdline 2>/dev/null; then
		set -- -o colors.background=00cc44 -o colors.foreground=101010 \
		       -o main.font=monospace:size=28 top
	else
		set --
	fi
	echo "I915-SWAY: starting cage on $CARD"
	cage -d -- "$CLIENT" "$@" > /tmp/sway.log 2>&1 &
else
	# Without the GPU, on request: the headless backend skips DRM, the seat and
	# input entirely, which separates a fault in the compositor from one in the
	# display path -- and it runs under plain QEMU. One virtual output, because a
	# headless compositor with no output has nowhere to put a window.
	if grep -q "b1nix.sway-headless" /proc/cmdline 2>/dev/null; then
		export WLR_BACKENDS=headless
		export WLR_HEADLESS_OUTPUTS=1
		echo "I915-SWAY: headless isolation run (virtual output 1920x1080)"
	fi
	echo "I915-SWAY: starting sway on $CARD t=$(up)"
	sway -d -c /etc/sway/config > /tmp/sway.log 2>&1 &
fi
SWAY_PID=$!

# The compositor faults inside a shared library, and a fault address alone says
# nothing: the loader places everything above 0x700000000000. The map turns the
# address into a file and an offset the host can resolve to a function.
sleep 3
echo "--- sway maps ---"
cat "/proc/$SWAY_PID/maps" 2>&1
echo "--- end sway maps ---"

# What the compositor made of the connector: the modes it kept and the one it
# picked. The kernel's side of that conversation is already in this log.
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
echo "I915-SWAY: ok socket t=$(up)"
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

if has_flag b1nix.chromium; then
	# A page built to be photographed: saturated colour fills the screen, the text
	# says which machine drew it, and a counter driven by the page's own script
	# changes between frames -- so a photograph shows the browser rendering rather
	# than a still image left over from something else.
	cat > /root/browser-proof.html <<'PROOF'
<!doctype html>
<meta charset="utf-8">
<title>b1nix</title>
<style>
  html,body{margin:0;height:100%;background:#00b04a;color:#0b0b0b;
            font-family:monospace;display:flex;flex-direction:column;
            align-items:center;justify-content:center}
  h1{font-size:6vw;margin:0}
  p{font-size:3vw;margin:2vh 0 0}
  #t{font-size:8vw;margin:3vh 0 0}
</style>
<h1>chromium on b1nix</h1>
<p id="ua"></p>
<div id="t">0</div>
<script>
  document.getElementById('ua').textContent = navigator.platform;
  let n = 0;
  setInterval(() => { document.getElementById('t').textContent = ++n; }, 1000);
</script>
PROOF
	# The control client (foot) binds fifteen globals and creates a toplevel on
	# this same socket, which is what established that the compositor path works
	# and the browser's silence is its own. Not run every time: it costs a fifth of
	# the boot and has twice hung and taken the run with it.
	if grep -q "b1nix.wayland-control" /proc/cmdline 2>/dev/null; then
		echo "I915-SWAY: control client (foot) t=$(up)"
		( WAYLAND_DEBUG=1 timeout -k 5 20 /usr/bin/foot true > /var/foot.log 2>&1
		  echo "FOOT-RUN-EXIT rc=$?" >> /var/foot.log )
		echo "--- control client protocol ---"
		for req in "\.bind(" "create_surface" "get_toplevel"; do
			echo "  $req: $(grep -ac "$req" /var/foot.log 2>/dev/null)"
		done
		# The globals it took, by NAME NUMBER: the browser binds everything numbered
		# 10 and up and nothing below it, a boundary in arrival order rather than in
		# the interfaces themselves. A client that takes 1 and 2 here proves the
		# early events are delivered.
		echo "  bound by number: $(grep -ao 'bind([0-9]*, "[a-z_0-9]*"' /var/foot.log 2>/dev/null |
			sed 's/bind(//; s/, "/:/; s/"//' | sort -n | tr '\n' ' ')"
		echo "--- end control client protocol ---"

		# A real window, held open, and a picture of it. A terminal kept open is a
		# window on the same output the browser was asked to draw on, and grim reads
		# back what was actually composited: a PNG with content proves the display
		# path end to end.
		( WAYLAND_DEBUG=0 /usr/bin/foot -- /bin/sleep 300 > /var/foot-win.log 2>&1 & )
		win_seen=""
		wt=0
		while [ "$wt" -lt 60 ]; do
			if timeout -k 2 5 swaymsg -t get_tree 2>/dev/null |
					grep -q '"app_id": *"foot"'; then
				win_seen="yes"
				break
			fi
			sleep 3
			wt=$((wt + 3))
		done
		if [ -n "$win_seen" ]; then
			echo "I915-SWAY: ok control-window (foot mapped after ${wt}s)"
			if grim /var/window.png 2>/var/grim.log; then
				echo "I915-SWAY: ok screenshot ($(wc -c < /var/window.png) bytes)"
				echo "  non-black check: $(od -An -tx1 -N 4096 /var/window.png 2>/dev/null |
					tr -d ' \n' | tr -s '0' '0' | wc -c) bytes of header entropy"
			else
				echo "I915-SWAY: FAIL screenshot — $(head -1 /var/grim.log 2>/dev/null)"
			fi
		else
			echo "I915-SWAY: FAIL control-window — no foot window after ${wt}s"
		fi
	fi

	# Shared memory, checked before the browser needs it: every Wayland frame is a
	# wl_shm buffer over a file both sides map, and "it cannot make a buffer" and
	# "it decided not to draw" look identical from outside.
	mkdir -p /dev/shm
	if : > /dev/shm/probe 2>/dev/null && dd if=/dev/zero of=/dev/shm/probe bs=4096 count=1 2>/dev/null; then
		echo "I915-SWAY: shm ok ($(ls -l /dev/shm/probe 2>&1 | awk '{print $5}') bytes)"
		rm -f /dev/shm/probe
	else
		echo "I915-SWAY: shm FAIL — a client cannot allocate a frame buffer"
	fi

	# Is /proc/<pid>/task readable, and does it agree with itself? Chromium's
	# sandbox counts the entries to decide whether the process is single-threaded.
	# Read it twice from a process whose thread count is known and compare with
	# Threads:.
	echo "I915-SWAY: task dir — self=$(ls /proc/self/task 2>&1 | wc -l) again=$(ls /proc/self/task 2>&1 | wc -l) Threads=$(grep -a '^Threads:' /proc/self/status 2>/dev/null | tr -d '\t ' | sed 's/Threads://')"
	swp=$(pidof sway 2>/dev/null | cut -d' ' -f1)
	if [ -n "$swp" ]; then
		echo "I915-SWAY: task dir — sway=$(ls /proc/$swp/task 2>&1 | wc -l) again=$(ls /proc/$swp/task 2>&1 | wc -l) Threads=$(grep -a '^Threads:' /proc/$swp/status 2>/dev/null | tr -d '\t ' | sed 's/Threads://')"
	fi

	# Does time pass correctly here? The browser is a queue of delayed tasks, and a
	# clock that jumps, stalls or runs at the wrong rate stops it dead while every
	# other subsystem looks healthy.
	t_a=$(date +%s)
	u_a=$(cut -d" " -f1 /proc/uptime)
	sleep 5
	t_b=$(date +%s)
	u_b=$(cut -d" " -f1 /proc/uptime)
	echo "I915-SWAY: clock check — wall $((t_b - t_a))s, uptime $u_a -> $u_b (expect 5s each)"
	m_a=$(date +%s%N 2>/dev/null)
	m_b=$(date +%s%N 2>/dev/null)
	echo "I915-SWAY: monotonic pair $m_a $m_b"
	# Resolution from the kernel, not from `date`: busybox's date has no %N, so
	# ten readings of it measure seconds and say nothing about the clock underneath.
	echo "I915-SWAY: clock source: $(grep -a -m1 'monotonic' /proc/timer_list 2>/dev/null ||
		echo '(no /proc/timer_list)')"

	# The size the browser actually asks for: its field-trial state is one 256 KiB
	# shared region created before any window exists, and the code that creates it
	# treats failure as fatal. A 4 KiB probe passing says nothing about it.
	if dd if=/dev/zero of=/dev/shm/probe256 bs=1024 count=256 2>/dev/null; then
		echo "I915-SWAY: shm 256K ok ($(ls -l /dev/shm/probe256 2>&1 | awk '{print $5}') bytes)"
		rm -f /dev/shm/probe256
	else
		echo "I915-SWAY: shm 256K FAIL — the size the field-trial allocator needs"
	fi

	# Does the browser work with no window in the way? Headless removes the window
	# layer from the question: a screenshot means the engine, the processes and the
	# IPC are fine and only the Ozone path is not.
	# A Chromium CHECK prints the file and condition that failed and then unwinds,
	# so pick the failure out rather than showing the last lines.
	if grep -q "b1nix.headless-probe" /proc/cmdline 2>/dev/null; then
	echo "I915-SWAY: headless probe t=$(up)"
	( timeout -k 5 90 /usr/bin/chromium --headless --no-sandbox --no-zygote \
	      --disable-gpu --user-data-dir=/tmp/chromium-headless \
	      --screenshot=/tmp/shot.png --window-size=800,600 \
	      file:///root/browser-proof.html > /var/headless.log 2>&1
	  echo "HEADLESS-EXIT rc=$?" >> /var/headless.log )
	if [ -s /tmp/shot.png ]; then
		echo "I915-SWAY: headless ok — $(ls -l /tmp/shot.png | awk '{print $5}') byte screenshot"
	else
		echo "I915-SWAY: headless FAIL — no screenshot"
		grep -aiE "FATAL|CHECK failed|DCHECK|Aborted|assert|NOTREACHED|out of memory|bad_alloc" \
			/var/headless.log 2>/dev/null | head -10
		echo "  --- last lines ---"
		grep -av "bus\.cc" /var/headless.log 2>/dev/null | tail -14
	fi
	fi

	# The same page through the same engine with no display server at all.
	# Answered: the browser hangs identically with no compositor in the picture,
	# which is what sent the search into mmap and found the VMA-list race. Kept
	# behind a flag as the cheapest way to ask again, off by default because it
	# costs four minutes.
	if grep -q "b1nix.headless-browser" /proc/cmdline 2>/dev/null; then
	echo "I915-SWAY: headless check t=$(up)"
	timeout -k 5 600 /usr/bin/chromium --headless --no-sandbox --no-zygote \
		--disable-gpu --disable-dev-shm-usage \
		--user-data-dir=/tmp/chromium-headless \
		--virtual-time-budget=5000 --dump-dom \
		file:///root/browser-proof.html > /var/chromium-headless.log 2>&1
	echo "I915-SWAY: headless rc=$? t=$(up)"
	echo "  headless output (first 6 lines):"
	head -6 /var/chromium-headless.log 2>/dev/null | sed 's/^/    /'
	echo "  headless bytes: $(wc -c < /var/chromium-headless.log 2>/dev/null)"
	fi

	# The smallest case that still fails -- one process, a blank page, no window
	# system -- left alone in silence so the kernel's watchdog dumps its threads.
	# The dump only fires after a minute with nothing on the console. Only under
	# b1nix.task-watch: without a watchdog to feed, the two minutes this costs are
	# two minutes the run does not have.
	if has_flag b1nix.task-watch; then
	echo "I915-SWAY: minimal start t=$(up)"
	( timeout -k 5 150 /usr/bin/chromium --headless=new --no-sandbox \
		--single-process --disable-gpu --disable-dev-shm-usage \
		--user-data-dir=/tmp/chromium-min \
		--dump-dom about:blank > /var/min.log 2>&1 & )

	sleep 95

	# Is it burning the processor, or waiting? Consumed processor time sampled
	# twice over a known interval says plainly which; the scheduler counter in the
	# task dump is an internal value and a poor thing to conclude from.
	pid=$(ls -d /proc/[0-9]* 2>/dev/null | while read -r d; do
		case "$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null)" in
		*chromium-min*) echo "${d#/proc/}"; break ;;
		esac
	done)
	if [ -n "$pid" ]; then
		u1=$(awk '{print $14, $15}' /proc/$pid/stat 2>/dev/null)
		sleep 10
		u2=$(awk '{print $14, $15}' /proc/$pid/stat 2>/dev/null)
		echo "I915-SWAY: minimal cpu pid=$pid over 10s: [$u1] -> [$u2]"
		echo "I915-SWAY: minimal threads=$(ls /proc/$pid/task 2>/dev/null | wc -l)"
	else
		echo "I915-SWAY: minimal cpu — process not found in /proc"
	fi

	echo "I915-SWAY: minimal after-quiet t=$(up) dom=$(grep -ac '<html' /var/min.log 2>/dev/null)"
	pkill -f "chromium-min" 2>/dev/null
	fi

	# The display name is the one discovered above, not a guess: sway names its
	# socket wayland-0 or wayland-1 depending on what else asked first.
	#
	# Into a file, not onto the console: at --v=1 the browser writes thousands of
	# lines through a 115200-baud serial line and the machine spends its startup
	# inside the UART instead of laying out a page. WAYLAND_DEBUG puts the protocol
	# trace in the same file -- when a client stops without a word, that trace is
	# what says whether it ever asked for a surface.
	#
	# --in-process-gpu and --no-zygote: the separate GPU process died here before
	# any window existed (it looks for a DRM render node, then VA-API on it), and
	# the zygote forks with clone(SIGCHLD, ...) which this kernel does not
	# implement. VaapiVideoDecoder is off for the same reason: no device to open.
	echo "I915-SWAY: starting chromium on $WAYLAND_DISPLAY t=$(up)"
	(
		WAYLAND_DEBUG=1 \
		/usr/bin/chromium \
			--ozone-platform=wayland --no-sandbox --no-zygote \
			--no-first-run --no-default-browser-check \
			--disable-sync --disable-background-networking \
			--disable-component-update --disable-domain-reliability \
			--metrics-recording-only --disable-breakpad \
			--disable-gpu --use-gl=swiftshader \
			--disable-gpu-compositing \
			--in-process-gpu \
			--single-process \
			--disable-dev-shm-usage --user-data-dir=/tmp/chromium-profile \
			--disable-accelerated-video-decode --disable-accelerated-video-encode \
			--ozone-platform-hint=wayland \
			--window-size=1280,800 --enable-logging=stderr \
			--vmodule=*wayland*=3,*ozone*=3,*platform_window*=3 \
			--disable-features=VaapiVideoDecoder,VaapiVideoEncoder,VaapiIgnoreDriverChecks,WaylandFractionalScaleV1 \
			--v=1 \
			--app=file:///root/browser-proof.html > /var/chromium.log 2>&1
		echo "CHROMIUM-EXIT rc=$?"
		echo "--- chromium log (tail) ---"
		tail -40 /var/chromium.log 2>/dev/null
		echo "--- end chromium log ---"
	) &

	# Who the browser's children are, while they are still alive. The one child it
	# starts dies after fifteen seconds and is a zombie with an empty cmdline by
	# the time anything looks; which process it was decides the diagnosis. The
	# interesting window is the first half minute.
	(
		n=0
		while [ "$n" -lt 30 ]; do
			for p in /proc/[0-9]*; do
				c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
				case "$c" in
					*--type=*) echo "CHILD ${p#/proc/}: $(echo "$c" |
						grep -oE '\-\-type=[a-z-]+|--utility-sub-type=[a-zA-Z.]+' |
						tr '\n' ' ')" ;;
				esac
			done
			n=$((n + 1))
			sleep 1
		done
	) > /var/children.log 2>&1 &

	# Wait on the clock, not on a loop count: every iteration spawns swaymsg, and
	# on one vCPU next to a 220 MB binary a "180 iteration" loop ran far longer
	# than 180 seconds and the run died inside it.
	#
	# The default is 180s, measured: the GPU thread spends about 77 of them in one
	# stretch before it says anything again. Overridable with b1nix.window-wait=N,
	# because "does it need longer?" is a question about this machine.
	win=""
	t0=$(date +%s)
	window_wait=$(sed -n 's/.*b1nix\.window-wait=\([0-9]*\).*/\1/p' /proc/cmdline 2>/dev/null)
	deadline=$((t0 + ${window_wait:-180}))
	# A deliberate silence, so the kernel's watchdog will speak: its task dump only
	# fires after sixty seconds with nothing on the console, and this loop prints
	# every fifteen. Only when there is a watchdog to feed.
	if has_flag b1nix.task-watch; then
		sleep 120
	else
		sleep 20
	fi
	while [ "$(date +%s)" -lt "$deadline" ]; do
		# Bounded, because this is also a probe of the compositor: an unbounded
		# swaymsg blocks the whole watch when sway stops answering, and a watch that
		# never comes round again looks exactly like a run in which nothing happened.
		tree=$(timeout -k 2 5 swaymsg -t get_tree 2>/dev/null)
		if [ -z "$tree" ]; then
			echo "I915-SWAY: compositor did not answer get_tree within 5s"
		# "chromium" is the package name, not the window's: the toplevel carries
		# app_id "chrome-__root_browser-proof.html-Default" and the title "b1nix".
		# Matching the package name reported "no window" for a window that was on
		# the screen.
		elif echo "$tree" | grep -qiE '"app_id": *"chrom|browser-proof'; then
			win=1
			break
		fi
		# -f: the binary is /usr/lib/chromium/chromium behind a launcher, so a bare
		# name match is not reliable.
		if ! pgrep -f "chromium" >/dev/null 2>&1; then
			echo "I915-SWAY: FAIL chromium-window (process exited after $(($(date +%s) - t0))s)"
			break
		fi
		echo "I915-SWAY: waiting for the window, $(($(date +%s) - t0))s, rss=$(cat /proc/meminfo 2>/dev/null | grep -i memfree | head -1)"
		wait_n=$((${wait_n:-0} + 1))
			# What the browser has said since the last check: a run that ends on the
			# runner's timeout never reaches the dump below. No sort and no tail on
			# the registry lines -- sorting is by text, so global(10..13) lands ahead
			# of global(2) and a tail cut exactly the entries in question.
		if [ $((wait_n % 4)) -eq 0 ]; then
			echo "--- browser so far ($(wc -l < /var/chromium.log 2>/dev/null) lines) ---"
			grep -av "^\[[0-9]* " /var/chromium.log 2>/dev/null | tail -4
			grep -aE "\.global\(|xdg_wm_base|xdg_surface|xdg_toplevel" \
				/var/chromium.log 2>/dev/null | sed 's/.*] //' | uniq
			echo "--- end browser so far ---"
		fi
		sleep 15
	done
		# The picture, taken the moment the window is there rather than at the end of
		# the run: the shot at the end has twice been lost to a compositor that did
		# not survive to reach it.
	if [ -n "$win" ]; then
		echo "I915-SWAY: ok chromium-window after $(($(date +%s) - t0))s"
		if grim /var/browser-window.png 2>/var/grim-win.log; then
			echo "I915-SWAY: ok browser-shot ($(wc -c < /var/browser-window.png) bytes)"
			# A second frame eight seconds later. The proof page drives a counter once
			# a second, so two frames that differ are a live renderer and two identical
			# frames are a picture left behind by one that stopped.
			sleep 8
			grim /var/browser-window2.png 2>>/var/grim-win.log ||
				echo "I915-SWAY: FAIL second-shot"
			echo "I915-SWAY: second shot $(wc -c < /var/browser-window2.png 2>/dev/null) bytes"
			# Out through a disk, not the console: the kernel writes its own lines
			# between the guest's, and base64 with a boot message spliced into a line
			# is not recoverable. The length goes first so the host knows where each
			# picture ends.
			if [ -b /dev/vdb ]; then
				( printf 'B1NIXPNG%08d\n' "$(wc -c < /var/browser-window.png)"
				  cat /var/browser-window.png
				  printf 'B1NIXPNG%08d\n' "$(wc -c < /var/browser-window2.png)"
				  cat /var/browser-window2.png ) > /dev/vdb 2>/dev/null &&
					echo "I915-SWAY: shots written to /dev/vdb" ||
					echo "I915-SWAY: shots could not be written to /dev/vdb"
				sync
			else
				echo "---PNG-browser---"
				base64 /var/browser-window.png
				echo "---PNG-browser-END---"
			fi
		else
			echo "I915-SWAY: FAIL browser-shot — $(head -1 /var/grim-win.log 2>/dev/null)"
		fi
	elif pgrep chromium >/dev/null 2>&1; then
		echo "I915-SWAY: FAIL chromium-window (alive, no window after $(($(date +%s) - t0))s)"
	fi
	echo "--- chromium tree ---"
	timeout -k 2 10 swaymsg -t get_tree 2>&1 | grep -iE "app_id|name|pid" | head -40
	echo "--- end chromium tree ---"
	echo "--- child processes seen alive ---"
	sort -u /var/children.log 2>/dev/null | head -12 | sed 's/^/  /'
	echo "  (none seen)" | head -$([ -s /var/children.log ] && echo 0 || echo 1)

	# How far the browser got in the Wayland protocol, counted rather than guessed
	# from a tail: a client that binds the globals and stops has a different
	# problem from one that creates surfaces but never gives them a toplevel.
	echo "--- wayland protocol summary ---"
	for req in wl_registry.bind create_surface xdg_surface get_toplevel \
	           set_title commit attach; do
		echo "  $req: $(grep -ac "$req" /var/chromium.log 2>/dev/null)"
	done
	echo "  last protocol line: $(grep -aE '@[0-9]+\.' /var/chromium.log 2>/dev/null | tail -1)"
	# The requests it SENT, not the events it received: a client that stops
	# mid-handshake and one that never starts look identical in the event stream,
	# because the compositor announces the same globals either way.
	echo "  --- last requests sent by the browser ---"
	grep -aE '^\[[0-9. ]*\] +-> ' /var/chromium.log 2>/dev/null |
		sed 's/^\[[0-9. ]*\] *//' | tail -20 | sed 's/^/    /'
	echo "  request types: $(grep -aoE '\-> [a-z_]+@[0-9]+\.[a-z_]+' /var/chromium.log 2>/dev/null |
		sed 's/-> //; s/@[0-9]*//' | sort -u | tr '\n' ' ')"
	echo "  globals received by the browser: $(grep -ac 'wl_registry.*global' /var/chromium.log 2>/dev/null)"
	echo "  bound interfaces: $(grep -ao 'bind([0-9]*, "[a-z_0-9]*"' /var/chromium.log 2>/dev/null |
		sed 's/.*"//' | sort -u | tr '\n' ' ')"
	echo "  --- startup stage ---"
	grep -aiE "startup|browser_creator|BrowserWindow|CreateBrowser|profile_manager|session_restore|first_run" \
		/var/chromium.log 2>/dev/null | grep -av '@[0-9]*\.' | tail -10 | sed 's/^/    /'
	echo "  --- what its wayland layer said ---"
	grep -aiE "wayland_connection|compositor|wl_shm|Failed to bind|not available|unsupported" \
		/var/chromium.log 2>/dev/null | grep -av '@[0-9]*\.' | grep -av '#[0-9]*\.' |
		tail -12 | sed 's/^/    /'
	echo "  --- first 40 protocol lines (browser's own trace) ---"
	grep -aE '@[0-9]+\.' /var/chromium.log 2>/dev/null | head -40 |
		sed -E 's/^\[[0-9. ]*\] *//' | sed 's/^/    /'
	echo "  --- last 25 lines of the browser log ---"
	tail -25 /var/chromium.log 2>/dev/null | sed 's/^/    /'
	echo "  wayland errors: $(grep -acE 'error|Error|refused|protocol' /var/chromium.log 2>/dev/null)"
	echo "  window lines:"
	grep -aiE "window|widget|surface|toplevel" /var/chromium.log 2>/dev/null |
		grep -av '@[0-9]*\.' | tail -12
	grep -aE 'error|Error' /var/chromium.log 2>/dev/null | grep -av '@[0-9]*\.' | tail -5
	echo "--- end wayland protocol summary ---"
	echo "--- browser log tail ($(wc -l < /var/chromium.log 2>/dev/null) lines) ---"
	grep -av '@[0-9]*\.' /var/chromium.log 2>/dev/null | tail -30
	echo "--- end browser log ---"
	echo "--- browser lines from the compositor log ---"
	grep -aiE "chromium|chrome|CHROMIUM-EXIT|ERROR|FATAL|Check failed|xdg_toplevel|new toplevel" /tmp/sway.log 2>&1 | tail -40
	echo "--- end browser lines ---"
	echo "--- writable check ---"
	for d in /tmp /var /run /root; do
		( : > "$d/.wtest" ) 2>/dev/null && { echo "$d writable"; rm -f "$d/.wtest"; } || echo "$d NOT writable"
	done
	echo "--- end writable check ---"
fi

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
