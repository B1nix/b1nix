#!/bin/sh

# Whole-word flag matching.
#
# `grep -q b1nix.chromium /proc/cmdline` also matches b1nix.chromium-min, which
# every image built by the browser target carries — so the branch that starts
# the browser ran even when the run asked for a plain terminal client instead,
# and the client never started. The dot is a regex wildcard as well, which is
# the same trap one character smaller.
has_flag() {
	# A flag matches whether it stands alone or carries a value:
	# b1nix.glprobe and b1nix.glprobe=virtio_gpu are the same flag, and an
	# exact-token test silently ignored the second form -- the probe simply
	# reported "not requested" for a command line that plainly requested it.
	for tok in $(cat /proc/cmdline 2>/dev/null); do
		[ "$tok" = "$1" ] && return 0
		case "$tok" in "$1"=*) return 0 ;; esac
	done
	return 1
}
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
if ! grep -q "b1nix.i915sway" /proc/cmdline 2>/dev/null; then
	# Silence here is indistinguishable from "the flag was not passed", and a
	# boot where the kernel printed the flag but this read did not see it looks
	# exactly the same as a boot that was never asked to run sway. Say which of
	# the two happened, and show what the read actually returned.
	echo "I915-SWAY: not requested; /proc/cmdline = [$(cat /proc/cmdline 2>&1)]"
	exit 0
fi

# Every milestone carries the guest's uptime, so a slow boot can be split
# into its parts instead of guessed at.
up() { cut -d" " -f1 /proc/uptime 2>/dev/null || echo "?"; }
echo "I915-SWAY: start t=$(up)"

# A heap-churn check, when asked for one, instead of the compositor.
#
# The same shape of work the compositor's crash needs — several threads
# allocating and freeing while every block is verified — with none of the
# graphics, so a run costs seconds rather than two minutes.
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

# Does the guest read back what the image actually holds?
#
# The loader has twice reported missing symbols whose names were themselves
# damaged ("erTMCloneTable" for "_ITM_registerTMCloneTable"), which is not a
# linking problem but a file whose contents arrive wrong. Checksumming the two
# binaries involved settles it in one run: matching sums mean the reads are
# faithful and the fault is above the filesystem; differing sums mean the read
# path corrupts data, and that is a kernel bug worth everything else waiting.
echo "I915-SWAY: md5 libpipewire $(md5sum /lib/libpipewire-0.3.so.0 2>&1 | cut -d' ' -f1) (expect 8519bd11f2693a79a0b5038dc553d23e)"
echo "I915-SWAY: md5 avcodec-16M $(md5sum /lib/libavcodec.so.60.31.102 2>&1 | cut -d' ' -f1) (expect 8255cd2b529e80e4363ccfa0efecd33c)"
echo "I915-SWAY: md5 icudata-30M $(md5sum /usr/share/icu/74.2/icudt74l.dat 2>&1 | cut -d' ' -f1) (expect 3637714fbde5e57185223d4a8aa254e6)"
# Read the big one twice. Two different wrong sums mean each read is corrupted
# afresh — the data is damaged in transit; one repeated wrong sum means a bad
# copy settled in the cache and is served consistently. The two have different
# causes and there is no way to tell them apart from a single reading.
echo "I915-SWAY: md5 chromium#1  $(md5sum /lib/chromium/chromium 2>&1 | cut -d' ' -f1) (expect 1f8f49c00bf53053df1fb3e2e54fae67)"
echo "I915-SWAY: md5 chromium#2  $(md5sum /lib/chromium/chromium 2>&1 | cut -d' ' -f1) (expect 1f8f49c00bf53053df1fb3e2e54fae67)"

# Where a graphics stack looks for a render node, checked rather than assumed.
# The node exists in /dev and is published under /sys, and Chromium still says
# it cannot find one — so print every path it could be enumerating.
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

CARD=/dev/dri/card1
[ -e "$CARD" ] || CARD=/dev/dri/card0
if [ ! -e "$CARD" ]; then
	echo "I915-SWAY: fail no-card"
	echo "I915-SWAY: done"
	exit 0
fi
echo "I915-SWAY: ok card $CARD t=$(up)"

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

# Render-only mode: draw off-screen on the GPU and check the pixels.
#
# The compositor answers a display question. This answers a different one that
# had been quietly riding along with it: does the render engine actually
# execute what Mesa submits? Every previous run stopped at "iris initialised",
# which a driver that never successfully submits a batch also prints.
#
# It is placed here, before the package stage, because it needs nothing from
# the network: Mesa is in the image whenever B1NIX_GPU_DRV=1 built it, and the
# probe reaches libEGL and libGLESv2 through dlopen. A run costs seconds.
if has_flag b1nix.glprobe; then
	iris=/usr/lib/xorg/modules/dri/iris_dri.so
	if ! grep -q 'b1nix\.glprobe=' /proc/cmdline 2>/dev/null && [ ! -e "$iris" ]; then
		echo "I915-SWAY: fail no-iris (build with B1NIX_GPU_DRV=1)"
		echo "I915-SWAY: done"
		exit 0
	fi
	# Name the driver rather than let the loader search. Without this Mesa is
	# free to answer with a software rasteriser, and a green run would then
	# say nothing at all about the GPU — the probe checks GL_RENDERER for the
	# same reason, but failing early is clearer than failing late.
	#
	# Which driver, though, depends on which GPU is in the machine:
	# `b1nix.glprobe=virtio_gpu` probes the virtio GPU's virgl path, and a
	# bare b1nix.glprobe keeps iris for the passed-through Intel part. The
	# iris file check above is skipped when another driver is named, because
	# demanding iris on a machine that has no Intel GPU would refuse a probe
	# that is about to succeed.
	drv=$(sed -n 's/.*b1nix\.glprobe=\([A-Za-z0-9_]*\).*/\1/p' /proc/cmdline)
	export MESA_LOADER_DRIVER_OVERRIDE="${drv:-iris}"
	export LIBGL_DRIVERS_PATH=/usr/lib/xorg/modules/dri
	export EGL_LOG_LEVEL=debug
	# Mesa's own account of why it gave up. Without these the client reports
	# one line -- "failed to create dri2 screen" -- for every possible cause,
	# and the driver's specific complaint stays inside it.
	export MESA_DEBUG=1
	export VIRGL_DEBUG=verbose
	export LIBGL_DEBUG=verbose
	# What Mesa reads to identify the device. drmGetDevice2 parses these, and
	# a device it cannot identify is one pipe_loader will not create a screen
	# for -- which looks from outside exactly like a driver that refused.
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
if [ -e /dev/vda ]; then
	mkdir -p /var/cache/bpkg
	if mount -t ext4 /dev/vda /var/cache/bpkg 2>/dev/null; then
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

# http, not https — the same choice apk makes.
#
# Every package carries an RSA signature over a control block that carries the
# payload's sha256, and bpkg refuses to install one whose chain does not check
# out. Transport encryption adds nothing to that and costs a great deal here:
# the cipher runs in software on one core, in front of every byte of a
# quarter-gigabyte download.
cat > /etc/bpkg.conf <<'EOF'
INDEX_URL=http://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64/APKINDEX.tar.gz http://dl-cdn.alpinelinux.org/alpine/v3.20/community/x86_64/APKINDEX.tar.gz
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
# A real browser, when asked for one.
#
# 113 MB to download and 248 MB installed, so it is never in the image and
# never fetched unless b1nix.chromium says so. It is the hardest userspace
# this system has been asked to run — threads, futexes, shared memory,
# sandboxing and a Wayland client all at once — which is exactly why it is
# worth running.
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
	# Every few packages, not every one.
	#
	# A run that is cut off part-way through the list must not lose everything
	# it fetched, so the cache is flushed as it goes — but flushing after each
	# package writes the whole dirty block cache each time, including for
	# packages that came from the cache and wrote nothing. Every eighth is
	# often enough to bound the loss and rare enough not to dominate the
	# install.
	pkg_n=$((${pkg_n:-0} + 1))
	[ $((pkg_n % 8)) -eq 0 ] && sync
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
# Clients for the desktop. What matters is that something other than the
# compositor renders — the terminal is Alpine's, like the compositor itself.
# Clients are launched only when the cmdline asks for them. sway spawns every
# one of these through posix_spawn, and a compositor that dies while starting a
# client tells us nothing about whether it can drive a display — the two
# questions get separated by b1nix.sway-clients.
: > /etc/sway/config
if has_flag b1nix.chromium && [ -x /usr/bin/chromium ]; then
	# The browser as sway's client.
	#
	# --no-sandbox because chromium's sandbox wants namespaces this kernel does
	# not have; --disable-gpu because there is no GL driver for this hardware
	# yet, so it composites in software and hands sway a shared-memory buffer —
	# which is the same path every other client here uses.
	#
	# --no-zygote because the zygote forks with clone(SIGCHLD, ...) and this
	# kernel only implements clone with CLONE_VM; chromium supports the flag
	# alongside --no-sandbox and forks its children directly instead.
	{
		echo 'output * bg #202020 solid_color'
		# Window appearance, taken from a working desktop's sway config
		# (bla1r1/DotsFiles) and cut down to what plain sway understands —
		# no swayFX blur or shadows, no bar, no launcher, nothing that would
		# need a program this image does not carry. A visible border and a
		# named colour make a photograph of the panel say which window is
		# which, which a plain grey field does not.
		echo 'default_border pixel 2'
		echo 'client.focused          #7aa2f7 #1a1b26 #c0caf5 #bb9af7 #7aa2f7'
		echo 'client.unfocused        #101014 #16161e #a9b1d6 #101014 #101014'
		echo 'gaps inner 5'
		: # the browser is started below, by this script, so its output is ours
	} > /etc/sway/config
elif has_flag b1nix.sway-clients; then
	{
		# Saturated colour and large text, for the same reason cage's run
		# uses them: the proof that the picture is on the panel is a
		# photograph, and a photograph of dark grey at an angle proves
		# nothing. b1nix.bright asks for the readable version.
		if grep -q "b1nix.bright" /proc/cmdline 2>/dev/null; then
			echo 'output * bg #00cc44 solid_color'
			# b1nix.client-nopty asks for a drawing client that opens no
			# pseudo-terminal: the terminal is the only client here that
			# does, and separating the two says whether the pty path is
			# what the compositor's crash needs.
			if has_flag b1nix.client-nopty; then
				echo 'exec /usr/bin/swaybg -c "#00cc44"'
			elif has_flag b1nix.client-quiet; then
				# The same terminal, the same threads and buffers, but a
				# program that writes nothing: what changes is the traffic
				# through the pseudo-terminal, and nothing else.
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
# The renderer.
#
# Software by default: pixman needs nothing from the GPU beyond a dumb buffer,
# so a run that fails still says something about KMS rather than about Mesa.
# b1nix.i915gl asks for the hardware path instead — wlroots then brings up EGL
# on the render node, which loads Mesa's iris and puts the GT to work. That
# driver only reaches the image under B1NIX_GPU_DRV=1, so say plainly when it is
# absent rather than letting EGL fail with a message about nothing in
# particular.
# The choice itself is /etc/render-select.sh, which every compositor launcher
# here shares: it looks for a render node, a Mesa driver behind it and a
# driver that really renders (gl_probe), and settles on pixman when any of
# those is missing rather than letting the compositor fail to start. Software
# unless b1nix.i915gl asks for the hardware path — a run that fails on pixman
# says something about KMS, and one that fails on GLES says something about
# Mesa, and keeping the default software keeps those apart.
. /etc/render-select.sh
if grep -q "b1nix.i915gl" /proc/cmdline 2>/dev/null; then
	# Name iris: this is the passed-through Intel GPU, and letting the loader
	# search has picked a different driver on a machine with two cards.
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
	echo "I915-SWAY: ok seatd t=$(up)"
else
	echo "I915-SWAY: fail seatd"
	tail -20 /tmp/seatd.log
fi
export XDG_SESSION_TYPE=wayland
# What a desktop session tells its clients about itself.
#
# Chromium reads XDG_CURRENT_DESKTOP when it decides how to behave on Wayland,
# and a session that names none is not a case it is written for. A real sway
# session exports these (see bla1r1/DotsFiles, which imports exactly this set
# into the user environment); this one exported only the session type.
export XDG_CURRENT_DESKTOP=sway
export XDG_SESSION_DESKTOP=sway
export GDK_BACKEND=wayland
export QT_QPA_PLATFORM=wayland

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
		# One virtual output, at the panel's size. A headless compositor with
		# no output has nowhere to put a window, and a client that asks for one
		# is told there is no screen — which would look exactly like the
		# failure being investigated.
		export WLR_HEADLESS_OUTPUTS=1
		echo "I915-SWAY: headless isolation run (virtual output 1920x1080)"
	fi
	echo "I915-SWAY: starting sway on $CARD t=$(up)"
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

# When a browser was asked for, say whether it actually put a window on the
# screen, and say it as a marker rather than leaving the run to be read by eye.
# A run that ends without either marker is a run that told us nothing.
if has_flag b1nix.chromium; then
	# Start the browser here rather than from sway's config. sway's exec
	# detaches its children and their stdout and stderr go nowhere we can read:
	# three runs in a row produced neither chromium's own errors nor the exit
	# code we appended to the command. Started from this script it is our
	# child, and this script's output is the serial console.
	# --in-process-gpu: the separate GPU process died here before any window
	# existed — it looks for a DRM render node (this card exposes none yet) and
	# then for VA-API on it, and the browser waits for a process that never
	# comes back. In-process there is one less child to lose, and the software
	# path this run uses does not need a process of its own. VaapiVideoDecoder
	# is switched off for the same reason: it has no device to open.
	#
	# A page built to be photographed, for the same reason the compositor runs
	# a green terminal: about:blank on a panel across a dark room is
	# indistinguishable from a monitor showing nothing. Saturated colour fills
	# the screen, the text says which machine drew it, and a counter driven by
	# the page's own script changes between frames — so a photograph shows the
	# browser rendering rather than a still image left over from something else.
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
	# The control client (foot) has served its purpose: on this same socket it
	# binds fifteen globals and creates a toplevel, which is what established
	# that the compositor path works and the browser's silence is its own. It is
	# not run every time — it costs a fifth of the boot and has twice hung and
	# taken the run with it. b1nix.wayland-control brings it back.
	if grep -q "b1nix.wayland-control" /proc/cmdline 2>/dev/null; then
		echo "I915-SWAY: control client (foot) t=$(up)"
		( WAYLAND_DEBUG=1 timeout -k 5 20 /usr/bin/foot true > /var/foot.log 2>&1
		  echo "FOOT-RUN-EXIT rc=$?" >> /var/foot.log )
		echo "--- control client protocol ---"
		for req in "\.bind(" "create_surface" "get_toplevel"; do
			echo "  $req: $(grep -ac "$req" /var/foot.log 2>/dev/null)"
		done
		# The globals it took, by NAME NUMBER, which is the whole comparison:
		# the browser binds everything numbered 10 and up and nothing below it,
		# a boundary in arrival order rather than in the interfaces themselves.
		# A client that takes number 1 and 2 here proves the early events are
		# delivered, and puts the loss on the browser's side of the socket.
		echo "  bound by number: $(grep -ao 'bind([0-9]*, "[a-z_0-9]*"' /var/foot.log 2>/dev/null |
			sed 's/bind(//; s/, "/:/; s/"//' | sort -n | tr '\n' ' ')"
		echo "--- end control client protocol ---"

		# A real window, held open, and a picture of it.
		#
		# Everything above says the compositor and the socket are sound and the
		# browser stops on its own side. That claim is worth proving rather
		# than asserting: a terminal kept open is a window on the same output
		# the browser was asked to draw on, and grim reads back what was
		# actually composited. If this produces a PNG with content, the display
		# path end to end — client, compositor, output — works.
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

	# Shared memory, checked before the browser needs it.
	#
	# A Wayland client shows nothing without it: every frame is a wl_shm buffer
	# over a file both sides map. The browser binds wl_shm and then creates no
	# surface at all, and "it cannot make a buffer" and "it decided not to draw"
	# look identical from outside. This says which.
	mkdir -p /dev/shm
	if : > /dev/shm/probe 2>/dev/null && dd if=/dev/zero of=/dev/shm/probe bs=4096 count=1 2>/dev/null; then
		echo "I915-SWAY: shm ok ($(ls -l /dev/shm/probe 2>&1 | awk '{print $5}') bytes)"
		rm -f /dev/shm/probe
	else
		echo "I915-SWAY: shm FAIL — a client cannot allocate a frame buffer"
	fi

	# Is /proc/<pid>/task readable, and does it agree with itself?
	#
	# The browser's main thread was found parked in the code that reads this
	# directory: its sandbox counts the entries to decide whether the process is
	# single-threaded. Read it the same way twice from a process whose thread
	# count is known, and compare with what the kernel reports elsewhere. A
	# listing that disagrees with itself between reads, or with Threads:, is the
	# fault; one that is stable and correct clears the directory entirely.
	echo "I915-SWAY: task dir — self=$(ls /proc/self/task 2>&1 | wc -l) again=$(ls /proc/self/task 2>&1 | wc -l) Threads=$(grep -a '^Threads:' /proc/self/status 2>/dev/null | tr -d '\t ' | sed 's/Threads://')"
	# And for a process that genuinely has many: sway is running by now.
	swp=$(pidof sway 2>/dev/null | cut -d' ' -f1)
	if [ -n "$swp" ]; then
		echo "I915-SWAY: task dir — sway=$(ls /proc/$swp/task 2>&1 | wc -l) again=$(ls /proc/$swp/task 2>&1 | wc -l) Threads=$(grep -a '^Threads:' /proc/$swp/status 2>/dev/null | tr -d '\t ' | sed 's/Threads://')"
	fi

	# Does time pass correctly here?
	#
	# The browser is a queue of delayed tasks: almost nothing it does runs
	# immediately, and a clock that jumps, stalls or runs at the wrong rate
	# stops it dead while every other subsystem looks healthy — which is the
	# shape of what we are left with. Two readings around a known sleep say
	# whether a second in here is a second.
	t_a=$(date +%s)
	u_a=$(cut -d" " -f1 /proc/uptime)
	sleep 5
	t_b=$(date +%s)
	u_b=$(cut -d" " -f1 /proc/uptime)
	echo "I915-SWAY: clock check — wall $((t_b - t_a))s, uptime $u_a -> $u_b (expect 5s each)"
	# And the monotonic clock the browser actually schedules on, read twice in
	# a row: it must never go backwards and must not stand still.
	m_a=$(date +%s%N 2>/dev/null)
	m_b=$(date +%s%N 2>/dev/null)
	echo "I915-SWAY: monotonic pair $m_a $m_b"
	# Resolution, from the kernel rather than from `date`: busybox's date has
	# no %N, so ten readings of it measure seconds and say nothing about the
	# clock underneath — the first version of this check "measured" 10 ms
	# resolution as 2 distinct values and meant nothing. /proc/timer_list
	# reports what the monotonic clock actually is.
	echo "I915-SWAY: clock source: $(grep -a -m1 'monotonic' /proc/timer_list 2>/dev/null ||
		echo '(no /proc/timer_list)')"

	# The size the browser actually asks for.
	#
	# Its field-trial state is one 256 KiB shared region, created before any
	# window exists, and the code that creates it treats failure as fatal — the
	# abort the browser dies in names that allocator and that size. A 4 KiB
	# probe passing says nothing about it, so ask for the real one.
	if dd if=/dev/zero of=/dev/shm/probe256 bs=1024 count=256 2>/dev/null; then
		echo "I915-SWAY: shm 256K ok ($(ls -l /dev/shm/probe256 2>&1 | awk '{print $5}') bytes)"
		rm -f /dev/shm/probe256
	else
		echo "I915-SWAY: shm 256K FAIL — the size the field-trial allocator needs"
	fi

	# Does the browser work at all, with no window in the way?
	#
	# It initialises Wayland completely — binds two dozen globals, answers the
	# compositor — and then creates no surface and says nothing about a window
	# even at --v=1. Headless removes the window layer from the question: a
	# screenshot means the engine, the processes and the IPC are all fine and
	# only the Ozone path is not; no screenshot means it never gets that far.
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
		# The abort's own words, not the tail. A Chromium CHECK prints the file
		# and condition that failed and then everything after it is unwinding
		# noise, so pick the failure out rather than showing the last lines.
		grep -aiE "FATAL|CHECK failed|DCHECK|Aborted|assert|NOTREACHED|out of memory|bad_alloc" \
			/var/headless.log 2>/dev/null | head -10
		echo "  --- last lines ---"
		grep -av "bus\.cc" /var/headless.log 2>/dev/null | tail -14
	fi
	fi

	# Does the browser work at all here, with no compositor in the picture?
	#
	# Everything so far says the Wayland side is healthy — the compositor
	# answers, the globals arrive — and that the browser stops before binding
	# any of them, in its own code. Headless renders the same page through the
	# same engine with no display server at all: if it prints the document, the
	# fault is in the display path; if it dies the same way, the display path is
	# innocent and the problem is under it. Either answer halves the search.
	# Answered: the browser hangs identically with no compositor in the picture,
	# which is what sent the search into mmap and found the VMA-list race. Kept
	# behind a flag rather than deleted — it is the cheapest way to ask the same
	# question again — but off by default, because it costs four minutes of a
	# run that has a window to wait for.
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

	# One question before the real one: does the smallest possible page render?
	#
	# Blank page, one process, no window system — the case that isolates the
	# browser's own machinery from everything around it. Kept short, because the
	# run's budget has to reach the window test that follows; the earlier
	# version asked six questions in sequence and spent the whole run before
	# reaching the one that matters.
	# Only when a snapshot is being collected.
	#
	# This pre-run exists to give the kernel's watchdog something readable to
	# dump, and it costs the run about two minutes of guest time before the
	# window question is even asked — with the browser and the compositor both
	# resident, that is the difference between reaching the verdict and being
	# killed on the runner's timeout. Without b1nix.task-watch there is no
	# watchdog to feed, so the run goes straight to the browser.
	if has_flag b1nix.task-watch; then
	echo "I915-SWAY: minimal start t=$(up)"
	( timeout -k 5 150 /usr/bin/chromium --headless=new --no-sandbox \
		--single-process --disable-gpu --disable-dev-shm-usage \
		--user-data-dir=/tmp/chromium-min \
		--dump-dom about:blank > /var/min.log 2>&1 & )

	# Left alone in silence while it is stuck, so the kernel's watchdog takes a
	# snapshot of its threads. The dump only fires after a minute with nothing
	# on the console, and this is now the smallest case that still fails — one
	# process, a blank page, no window system — so the snapshot is readable
	# rather than a wall of threads from three processes.
	sleep 95

	# Is it burning the processor, or waiting?
	#
	# The task dump showed its scheduler counter advancing, which suggested a
	# thread spinning — but that counter is a scheduler-internal value and a
	# poor thing to conclude from. Consumed processor time is the direct
	# measure: sampled twice, the difference over a known interval says plainly
	# whether the process is running or parked.
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

	echo "I915-SWAY: starting chromium on $WAYLAND_DISPLAY t=$(up)"
	# The display name is the one discovered above, not a guess: sway names its
	# socket wayland-0 or wayland-1 depending on what else asked for one first,
	# and a browser pointed at the wrong name reports no Wayland server at all.
	# Into a file, not onto the console. The console is a 115200-baud serial
	# line, and at --v=1 the browser writes thousands of lines through it — the
	# machine spends its startup inside the UART instead of laying out a page.
	# The verdict below dumps the tail of this file, which is the part anyone
	# reads anyway.
	(
		# Every Wayland request and event the browser exchanges with the
		# compositor, into the same file. When a client stops without a word,
		# the protocol trace is what says whether it ever asked for a surface
		# — the difference between "the compositor ignored it" and "it never
		# got that far".
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
		# What the browser itself said, on the console.
		#
		# Its output went to a file inside the guest, so a crash here was
		# visible only as a fault address: the lines that name which process
		# type died, and what failed before it, never left the disk. The
		# window is the interesting part, so the tail is enough.
		echo "--- chromium log (tail) ---"
		tail -40 /var/chromium.log 2>/dev/null
		echo "--- end chromium log ---"
	) &

	# Who the browser's children are, while they are still alive.
	#
	# The one child it starts dies after fifteen seconds — its IPC channel never
	# connects — and by the time anything looks, it is a zombie with an empty
	# cmdline. Which process it was decides the diagnosis: a network service
	# means the profile is waiting on it, a renderer means the window already
	# existed. Sample often and briefly; the interesting window is the first
	# half minute.
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

	# Wait on the clock, not on a loop count: every iteration here spawns
	# swaymsg, and on one vCPU with an eagerly-loaded 220 MB binary next door
	# a "180 iteration" loop ran far longer than 180 seconds and the run died
	# inside it, before it could report anything. Poll rarely, say we are alive,
	# and stop at a deadline we can state.
	win=""
	t0=$(date +%s)
	# Shorter than the run's own timeout, or the summaries below never print:
	# at 1500s the VM was always killed mid-wait and the protocol counters — the
	# whole point of the wait — went with it. 90s because a browser that is going
	# to create a surface does so within seconds of binding the compositor's
	# globals; waiting minutes past that only makes each experiment slower.
	# 180s, measured rather than guessed. The GPU thread spends about 77 of them
	# in one stretch before it says anything again — it tries VA-API, which this
	# machine has no driver for, and only then carries on. At 90s the wait ended
	# inside that gap and reported no window for a browser that had not finished
	# starting; the flags above remove the detour, and this leaves room for what
	# is left of it.
	# Overridable, because "does it need longer?" is a question about this
	# machine and not a property of the browser. Every gap in its own log here
	# is tens of seconds — a start-up that takes a few seconds on hardware is
	# running under emulation with software rendering — so a fixed 180 cannot
	# distinguish "stuck" from "not finished".
	window_wait=$(sed -n 's/.*b1nix\.window-wait=\([0-9]*\).*/\1/p' /proc/cmdline 2>/dev/null)
	deadline=$((t0 + ${window_wait:-180}))
	# Give it a moment to exist before asking whether it died: the first check
	# used to run before the process had even exec'd and reported it gone at 0s,
	# which stopped the watch while the browser was in fact starting fine.
	# A deliberate silence, so the kernel's watchdog will speak.
	#
	# The task dump that names what a parked thread is waiting on only fires
	# after sixty seconds with nothing on the console — and this loop prints
	# every fifteen, so it never fired while the browser was stalled. The stall
	# happens within ten seconds of the browser starting, so hold quiet across
	# it and let the watchdog take its snapshot.
	# The silence is for the watchdog, so it is only kept when there is one:
	# two minutes of saying nothing while the browser starts is two minutes the
	# run does not have for the window it is waiting on.
	if has_flag b1nix.task-watch; then
		sleep 120
	else
		sleep 20
	fi
	while [ "$(date +%s)" -lt "$deadline" ]; do
		# Bounded, because this is also a probe of the compositor.
		#
		# An unbounded swaymsg blocks this whole watch when sway stops
		# answering its socket, and a watch that never comes round again looks
		# exactly like a run in which nothing happened: no verdict, no browser
		# log, just a timeout. Several runs were lost that way. Ask with a
		# deadline, and say so when it is not met.
		tree=$(timeout -k 2 5 swaymsg -t get_tree 2>/dev/null)
		if [ -z "$tree" ]; then
			echo "I915-SWAY: compositor did not answer get_tree within 5s"
		# "chromium" is the package name, not the window's.
		#
		# The toplevel this browser creates carries app_id
		# "chrome-__root_browser-proof.html-Default" and, once the page has
		# loaded, the title "b1nix" — neither of which contains the string this
		# watch was looking for. So a run whose window was on the screen, acked
		# its configure and set its title reported "no window after 300s", and
		# the search went looking for a fault in the compositor that was never
		# there. Match what the browser actually calls itself.
		elif echo "$tree" | grep -qiE '"app_id": *"chrom|browser-proof'; then
			win=1
			break
		fi
		# -f: match the whole command line. The binary is /usr/lib/chromium/chromium
		# behind a launcher, so a bare name match is not reliable.
		if ! pgrep -f "chromium" >/dev/null 2>&1; then
			echo "I915-SWAY: FAIL chromium-window (process exited after $(($(date +%s) - t0))s)"
			break
		fi
		echo "I915-SWAY: waiting for the window, $(($(date +%s) - t0))s, rss=$(cat /proc/meminfo 2>/dev/null | grep -i memfree | head -1)"
		# What the browser has said since the last check. A run that ends on
		# the runner's timeout never reaches the dump below, and then a
		# forty-minute wait produces no evidence at all — this way the log
		# arrives as it is written.
		wait_n=$((${wait_n:-0} + 1))
		if [ $((wait_n % 4)) -eq 0 ]; then
			echo "--- browser so far ($(wc -l < /var/chromium.log 2>/dev/null) lines) ---"
			# The browser's own words, then the last of the protocol
			# traffic: with WAYLAND_DEBUG on, the trace would otherwise
			# bury everything else.
			grep -av "^\[[0-9]* " /var/chromium.log 2>/dev/null | tail -4
			# What the compositor offers and what the browser took: a client
			# that never asks for a window shell is a client that did not
			# find one, and the registry is where that shows.
			# No sort, no tail: sorting is by text, so global(10..13) lands
			# ahead of global(2) and a tail cut exactly the entries in
			# question — which is how a filter artefact came to look like
			# lost protocol messages.
			grep -aE "\.global\(|xdg_wm_base|xdg_surface|xdg_toplevel" \
				/var/chromium.log 2>/dev/null | sed 's/.*] //' | uniq
			echo "--- end browser so far ---"
		fi
		sleep 15
	done
	if [ -n "$win" ]; then
		echo "I915-SWAY: ok chromium-window after $(($(date +%s) - t0))s"
		# The picture, taken the moment the window is there rather than at the
		# end of the run. The shot at the end has twice been lost to a
		# compositor that did not survive to reach it, and "a toplevel exists"
		# is not the same claim as "the browser drew the page" — this is the
		# one that carries out of the guest, as base64 on the console.
		if grim /var/browser-window.png 2>/var/grim-win.log; then
			echo "I915-SWAY: ok browser-shot ($(wc -c < /var/browser-window.png) bytes)"
			# Out through a disk, not through the console.
			#
			# The console is shared: the kernel writes its own lines
			# between the guest's, and base64 that has a boot message
			# spliced into the middle of a line is not recoverable —
			# a 25 KB picture arrived with five lines destroyed and
			# would not decompress. A raw block device carries the
			# bytes exactly. The runner attaches one as the second
			# virtio disk and reads the PNG straight off it: the
			# length goes first, so the host knows where it ends.
			# A second frame, eight seconds later.
			#
			# One picture proves a window was mapped with the page's colours in
			# it; it does not prove the browser is still running. The proof
			# page drives a counter from its own script once a second, so two
			# frames that differ are a live renderer, and two identical frames
			# are a picture left behind by one that stopped. Both go out on the
			# disk, one after the other, each with its length.
			sleep 8
			grim /var/browser-window2.png 2>>/var/grim-win.log ||
				echo "I915-SWAY: FAIL second-shot"
			echo "I915-SWAY: second shot $(wc -c < /var/browser-window2.png 2>/dev/null) bytes"
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
	# Chromium's output goes to sway's stderr, which is this console — the two
	# attempts to put it in a file failed because neither /tmp nor /var was
	# writable at this point in boot, and an unwritable redirect kills the
	# launch outright rather than just losing the log.
	# Chromium inherits sway's stdout/stderr, and sway's go to /tmp/sway.log —
	# so its own words are in there, not on the console. The head -120 dump
	# earlier in this script runs before the browser has said anything.
	# sway runs with -d, so a plain tail is 60 lines of its own debug. Pick out
	# what the browser and the compositor said about the browser.
	# How far the browser got in the Wayland protocol, counted rather than
	# guessed from a tail. A client that binds the globals and stops has a
	# different problem from one that creates surfaces but never gives them a
	# toplevel, and the tail of a 100k-line trace shows neither.
	echo "--- child processes seen alive ---"
	sort -u /var/children.log 2>/dev/null | head -12 | sed 's/^/  /'
	echo "  (none seen)" | head -$([ -s /var/children.log ] && echo 0 || echo 1)

	echo "--- wayland protocol summary ---"
	for req in wl_registry.bind create_surface xdg_surface get_toplevel \
	           set_title commit attach; do
		echo "  $req: $(grep -ac "$req" /var/chromium.log 2>/dev/null)"
	done
	echo "  last protocol line: $(grep -aE '@[0-9]+\.' /var/chromium.log 2>/dev/null | tail -1)"
	# The requests it sent, not the events it received. A client that stops
	# mid-handshake and one that never starts look identical in the event
	# stream — the compositor announces the same globals either way. What it
	# ASKED for is the half that says where it stopped.
	echo "  --- last requests sent by the browser ---"
	grep -aE '^\[[0-9. ]*\] +-> ' /var/chromium.log 2>/dev/null |
		sed 's/^\[[0-9. ]*\] *//' | tail -20 | sed 's/^/    /'
	echo "  request types: $(grep -aoE '\-> [a-z_]+@[0-9]+\.[a-z_]+' /var/chromium.log 2>/dev/null |
		sed 's/-> //; s/@[0-9]*//' | sort -u | tr '\n' ' ')"
	# The exchange in order, from the browser's own trace. Counting request
	# types says what it sent but not what it had been told first; a client
	# that receives no globals and one that receives them and declines to bind
	# produce the same empty bind count.
	echo "  globals received by the browser: $(grep -ac 'wl_registry.*global' /var/chromium.log 2>/dev/null)"
	# Which globals it took, and what its own Wayland layer said about the ones
	# it did not. It binds sixteen extensions and skips wl_compositor — without
	# which there is no surface to create — so its reasoning is the question.
	echo "  bound interfaces: $(grep -ao 'bind([0-9]*, "[a-z_0-9]*"' /var/chromium.log 2>/dev/null |
		sed 's/.*"//' | sort -u | tr '\n' ' ')"
	# How far up the browser got. Its Wayland layer initialises (it reports the
	# display it found), so the surface it never creates is not missing because
	# the platform failed — something above it never asked for a window.
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
	# Which globals it actually took. The control client binds fifteen; this one
	# binds one, and the difference between "did not want it" and "could not get
	# it" is the whole question.
	echo "  bound: $(grep -aE 'bind\(' /var/chromium.log 2>/dev/null | sed 's/.*bind(//' | cut -d, -f2 | sort -u | tr '\n' ' ')"
	echo "  wayland errors: $(grep -acE 'error|Error|refused|protocol' /var/chromium.log 2>/dev/null)"
	# What it says about windows. At --v=1 the window and widget layers narrate
	# their own decisions, and the question here is which decision it makes
	# instead of creating a surface.
	# The browser's own map, so a stack walk can be filtered against it: the
	# kernel cannot tell code from data here (pages are not marked
	# non-executable), and every candidate frame has to be checked against the
	# ranges that really are executable.
	echo "  chromium maps:"
	# By cmdline, not by pgrep: the browser's threads are named "pthread" and
	# its processes run as /proc/self/exe, so a name match finds the wrong
	# process — the last attempt printed the compositor's map instead.
	# Every process and what it is, first: two attempts to find the browser by
	# name found the compositor and then only its crash handler, because its
	# threads are named "pthread" and its children run as /proc/self/exe.
	echo "   all processes:"
	for pid in $(ls -d /proc/[0-9]* 2>/dev/null | sed 's|/proc/||'); do
		echo "     $pid: $(head -c 50 /proc/$pid/cmdline 2>/dev/null | tr '\0' ' ')"
	done
	for pid in $(ls -d /proc/[0-9]* 2>/dev/null | sed 's|/proc/||'); do
		if grep -aq chromium /proc/$pid/cmdline 2>/dev/null; then
			echo "   pid $pid ($(head -c 60 /proc/$pid/cmdline 2>/dev/null | tr '\0' ' ')):"
			grep -aE " r-xp " /proc/$pid/maps 2>/dev/null | head -24
		fi
	done
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
