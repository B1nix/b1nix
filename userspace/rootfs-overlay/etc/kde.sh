#!/bin/sh
# kde.sh — KDE's compositor on b1nix.
#
# Every other desktop here is wlroots (sway, cage). kwin_wayland is Qt6 with KDE
# Frameworks underneath, with its own input and session handling, so it
# exercises assumptions the wlroots stack never made.
#
# Backends:
#   drm       real modeset on the GPU, the same path sway takes.
#   virtual   an offscreen output, for a run with no display attached.
#   nested    kwin as a client of sway (b1nix.kde-nested), which proves KDE's
#             clients draw without needing a session or a card.
#
# Run from /etc/inittab, and only when the cmdline carries b1nix.kde.
grep -q "b1nix.kde" /proc/cmdline 2>/dev/null || exit 0

# A system bus, a udev database and a logind session on it.
#
# kwin's DRM backend does not open a graphics device itself: it asks logind for
# one, and refuses BEFORE any syscall when no session is registered. logind is
# elogind, reached from the SYSTEM bus (the session bus is a different bus and
# does not carry org.freedesktop.login1). The session itself is created by
# pam_elogind.so, so it needs a login that speaks PAM -- BusyBox's applet does
# not, util-linux's does, staged as /sbin/login-pam.
start_system_bus() {
	[ -x /usr/bin/dbus-daemon ] || return 1
	if [ ! -S /run/dbus/system_bus_socket ]; then
		mkdir -p /run/dbus
		dbus-daemon --system --fork > /tmp/kde-systembus.log 2>&1
		__i=0
		while [ $__i -lt 15 ] && [ ! -S /run/dbus/system_bus_socket ]; do
			__i=$((__i + 1)); sleep 1
		done
	fi
	if [ ! -S /run/dbus/system_bus_socket ]; then
		echo "KDE: fail system-bus t=$(up): $(tail -2 /tmp/kde-systembus.log 2>/dev/null | tr '\n' ' ')"
		return 1
	fi
	echo "KDE: ok system-bus t=$(up)"
	start_udev
	start_logind
}

# udev, because a seat is something udev builds.
#
# logind hands a graphics device to a session only when the udev database says
# the device belongs to that session's seat: the "seat" tag on
# /run/udev/data/c226:N. With no entry every card belongs to no seat and
# TakeDevice answers ENODEV without opening anything. The entry is written by
# elogind's own 71-seat.rules under /lib/udev/rules.d when udevd processes an
# "add"; devices that existed before udevd started are replayed by
# `udevadm trigger`, which writes to each /sys/**/uevent -- so the DRM minors'
# uevent files must be writable (see kernel/dev/drm.c).
start_udev() {
	__udevd=""
	for u in /sbin/udevd /lib/udev/udevd /usr/sbin/udevd /usr/lib/udev/udevd; do
		[ -x "$u" ] && { __udevd="$u"; break; }
	done
	if [ -z "$__udevd" ]; then
		echo "KDE: fail no-udevd t=$(up)"
		return 1
	fi
	mkdir -p /run/udev
	pgrep -f "[u]devd" > /dev/null 2>&1 || 		setsid "$__udevd" --daemon > /tmp/kde-udevd.log 2>&1
	__i=0
	while [ $__i -lt 10 ]; do
		[ -e /run/udev/control ] && break
		__i=$((__i + 1)); sleep 1
	done
	# The coldplug replay, bounded: `udevadm settle` waits on a queue that a
	# udevd which never started would never drain.
	udevadm trigger --action=add --subsystem-match=drm > /tmp/kde-trigger.log 2>&1
	udevadm trigger --action=add --subsystem-match=input >> /tmp/kde-trigger.log 2>&1
	udevadm settle --timeout=20 >> /tmp/kde-trigger.log 2>&1
	echo "KDE: udev db after trigger: [$(ls /run/udev/data 2>&1 | tr '\n' ' ' | cut -c1-160)]"
	if [ -e /run/udev/data/c226:1 ]; then
		echo "KDE: ok udev-tagged-card t=$(up)"
		return 0
	fi
	echo "KDE: fail udev-tagged-card t=$(up): $(tail -4 /tmp/kde-trigger.log 2>/dev/null | tr '\n' ' ' | cut -c1-200)"
	echo "KDE: udevd said: $(tail -6 /tmp/kde-udevd.log 2>/dev/null | tr '\n' ' ' | cut -c1-240)"
	return 1
}

# elogind, started rather than waited for: D-Bus activation does not happen
# here, and Alpine's OpenRC service runs exactly this command. Verify the NAME
# is on the bus rather than assuming the process implies it.
start_logind() {
	__el=""
	for e in /usr/libexec/elogind/elogind /usr/lib/elogind/elogind \
		 /usr/sbin/elogind /lib/elogind/elogind; do
		[ -x "$e" ] && { __el="$e"; break; }
	done
	if [ -z "$__el" ]; then
		echo "KDE: no elogind in image t=$(up)"
		return 1
	fi
	# Detached: it must outlive the login that is about to happen. It renames
	# itself elogind-daemon once running, so ask by path, not by name.
	pgrep -f "[e]logind --daemon" > /dev/null 2>&1 || \
		setsid env ELOGIND_LOG_LEVEL=debug SYSTEMD_LOG_LEVEL=debug \
			"$__el" --daemon > /tmp/kde-elogind.log 2>&1 &
	__i=0
	while [ $__i -lt 20 ]; do
		if dbus-send --system --dest=org.freedesktop.DBus \
			--type=method_call --print-reply /org/freedesktop/DBus \
			org.freedesktop.DBus.ListNames 2>/dev/null \
			| grep -q org.freedesktop.login1; then
			echo "KDE: ok logind t=$(up) after ${__i}s"
			return 0
		fi
		__i=$((__i + 1)); sleep 1
	done
	echo "KDE: fail logind t=$(up): $(tail -3 /tmp/kde-elogind.log 2>/dev/null | tr '\n' ' ')"
	return 1
}

# Re-enter this script inside a real login session, once.
#
# The marker file is how the second pass knows itself: login sanitises the
# environment, so an exported variable does not survive it. Output goes to
# /dev/console because login attaches to tty1 and every marker has to reach the
# serial log.
enter_session() {
	[ -n "${XDG_SESSION_ID:-}" ] && return 1
	[ -f /run/kde-session-attempted ] && return 1
	start_system_bus || return 1
	: > /run/kde-session-attempted

	# Nothing else may hold the VT this session claims: logind opens /dev/tty1
	# while taking control, and the getty /etc/inittab respawns holds the same
	# node.
	if pgrep -f "[g]etty.*tty1" > /dev/null 2>&1; then
		echo "KDE: getty holds tty1, stopping it t=$(up)"
		pkill -f "[g]etty.*tty1" 2>/dev/null
		sleep 1
	fi
	echo "KDE: tty1 held by: $(fuser /dev/tty1 2>&1 | tr '\n' ' ' | cut -c1-60)"

	# runuser with XDG_SEAT and XDG_VTNR named explicitly -- which is how a
	# display manager registers a session it is about to hand to a compositor.
	# Without them pam_elogind makes a session with no seat and no VT, and logind
	# gives graphics devices only to a session that owns them through a seat.
	#
	# This also steps around util-linux login, which is built against utmps and
	# talks to /run/utmps/.utmpd-socket; the s6 IPC server behind that socket is
	# not in this image, and login takes the failure as a length and faults in a
	# memset. It stays as the fallback: a session without a seat beats none.
	if [ -x /sbin/runuser ]; then
		echo "KDE: entering session via runuser (seat0, vt1) t=$(up)"
		exec setsid env XDG_SEAT=seat0 XDG_VTNR=1 \
			/sbin/runuser -l root -c "/bin/sh /etc/kde.sh" \
			> /dev/console 2>&1
	fi
	if [ -x /sbin/login-pam ]; then
		echo "KDE: entering session via login t=$(up)"
		exec setsid /sbin/login-pam -f root < /dev/tty1 > /dev/tty1 2>&1
	fi
	echo "KDE: no pam session tool in image t=$(up)"
	return 1
}

has_flag() {
	for tok in $(cat /proc/cmdline 2>/dev/null); do
		[ "$tok" = "$1" ] && return 0
	done
	return 1
}

up() { cut -d' ' -f1 /proc/uptime; }

echo "KDE: start t=$(up)"

export HOME=/root
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export XDG_RUNTIME_DIR=/run/user/0
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=KDE
mkdir -p /run/user/0 /tmp /root /var/lib/kwin
chmod 700 /run/user/0

# Qt has to be told where things are: on a normal KDE install these come from
# the session manager, which is not running here.
#
# NOT QT_QPA_PLATFORM: kwin_wayland installs a platform plugin of its own
# ("wayland-org.kde.kwin.qpa"), and pointing Qt at the ordinary wayland client
# platform asks it to connect to a compositor it has not started yet.
#
# The session and backend warnings are printed early, so a tail of the log
# never shows them; QT_DEBUG_PLUGINS is what makes a failed platform-plugin
# load say why.
export QT_LOGGING_RULES="*.debug=false;kwin_*=true;qt.qpa.*=true"
export QT_DEBUG_PLUGINS=1
export KWIN_COMPOSE=O2ES

if [ ! -x /usr/bin/kwin_wayland ]; then
	echo "KDE: fail no-kwin (build with B1NIX_KDE=1)"
	echo "KDE: done t=$(up)"
	exit 0
fi
echo "KDE: ok kwin-present t=$(up)"

# The renderer. kwin's OpenGL backend needs a working EGL on a render node,
# which iris provides once B1NIX_GPU_DRV=1 put it in the image; without one,
# QPainter composites in software and still produces a picture. The choice is
# made by /etc/render-select.sh, the same probe every compositor here uses.
# kwin reads KWIN_COMPOSE rather than WLR_RENDERER, so what is taken from the
# selection is the verdict, not the variable.
if has_flag b1nix.kdegl; then
	B1NIX_MESA_DRIVER=iris
	. /etc/render-select.sh
	render_select
	unset B1NIX_MESA_DRIVER
else
	RENDER_MODE=software
	RENDER_REASON=not-requested
fi
if [ "$RENDER_MODE" = accelerated ]; then
	export KWIN_COMPOSE=O2ES
	echo "KDE: renderer opengl ($RENDER_REASON, ${RENDER_GL_RENDERER:-unknown})"
else
	export KWIN_COMPOSE=Q
	echo "KDE: renderer qpainter (software: $RENDER_REASON)"
fi

# A message bus, because kwin asks logind for its devices over it -- "File
# descriptor for %s from logind is invalid" is the binary's own account of a
# missing bus. seatd does not help: kwin is not linked against libseat.
mkdir -p /run/dbus /var/lib/dbus /run/systemd
if [ -x /usr/bin/dbus-uuidgen ] && [ ! -s /var/lib/dbus/machine-id ]; then
	dbus-uuidgen > /var/lib/dbus/machine-id 2>/dev/null
	cp -f /var/lib/dbus/machine-id /etc/machine-id 2>/dev/null
fi
if [ -x /usr/bin/dbus-daemon ]; then
	# Our own configuration, because Alpine's system.conf drops privileges to a
	# "messagebus" user that its post-install script creates and this image never
	# runs; dbus-daemon then exits before binding anything. Same socket, same
	# policy shape, no identity change.
	cat > /etc/dbus-1/b1nix-system.conf <<'CONF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=/run/dbus/system_bus_socket</listen>
  <policy context="default">
    <allow user="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
  <includedir>/etc/dbus-1/system.d</includedir>
</busconfig>
CONF
	# Ask the bus a question rather than looking for the socket file: a socket
	# nobody listens on is exactly the case that has to be told apart. Starting a
	# second bus on the same path is how logind ended up alive and answering with
	# its name absent -- old clients keep the first bus, new ones reach the
	# second.
	__probe=$(dbus-send --system --dest=org.freedesktop.DBus --type=method_call \
		--print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>&1)
	if [ $? -ne 0 ]; then
		echo "KDE: dbus probe failed t=$(up): $(printf %s "$__probe" | head -2 | tr '\n' ' ')"
		echo "KDE: /run/dbus holds: $(ls -la /run/dbus 2>&1 | tr '\n' ' ' | cut -c1-160)"
		echo "KDE: daemons: $(pgrep -f "[d]bus-daemon" | tr '\n' ' ')"
	fi
	if printf %s "$__probe" | grep -q org.freedesktop.DBus; then
		echo "KDE: ok dbus t=$(up) (already running)"
	else
		# setsid, because this script re-enters itself through a PAM login and a
		# daemon left in the old session goes away with it.
		setsid dbus-daemon --config-file=/etc/dbus-1/b1nix-system.conf --fork \
			> /tmp/kde-dbus.log 2>&1
		i=0
		while [ $i -lt 15 ] && [ ! -S /run/dbus/system_bus_socket ]; do
			i=$((i + 1)); sleep 1
		done
		if [ -S /run/dbus/system_bus_socket ]; then
			echo "KDE: ok dbus t=$(up)"
		else
			echo "KDE: no dbus socket t=$(up): $(head -3 /tmp/kde-dbus.log 2>/dev/null | tr '\n' ' ')"
		fi
	fi
	export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket
else
	echo "KDE: no dbus-daemon in the image t=$(up)"
fi

# A seat for the DRM backend, for the compositors that do use libseat. The
# virtual backend needs none of it, so a failure here is only fatal to DRM.
if [ -x /usr/sbin/seatd ] || [ -x /usr/bin/seatd ]; then
	export LIBSEAT_BACKEND=seatd
	SEATD_VTBOUND=0 seatd -g root > /tmp/kde-seatd.log 2>&1 &
	i=0
	while [ $i -lt 20 ] && [ ! -S /run/seatd.sock ]; do i=$((i + 1)); sleep 1; done
	[ -S /run/seatd.sock ] && echo "KDE: ok seatd t=$(up)" \
	                       || echo "KDE: no seatd socket t=$(up)"
	echo "KDE: seatd says: $(tail -3 /tmp/kde-seatd.log 2>/dev/null | tr '\n' ' ')"
fi

# Which card, and what is on offer. Several DRM minors exist here -- b1nix's own
# device and the imported core's -- and only some can be opened for modesetting.
# Open each from this shell first: kwin reports "failed to open drm device"
# without saying whether the open or something after it failed.
echo "KDE: dri nodes: $(ls -1 /dev/dri 2>/dev/null | tr '\n' ' ')"
echo "KDE: node modes: $(ls -l /dev/dri/ 2>/dev/null | tr -s ' ' | cut -d' ' -f1,3,4,10 | tr '\n' ' ')"
for c in /dev/dri/card0 /dev/dri/card1 /dev/dri/card2; do
	[ -e "$c" ] || continue
	if (exec 3<>"$c") 2>/dev/null; then
		echo "KDE: open $c ok"
	else
		echo "KDE: open $c FAILED"
	fi
done
[ -x /bin/gpuinfo ] && echo "KDE: gpuinfo: $(/bin/gpuinfo 2>&1 | head -3 | tr '\n' ' ')"
echo "KDE: sys drm:   $(ls -1 /sys/class/drm 2>/dev/null | tr '\n' ' ')"
# A client to draw, so the screen has something on it that is not the
# compositor's own background.
CLIENT=""
[ -x /usr/bin/foot ] && CLIENT="/usr/bin/foot"

# Is this program still running?
#
# Every launch here is `timeout N prog &`, so $! is the timeout, not the
# program -- and timeout exits on its own while its child keeps running (a
# kernel defect of ours, tracked separately). Testing the recorded pid reports
# healthy programs as dead. Ask by name, and fall back to the pid.
prog_alive() {
	pgrep -x "$1" > /dev/null 2>&1 && return 0
	[ -n "${2:-}" ] && kill -0 "$2" 2>/dev/null
}

# Shut the session down from the top: clients first, then the compositor.
# Killing the compositor first leaves plasmashell holding a Wayland connection
# that has gone away, and Qt faults in libQt6Widgets during teardown.
stop_session() {
	for c in plasmashell kactivitymanagerd foot; do
		pkill -TERM -x "$c" 2>/dev/null
	done
	i=0
	while [ $i -lt 5 ] && pgrep -x plasmashell > /dev/null 2>&1; do
		i=$((i + 1)); sleep 1
	done
	kill "$@" 2>/dev/null
}

wait_kwin_socket() {
	export XDG_RUNTIME_DIR=/run/user/0
	__i=0
	while [ $__i -lt 40 ] && [ ! -S "$XDG_RUNTIME_DIR/${KWIN_SOCK:-wayland-1}" ]; do
		__i=$((__i + 1))
		sleep 1
	done
	if [ -S "$XDG_RUNTIME_DIR/${KWIN_SOCK:-wayland-1}" ]; then
		echo "KDE: ok kwin-socket t=$(up) after ${__i}s"
		return 0
	fi
	echo "KDE: fail kwin-socket t=$(up) (no $XDG_RUNTIME_DIR/${KWIN_SOCK:-wayland-1})"
	return 1
}

# The session bus. Plasma's components find each other over it -- kwin
# registers org.kde.KWin, plasmashell asks that name for a screen to put a panel
# on -- and with only a system bus present plasmashell exits during start-up. It
# must be up BEFORE kwin, so kwin has a DBUS_SESSION_BUS_ADDRESS to inherit;
# started afterwards, the shell reports "Did not find a valid screen to place a
# new panel".
start_session_bus() {
	[ -x /usr/bin/dbus-daemon ] || return 0
	cat > /etc/dbus-1/b1nix-session.conf <<'CONF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=/run/user/0/bus</listen>
  <policy context="default">
    <allow user="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
  <includedir>/etc/dbus-1/session.d</includedir>
</busconfig>
CONF
	setsid dbus-daemon --config-file=/etc/dbus-1/b1nix-session.conf --fork \
		> /tmp/kde-sessionbus.log 2>&1
	i=0
	while [ $i -lt 15 ] && [ ! -S /run/user/0/bus ]; do
		i=$((i + 1)); sleep 1
	done
	if [ -S /run/user/0/bus ]; then
		echo "KDE: ok session-bus t=$(up)"
	else
		echo "KDE: fail session-bus t=$(up): $(head -3 /tmp/kde-sessionbus.log 2>/dev/null | tr '\n' ' ')"
	fi
	export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus
}

# Where the memory and the seconds go. Plasma exhausts a 4 GB guest before it
# paints and needs 8. Sample the /proc/meminfo breakdown (Cached is the page
# cache, Slab the kernel heap, AnonPages the rest) with the biggest resident
# processes, stamped with the uptime so the samples read as a curve.
memsnap() {
	__tag=$1
	__mi=$(cat /proc/meminfo 2>/dev/null | tr '\n' ' ' | sed 's/  */ /g')
	echo "KDE: MEM[$__tag] t=$(up) $__mi"
	# Two heap samples far apart separate a leak (one size class growing) from a
	# high-water mark left by fragmentation (mapped far above live).
	case "$__tag" in
	kwin-up|t5|t20|scanout-end)
		echo "KDE: KHEAP[$__tag] t=$(up)"
		cat /proc/b1nix-kheap > /dev/null 2>&1
		;;
	esac
	for __p in /proc/[0-9]*; do
		__n=$(cat "$__p/comm" 2>/dev/null) || continue
		__r=$(awk '/^VmRSS:/{print $2}' "$__p/status" 2>/dev/null)
		[ -n "$__r" ] || continue
		echo "$__r $(basename "$__p") $__n"
	done 2>/dev/null | sort -rn | head -6 | while read -r __kb __pid __nm; do
		echo "KDE: RSS[$__tag] ${__kb}kB pid=$__pid $__nm"
	done
}

memsnap_loop() {
	__i=0
	while [ $__i -lt 40 ]; do
		sleep 5
		__i=$((__i + 1))
		memsnap "t$__i"
	done
}

start_plasma() {
if [ -x /usr/bin/plasmashell ]; then

	# The activity manager. Not optional: plasmashell aborts its load without it
	# ("The activity manager daemon (kactivitymanagerd) is not running"). Normally
	# D-Bus activated; started directly so the run does not depend on activation.
	# Alpine installs it under /usr/lib/libexec, not /usr/bin.
	KAMD=""
	for k in /usr/lib/libexec/kactivitymanagerd \
		 /usr/libexec/kactivitymanagerd \
		 /usr/lib/kactivitymanagerd \
		 /usr/bin/kactivitymanagerd; do
		[ -x "$k" ] && { KAMD="$k"; break; }
	done
	if [ -n "$KAMD" ]; then
		# offscreen, not wayland: it is a background service that owns a D-Bus
		# name and draws nothing. On the Wayland platform it initialises
		# wayland-egl, hits the same Mesa fault as every other Qt client here and
		# dies, taking the shell's start-up with it.
		QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
		LIBGL_ALWAYS_SOFTWARE=1 "$KAMD" > /tmp/kde-kamd.log 2>&1 &
		i=0
		while [ $i -lt 20 ]; do
			dbus-send --session --dest=org.freedesktop.DBus \
				--type=method_call --print-reply \
				/org/freedesktop/DBus \
				org.freedesktop.DBus.ListNames 2>/dev/null \
				| grep -q org.kde.ActivityManager && break
			i=$((i + 1)); sleep 1
		done
		if [ $i -lt 20 ]; then
			echo "KDE: ok activity-manager t=$(up) after ${i}s"
		else
			echo "KDE: fail activity-manager t=$(up): $(tail -3 /tmp/kde-kamd.log 2>/dev/null | tr '\n' ' ')"
		fi
	else
		echo "KDE: no kactivitymanagerd in the image t=$(up)"
	fi

	# Qt Quick on the CPU. plasmashell draws its panel with QML, and Qt's Wayland
	# client picks the wayland-egl buffer integration by default; with no GL
	# driver Mesa falls through to zink, Vulkan is absent and EGL init faults at a
	# null pointer inside Mesa. shm keeps Qt off that path entirely.
	WAYLAND_DISPLAY="${KWIN_SOCK:-wayland-2}" QT_QPA_PLATFORM=wayland \
	QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1 \
	QT_WAYLAND_CLIENT_BUFFER_INTEGRATION=shm \
		timeout 900 plasmashell --no-respawn \
			> /tmp/kde-plasmashell.log 2>&1 &
	PLASMAPID=$!
	i=0
	plasma_running() { prog_alive plasmashell $PLASMAPID; }
	# Wait for evidence that it PAINTED. Ask the COMPOSITOR, not the client:
	# kwin logs the interfaces it offers each client by executable as that client
	# binds them, so `of "/bin/plasmashell"` in kwin's log is one process
	# observing another. The plasmashell-side strings are kept only as a
	# fallback; they do not appear in this build.
	while [ $i -lt 45 ]; do
		plasma_running || break
		grep -aq 'of "/bin/plasmashell"' /tmp/kde-kwin.log 2>/dev/null && break
		grep -aq "backingstore\|QQuickWindow\|Loading the desktop" \
			/tmp/kde-plasmashell.log 2>/dev/null && break
		i=$((i + 1)); sleep 1
	done
	if [ $i -ge 45 ]; then
		echo "KDE: plasmashell-no-paint-within ${i}s t=$(up)"
	else
		echo "KDE: ok plasmashell-bound t=$(up) after ${i}s"
	fi
	if plasma_running; then
		echo "KDE: ok plasmashell-alive t=$(up) after ${i}s"
	else
		echo "KDE: fail plasmashell-died t=$(up)"
		echo "--- plasmashell log ---"
		tail -25 /tmp/kde-plasmashell.log 2>/dev/null
	fi
	# A witness window, so a black picture can be read. Black inside kwin but not
	# on the host compositor puts the fault in kwin's nested presentation; black
	# in both means client content is reaching no compositor at all.
	if [ -x /usr/bin/foot ]; then
		WAYLAND_DISPLAY="${KWIN_SOCK:-wayland-2}" foot > /tmp/kde-foot.log 2>&1 &
		[ -n "${HOST_SOCK:-}" ] && \
			WAYLAND_DISPLAY="$HOST_SOCK" foot > /tmp/kde-foot-host.log 2>&1 &
		sleep 8
	fi
	# Print what the shell said either way: logging it only on death is how a run
	# that produced a black window told us nothing.
	echo "--- plasmashell log (last 20) ---"
	tail -20 /tmp/kde-plasmashell.log 2>/dev/null
	echo "--- kwin log (last 15) ---"
	tail -15 /tmp/kde-kwin.log 2>/dev/null
	echo "--- end logs ---"
	sleep 10
else
	echo "KDE: no plasmashell in the image t=$(up)"
fi
}

# Nested: kwin as a client of a compositor that already works here. sway
# provides the output, kwin renders into a surface on it, grim takes the
# picture. This is also how KDE itself is developed.
if has_flag b1nix.kde-nested; then
	echo "KDE: nested under sway t=$(up)"
	export XDG_RUNTIME_DIR=/run/user/0
	HOST_SOCK=wayland-1
	KWIN_SOCK=wayland-2
	mkdir -p /etc/sway
	cat > /etc/sway/config <<'CFG'
output * bg #204060 solid_color
CFG
	WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
	WLR_HEADLESS_OUTPUTS=1 WAYLAND_DISPLAY=wayland-1 \
		sway > /tmp/kde-sway.log 2>&1 &
	SWAYPID=$!
	i=0
	while [ $i -lt 25 ] && [ ! -S /run/user/0/wayland-1 ]; do i=$((i+1)); sleep 1; done
	if [ ! -S /run/user/0/wayland-1 ]; then
		echo "KDE: fail no-host-compositor t=$(up)"
		echo "  sway said: $(tail -3 /tmp/kde-sway.log 2>/dev/null | tr '\n' ' ')"
		echo "KDE: done t=$(up)"
		exit 0
	fi
	echo "KDE: ok host-compositor t=$(up)"

	start_session_bus

	# What this build accepts, printed once: the nested backend is selected by
	# naming the host display, and there is no --wayland flag to go with it.
	echo "KDE: kwin options: $(timeout 15 kwin_wayland --help 2>&1 | grep -aoE '^\s+--[a-z-]+' | tr -d ' ' | tr '\n' ' ')"
	# 600 s, because the screenshot is taken past t=240 with plasmashell in the
	# sequence; a compositor killed earlier leaves the host's background in the
	# picture.
	WAYLAND_DISPLAY=wayland-1 timeout 600 kwin_wayland \
		--wayland-display wayland-1 \
		--width 1280 --height 720 --socket wayland-2 --no-lockscreen \
		${CLIENT:+"$CLIENT"} > /tmp/kde-kwin.log 2>&1 &
	KWINPID=$!
	i=0
	while [ $i -lt 40 ] && [ ! -S /run/user/0/wayland-2 ]; do i=$((i+1)); sleep 1; done
	if [ -S /run/user/0/wayland-2 ]; then
		echo "KDE: ok nested-socket t=$(up)"
	else
		echo "KDE: fail nested-socket t=$(up)"
		echo "--- kwin log (all of it) ---"
		tail -25 /tmp/kde-kwin.log 2>/dev/null
		echo "--- kwin log size: $(wc -c < /tmp/kde-kwin.log 2>/dev/null) ---"
		kill $SWAYPID 2>/dev/null
		echo "KDE: done t=$(up)"
		exit 0
	fi
	sleep 8
	prog_alive kwin_wayland $KWINPID && echo "KDE: ok nested-alive t=$(up)" \
	                                 || echo "KDE: fail nested-died t=$(up)"

	start_plasma
	# The picture: sway composites kwin's surface, grim reads sway's output. The
	# 180 s deadline is not slack -- a software-composited read-back through
	# wlr-screencopy on a loaded guest overran 30 s and grim came back
	# "Terminated".
	if command -v grim >/dev/null 2>&1; then
		if WAYLAND_DISPLAY=wayland-1 timeout 180 grim /tmp/kde-shot.png 2>/tmp/kde-grim.log; then
			echo "KDE: SHOT $(wc -c < /tmp/kde-shot.png) bytes t=$(up)"
			# Out through the serial log, so the host can reconstruct it.
			echo "KDE: SHOT-BEGIN"
			base64 /tmp/kde-shot.png 2>/dev/null || uuencode /tmp/kde-shot.png shot 2>/dev/null
			echo "KDE: SHOT-END"
		else
			echo "KDE: grim failed: $(head -2 /tmp/kde-grim.log 2>/dev/null)"
		fi
	fi
	sleep 5
	stop_session $KWINPID $SWAYPID
	echo "KDE: done t=$(up)"
	exit 0
fi

# The DRM path needs a session; get one before looking at cards. This
# re-executes the script inside a PAM login and does not return. The second pass
# arrives with XDG_SESSION_ID set and falls straight through.
enter_session

if [ -n "${XDG_SESSION_ID:-}" ]; then
	echo "KDE: ok login-session id=$XDG_SESSION_ID t=$(up)"
	# A session is not enough on its own: logind hands out devices to a session
	# that is Active, on a Seat that owns them. Print the properties rather than
	# treat "a session exists" as the answer -- and check logind is still there,
	# because nothing supervises it here.
	if ! pgrep -x elogind > /dev/null 2>&1; then
		echo "KDE: logind exited, its log says: $(tail -6 /tmp/kde-elogind.log 2>/dev/null | tr '\n' ' ')"
	fi
	echo "KDE: logind alive=$(pgrep -c -x elogind 2>/dev/null || echo 0) name=$(dbus-send --system \
		--dest=org.freedesktop.DBus --type=method_call --print-reply \
		/org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>/dev/null \
		| grep -c login1)"
	for lc in /usr/bin/loginctl /bin/loginctl /usr/sbin/loginctl; do
		[ -x "$lc" ] || continue
		echo "KDE: session props: $("$lc" show-session "$XDG_SESSION_ID" \
			-p Id -p Active -p State -p Seat -p VTNr -p Type -p Class \
			2>&1 | tr '\n' ' ')"
		echo "KDE: seats: $("$lc" list-seats 2>&1 | tr '\n' ' ')"
		break
	done
else
	echo "KDE: no login-session t=$(up) (kwin's DRM backend will not ask for a device)"
fi

# Only the cards the kernel actually publishes for discovery.
#
# /dev/dri/card0 is b1nix's own small DRM device, kept for the tests written
# against it, and the kernel deliberately leaves it out of /sys/class/drm so
# discovery does not see two devices for one GPU. logind looks a device up by
# number at /sys/dev/char/<major>:<minor>, so a card listed under
# /sys/class/drm with no entry there is still ENODEV -- card0 is exactly that.
CARDS=""
for __c in /sys/class/drm/card[0-9]*; do
	__n=$(basename "$__c" 2>/dev/null)
	case "$__n" in *-*) continue ;; esac
	[ -e "/dev/dri/$__n" ] || continue
	__dev=$(cat "$__c/dev" 2>/dev/null)
	[ -n "$__dev" ] && [ -e "/sys/dev/char/$__dev" ] || continue
	CARDS="$CARDS /dev/dri/$__n"
done
CARDS="${CARDS# }"
[ -n "$CARDS" ] || CARDS="$(ls -1 /dev/dri/card* 2>/dev/null | tr '\n' ' ')"
CARD="$(echo $CARDS | awk '{print $1}')"

if [ -n "$CARD" ] && ! has_flag b1nix.kde-virtual; then
	echo "KDE: backend drm, candidates: $CARDS t=$(up)"
	DRM_CANDIDATES="$CARDS"
else
	# Name the backend. This branch announced "virtual" and then launched
	# kwin_wayland with $BACKEND unset, so it passed no backend flag at all
	# and kwin chose for itself -- which on the DRM path it was trying to
	# avoid is the one thing this mode exists not to do.
	BACKEND="--virtual"
	echo "KDE: backend virtual t=$(up)"
fi

# The seat, and why it is not seat0.
#
# kwin's direct session -- the one it uses when there is no logind -- treats
# "seat0" as the seat that owns the console and then insists on a virtual
# terminal: it opens /dev/tty0 and puts it in graphics mode. b1nix has no VTs,
# so no session object is created and every device open is refused before it
# reaches the driver. Any other seat name skips the VT dance entirely.
export XDG_SEAT=seat1
echo "KDE: tty0 present: $([ -e /dev/tty0 ] && echo yes || echo no)"

export QT_PLUGIN_PATH=/usr/lib/qt6/plugins
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt6/plugins/platforms

# Does the Qt runtime work here at all? Bounded, because --version once
# returned nothing and never came back: whatever blocks there blocks everything
# after it.
if timeout 20 kwin_wayland --version > /tmp/kde-version.log 2>&1; then
	echo "KDE: kwin says: $(head -1 /tmp/kde-version.log 2>/dev/null)"
else
	echo "KDE: fail version-timeout (Qt cannot start) t=$(up)"
	echo "--- version attempt ---"
	head -20 /tmp/kde-version.log 2>/dev/null
fi

# On the DRM path each candidate gets a turn: a card that cannot be opened for
# modesetting is not a failure of the compositor.
if [ -n "${DRM_CANDIDATES:-}" ]; then
	start_session_bus
	for c in $DRM_CANDIDATES; do
		echo "KDE: trying $c t=$(up)"
		export KWIN_DRM_DEVICES=$c
		# A previous candidate may have created the socket before giving up, and a
		# leftover socket makes the next one look successful.
		rm -f /run/user/0/wayland-1
		export QT_LOGGING_RULES="kwin_*.debug=true"
		timeout 900 /usr/bin/kwin_wayland --drm --socket wayland-1 \
			--no-lockscreen > /tmp/kde-kwin.log 2>&1 &
		KWINPID=$!
		w=0
		while [ $w -lt 15 ]; do
			[ -S /run/user/0/wayland-1 ] && break
			kill -0 $KWINPID 2>/dev/null || break
			sleep 1
			w=$((w + 1))
		done
		if [ -S /run/user/0/wayland-1 ] && kill -0 $KWINPID 2>/dev/null &&
		   ! grep -aq "No suitable DRM devices" /tmp/kde-kwin.log 2>/dev/null &&
		   ! grep -aq "failed to open drm device" /tmp/kde-kwin.log 2>/dev/null; then
			echo "KDE: ok drm-card $c t=$(up)"
			break
		fi
		echo "KDE: $c did not work: $(grep -a "kwin_wayland_drm" /tmp/kde-kwin.log | tail -2 | tr '\n' ' ')"
		echo "KDE: $c session: $(grep -aiE "session|logind|ConsoleKit|seat" /tmp/kde-kwin.log | tail -4 | tr '\n' ' ')"
		kill $KWINPID 2>/dev/null
		KWINPID=""
	done
	if [ -z "${KWINPID:-}" ]; then
		echo "KDE: fail no-usable-drm-card t=$(up)"
		echo "--- kwin log (last attempt, in full) ---"
		cat /tmp/kde-kwin.log 2>/dev/null
		echo "--- end kwin log ---"
		echo "KDE: done t=$(up)"
		exit 0
	fi
	sleep 5
	prog_alive kwin_wayland $KWINPID && echo "KDE: ok alive t=$(up)" \
	                                 || echo "KDE: fail died t=$(up)"

	KWIN_SOCK=wayland-1
	wait_kwin_socket
	# The memory profiler, behind a flag now that it has done its job.
	#
	# It found the allocator bug it was written for (the kernel heap never
	# split a block it reused), and it is not free: the loop spawns cat, tr,
	# sort and head every five seconds for the whole run, which is a
	# measurable part of the time to a drawn desktop on a machine this slow.
	# Kept, because the next memory question will want it, and off by default
	# because the current one is answered.
	if has_flag b1nix.kde-memprof; then
		memsnap "kwin-up"
		memsnap_loop &
		__memloop=$!
	fi
	start_plasma
	has_flag b1nix.kde-memprof && memsnap "plasma-started"

	# The picture is taken from OUTSIDE, with the QEMU monitor's screendump on
	# the scanout kwin programmed -- nothing in the guest produces it, so nothing
	# in the guest can fake it. These two markers bracket the window in which the
	# framebuffer is worth capturing; the host watches the serial log for them.
	has_flag b1nix.kde-memprof && memsnap "scanout-ready"
	echo "KDE: SCANOUT-READY t=$(up)"
	sleep 90
	echo "KDE: SCANOUT-END t=$(up)"
	has_flag b1nix.kde-memprof && memsnap "scanout-end"
	[ -n "${__memloop:-}" ] && kill $__memloop 2>/dev/null
	# The DRM backend's own account first and in full: the head of the log is
	# Qt's plugin inventory, hundreds of lines that push every line about outputs,
	# connectors and page flips past any cut.
	echo "--- kwin drm ---"
	grep -a "kwin_wayland_drm\|kwin_scene\|kwin_screencast\|DrmGpu\|drmMode\|kwin_core: Failed\|No suitable\|connector\|Connector\|modeset\|page flip\|pageflip" \
		/tmp/kde-kwin.log 2>/dev/null | tail -60
	echo "--- kwin log ---"
	grep -av "MetaData\|Keys\|IID\|qt.qpa.plugin: Found\|ELF load\|elf: load" \
		/tmp/kde-kwin.log 2>/dev/null | head -60
	kill $KWINPID 2>/dev/null
	echo "KDE: done t=$(up)"
	exit 0
fi

# The virtual backend. kwin does not speak wlr-screencopy, so grim cannot read
# it; the frame is what the emulator's display shows and the host takes it
# through the QEMU monitor. The guest's job is to keep rendering long enough to
# be seen.
echo "KDE: starting kwin_wayland t=$(up)"
if [ -n "$CLIENT" ]; then
	timeout 120 /usr/bin/kwin_wayland $BACKEND --socket wayland-1 --no-lockscreen \
		"$CLIENT" > /tmp/kde-kwin.log 2>&1 &
else
	/usr/bin/kwin_wayland $BACKEND --socket wayland-1 --no-lockscreen \
		> /tmp/kde-kwin.log 2>&1 &
fi
KWINPID=$!

w=0
while [ $w -lt 60 ]; do
	[ -S /run/user/0/wayland-1 ] && break
	sleep 1
	w=$((w + 1))
done

if [ -S /run/user/0/wayland-1 ]; then
	echo "KDE: ok socket t=$(up)"
else
	echo "KDE: fail no-socket t=$(up)"
	echo "--- kwin log ---"
	tail -60 /tmp/kde-kwin.log 2>/dev/null
	echo "KDE: done t=$(up)"
	exit 0
fi

sleep 5
prog_alive kwin_wayland $KWINPID && echo "KDE: ok alive t=$(up)" \
                                 || echo "KDE: fail died t=$(up)"

# kwin's own socket differs by backend: wayland-2 nested inside sway,
# wayland-1 when it owns the display. The shell must connect to KWIN.
KWIN_SOCK=wayland-1
export KWIN_SOCK
wait_kwin_socket
start_plasma

echo "KDE: holding the session t=$(up)"
sleep 60

echo "--- kwin log tail ---"
tail -30 /tmp/kde-kwin.log 2>/dev/null
stop_session $KWINPID
echo "KDE: done t=$(up)"
