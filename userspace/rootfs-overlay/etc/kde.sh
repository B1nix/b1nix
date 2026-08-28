#!/bin/sh
# kde.sh — KDE's compositor on b1nix.
#
# Every desktop this system has run so far has been wlroots: sway, cage, and the
# clients around them. kwin_wayland is a different animal — Qt6 for its
# rendering and its event loop, KDE Frameworks underneath, its own input and
# session handling — and that is the reason to run it. A second compositor
# stack exercises assumptions the first one never made, and the ones it breaks
# on are ours.
#
# Two backends, in order of what each proves:
#
#   drm       real modeset on the GPU, the same path sway takes.
#   virtual   an offscreen output, for a run with no display attached. kwin
#             renders into it and its own screenshot interface reads it back,
#             so a picture is available without a monitor or a passthrough.
#
# Run from /etc/inittab, and only when the cmdline carries b1nix.kde.
grep -q "b1nix.kde" /proc/cmdline 2>/dev/null || exit 0

# A system bus, and a logind session on it.
#
# kwin's DRM backend does not open a graphics device itself: it asks logind for
# one. With no session registered, its session object is a stub that refuses,
# and it refuses BEFORE any syscall -- a kernel trace of every open() under
# /dev/dri across a whole boot recorded five, all of them from this script's
# own probes and none from the compositor. So "kwin cannot open the card" was
# never about the card.
#
# logind is elogind here, D-Bus activated from the SYSTEM bus (the session bus
# above is a different bus and does not carry org.freedesktop.login1). The
# session itself is created by pam_elogind.so, which /etc/pam.d/base-session
# already includes -- it needs a login that speaks PAM, and BusyBox's applet
# does not. util-linux's does, staged beside it as /sbin/login-pam.
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

# elogind, started rather than waited for.
#
# The D-Bus service file activates it on demand, and that did not happen here:
# pam_elogind reported "Failed to create session: The name
# org.freedesktop.login1 was not provided", which is a lookup that found no
# provider, not a daemon that failed to start. Distributions do not rely on
# activation either -- Alpine's OpenRC service runs exactly this command -- so
# run it, and then verify the NAME is on the bus rather than assuming the
# process implies it.
# udev, because a seat is something udev builds.
#
# logind hands a graphics device to a session only when that device belongs to
# the session's seat, and it decides that from the udev database: the tag
# "seat" on /run/udev/data/c226:N. Nothing in this image was writing that file,
# so every card was catalogued as belonging to no seat and TakeDevice answered
# ENODEV without ever opening anything.
#
# The database entry is not something to write by hand. It is produced by
# elogind's own 71-seat.rules matching SUBSYSTEM=="drm", KERNEL=="card*" --
# rules already installed under /lib/udev/rules.d -- when udevd processes an
# "add" for the device. Devices that existed before udevd started are replayed
# by `udevadm trigger`, which writes "add" to each /sys/**/uevent, so the DRM
# minors' uevent files have to be writable for the replay to reach them (they
# now are; see kernel/dev/drm.c).
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
	# The coldplug replay. Bounded, because udevadm settle waits on a queue
	# that a udevd which never started would never drain, and an unbounded wait
	# there is a boot that never continues.
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
	# Also detached: it must outlive the login that is about to happen, and it
	# names itself elogind-daemon once running, so ask by path rather than by
	# the name it no longer has.
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
# environment, so an exported variable does not survive it. Output is sent to
# /dev/console because login attaches to tty1, and every marker this script
# prints has to reach the serial log to be checked.
enter_session() {
	[ -n "${XDG_SESSION_ID:-}" ] && return 1
	[ -f /run/kde-session-attempted ] && return 1
	start_system_bus || return 1
	: > /run/kde-session-attempted

	# What login asks the kernel about itself, before asking it.
	#
	# login faulted with a string that had no terminator; the usual source of
	# one is a kernel call that reports a length it did not write, and
	# ttyname() -- readlink of /proc/self/fd/0, then stat of the result -- is
	# the call login makes first. Printing both answers costs nothing and says
	# whether they are well-formed, rather than leaving "probably not ours" as
	# an assumption.
	echo "KDE: probe fd0=[$(readlink /proc/self/fd/0 2>&1)] tty=[$(tty 2>&1)]"
	echo "KDE: probe tty1-fd0=[$( (exec 0</dev/tty1; readlink /proc/self/fd/0) 2>&1)]"
	echo "KDE: probe stat-tty1=[$(stat -c '%F %t:%T' /dev/tty1 2>&1)]"

	# runuser before login, because login's tty handling is where this broke.
	#
	# Both create a session through the same pam_elogind.so; the difference is
	# everything else login does. /sbin/login-pam faulted with its frame
	# pointer holding the bytes " root" and a pointer walked to
	# 0x800000000000 -- the first address past the user half -- which is a
	# string copied without a terminator, in its own tty/utmp handling, after
	# the session had already reached cgroup setup. runuser needs none of that
	# machinery, so it reaches the same result by a shorter road.
	# login first, because only it gives the session a seat and a VT.
	#
	# runuser creates a session too, and it worked -- but with no controlling
	# terminal it has no VT and no seat, and logind hands graphics devices to a
	# session that owns them through a seat. So the shorter road does not
	# arrive: it produced Id=c1 and kwin still asked for nothing.
	#
	# login's earlier fault is no longer expected to stand: every VT reported
	# st_rdev 0:0, a device file that was no device, and identifying the
	# terminal is exactly what login does at that point. runuser stays as the
	# fallback, because a session without a seat is still better than none.
	# Nothing else may be holding the VT this session claims.
	#
	# logind opens /dev/tty1 while taking control, and that open never
	# returned -- the task dump shows the daemon blocked in syscall 2 waiting
	# on an inode lock. A getty is respawned on tty1 by /etc/inittab and holds
	# the same node. Whether the block is contention on that node or something
	# else is exactly what this separates: with no getty there, an open that
	# still hangs is not about sharing the terminal.
	if pgrep -f "[g]etty.*tty1" > /dev/null 2>&1; then
		echo "KDE: getty holds tty1, stopping it t=$(up)"
		pkill -f "[g]etty.*tty1" 2>/dev/null
		sleep 1
	fi
	echo "KDE: tty1 held by: $(fuser /dev/tty1 2>&1 | tr '\n' ' ' | cut -c1-60)"

	# Name the seat and the VT, and runuser is enough.
	#
	# pam_elogind takes XDG_SEAT and XDG_VTNR from the environment when the
	# caller supplies them -- which is how a display manager registers a
	# session it is about to hand to a compositor. Without them runuser
	# produced a session with no seat and no VT, and logind hands graphics
	# devices to a session that owns them through a seat, so kwin still asked
	# for nothing.
	#
	# This also steps around util-linux login entirely. That binary is built
	# against utmps and talks to /run/utmps/.utmpd-socket; the daemons behind
	# that socket are started by an s6 IPC server, which this image does not
	# have, and login takes the failure as a length -- it faults in a memset
	# whose count is 0xffffffffffffffe7. Running the utmps daemons is the
	# proper repair for login and is worth doing on its own; it is not on the
	# path to a compositor.
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

# Everything logind inspects before it opens a graphics device.
#
# TakeDevice(226,1) answers ENODEV while logind never opens the node, so the
# refusal happens while it is resolving the device. That resolution is
# sd-device's, and udevadm runs the same library: if udevadm can describe the
# card, logind's lookup of it is not what fails, and if it cannot, its error
# names the step. Ask both, plus the pieces sd-device reads by hand, and call
# the method itself so the answer is the daemon's own.
probe_logind_device() {
	__sp=$1
	echo "KDE: --- logind device probe ---"
	for __c in /sys/class/drm/card[0-9]*; do
		__n=$(basename "$__c" 2>/dev/null)
		case "$__n" in *-*) continue ;; esac
		__syspath=$(readlink -f "$__c" 2>/dev/null)
		echo "KDE: probe $__n syspath=[$__syspath]"
		echo "KDE: probe $__n dev=[$(cat "$__c/dev" 2>&1 | tr -d '\n')] subsystem-link=[$(readlink "$__c/subsystem" 2>&1)]"
		echo "KDE: probe $__n subsystem-resolves=[$(readlink -f "$__c/subsystem" 2>&1)]"
		echo "KDE: probe $__n uevent=[$(cat "$__c/uevent" 2>&1 | tr '\n' ' ')]"
		echo "KDE: probe $__n entries=[$(ls "$__c/" 2>&1 | tr '\n' ' ' | cut -c1-160)]"
	done
	# The udev database, which is where the seat tags live. A card with no
	# entry has no ID_SEAT and no master-of-seat tag, and logind decides both
	# from there.
	echo "KDE: probe udev-db=[$(ls /run/udev/data 2>&1 | tr '\n' ' ' | cut -c1-200)]"
	for __d in c226:0 c226:1 c226:128; do
		[ -e "/run/udev/data/$__d" ] || continue
		echo "KDE: probe db $__d=[$(cat "/run/udev/data/$__d" 2>&1 | tr '\n' ' ' | cut -c1-200)]"
	done
	# sd-device's own answer, from the tool that links the same library.
	for __u in /bin/udevadm /sbin/udevadm /usr/bin/udevadm /usr/sbin/udevadm; do
		[ -x "$__u" ] || continue
		echo "KDE: probe udevadm-by-name=[$("$__u" info --query=all --name=/dev/dri/card1 2>&1 | tr '\n' ' ' | cut -c1-260)]"
		echo "KDE: probe udevadm-by-path=[$("$__u" info --query=all --path=/sys/dev/char/226:1 2>&1 | tr '\n' ' ' | cut -c1-260)]"
		echo "KDE: probe udevadm-drm-enum=[$("$__u" info --export-db 2>/dev/null | grep -c . )] lines"
		break
	done
	# The seat logind believes this session sits on, and what it believes that
	# seat can do. A seat with no graphical device is a seat no compositor is
	# given a card by.
	for __lc in /usr/bin/loginctl /bin/loginctl /usr/sbin/loginctl; do
		[ -x "$__lc" ] || continue
		echo "KDE: probe seat-status=[$("$__lc" seat-status seat0 2>&1 | tr '\n' ' ' | cut -c1-260)]"
		break
	done
	# And the method itself, so the error is logind's rather than inferred.
	for __mm in 226:1 226:0 226:128; do
		__maj=${__mm%%:*}; __min=${__mm##*:}
		[ -e "/sys/dev/char/$__mm" ] || continue
		echo "KDE: probe TakeDevice($__maj,$__min)=[$(dbus-send --system --print-reply \
			--dest=org.freedesktop.login1 "$__sp" \
			org.freedesktop.login1.Session.TakeDevice \
			uint32:"$__maj" uint32:"$__min" 2>&1 | tr '\n' ' ' | cut -c1-200)]"
	done
	# Can the seat draw, as logind sees it?
	#
	# CanGraphical is read straight out of logind's own device map: it is true
	# only when a device tagged master-of-seat has been attached to the seat.
	# False says the refusal is about how logind catalogues the card, not about
	# the file descriptor it would hand over.
	for __bc in /usr/bin/busctl /bin/busctl; do
		[ -x "$__bc" ] || continue
		echo "KDE: probe CanGraphical=[$("$__bc" get-property org.freedesktop.login1 \
			/org/freedesktop/login1/seat/seat0 \
			org.freedesktop.login1.Seat CanGraphical 2>&1 | tr '\n' ' ' | cut -c1-100)]"
		break
	done
	# What udev made of the card, which is what logind reads.
	#
	# The database entry is the whole of the seat association: no entry, no
	# "seat" tag, and logind drops the device while cataloguing it and reports
	# that as ENODEV without opening anything. This used to be written here by
	# hand to prove that was the cause; it is written by udevd now, and this
	# only reports what is there.
	for __mm in 226:1 226:0; do
		[ -e "/run/udev/data/c$__mm" ] || continue
		echo "KDE: probe udev-entry c$__mm=[$(cat "/run/udev/data/c$__mm" 2>&1 | tr '\n' ' ' | cut -c1-200)]"
	done
	echo "KDE: probe logind-tail=[$(tail -25 /tmp/kde-elogind.log 2>/dev/null | tr '\n' ' ' | cut -c1-900)]"
	echo "KDE: --- end logind device probe ---"
}


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
# NOT QT_QPA_PLATFORM: kwin_wayland is the compositor, and it installs a
# platform plugin of its own ("wayland-org.kde.kwin.qpa"). Telling Qt to use the
# ordinary wayland client platform on top of that asks it to connect to a
# compositor it has not started yet.
# Everything, including the categories that carry session and backend
# warnings. The plugin-loader chatter is noise, but the one line that says why a
# device was refused has been in it all along — and it is printed early, which
# is why a tail of this log never showed it.
export QT_LOGGING_RULES="*.debug=false;kwin_*=true;qt.qpa.*=true"
# What the plugin loader decides, and why. A compositor that stops after
# "Attempting to load Qt platform plugin" has failed inside that load, and
# without this the reason is not printed at all.
export QT_DEBUG_PLUGINS=1
export KWIN_COMPOSE=O2ES

if [ ! -x /usr/bin/kwin_wayland ]; then
	echo "KDE: fail no-kwin (build with B1NIX_KDE=1)"
	echo "KDE: done t=$(up)"
	exit 0
fi
echo "KDE: ok kwin-present t=$(up)"

# The renderer. kwin's OpenGL backend needs a working EGL on a render node,
# which is what iris provides once B1NIX_GPU_DRV=1 put it in the image; without
# one, QPainter composites in software and still produces a picture.
# The choice is made by the same probe every compositor here uses
# (/etc/render-select.sh): a render node, a driver behind it, and a driver that
# really renders. kwin reads KWIN_COMPOSE rather than WLR_RENDERER, so what is
# taken from the selection is the verdict, not the variable.
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

# A message bus and a session manager, because kwin asks logind for its devices.
#
# The binary says so outright — "File descriptor for %s from logind is invalid"
# — and that is the whole explanation for a compositor that reports "failed to
# open drm device" without the kernel ever seeing an open: the request goes over
# D-Bus to logind, and with neither running there is nothing to answer it. seatd
# does not help here; kwin is not linked against libseat at all.
mkdir -p /run/dbus /var/lib/dbus /run/systemd
if [ -x /usr/bin/dbus-uuidgen ] && [ ! -s /var/lib/dbus/machine-id ]; then
	dbus-uuidgen > /var/lib/dbus/machine-id 2>/dev/null
	cp -f /var/lib/dbus/machine-id /etc/machine-id 2>/dev/null
fi
if [ -x /usr/bin/dbus-daemon ]; then
	# Our own configuration, because Alpine's system.conf drops privileges to a
	# "messagebus" user that its post-install script creates and this image
	# never runs. dbus-daemon then exits before it binds anything, and every
	# client — kwin included — reports only that the service it wanted is not
	# there. Same socket, same policy shape, no identity change.
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
	# Not a second one over the first.
	#
	# This script re-enters itself inside a login session, and the whole
	# preamble ran again -- starting another system bus on the same socket
	# path. The daemons already connected to the first one (logind among them)
	# keep their connections and go on working, while every new client reaches
	# the second bus, which has never heard of org.freedesktop.login1. That is
	# exactly what was seen: elogind alive and answering, its name absent, and
	# kwin -- a new client -- unable to ask anyone for a device.
	#
	# Tested by asking the bus a question, not by looking for the socket file:
	# a socket that no one is listening on is exactly the case this has to
	# tell apart, and it fooled this script once already today.
	# Say why, when it says no.
	#
	# Two guesses have already been wrong here -- that the bus dies with the
	# login's session, and that setsid would save it -- so print the refusal
	# itself and what is actually on disk, rather than reasoning about it a
	# third time.
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
		# In its own session, so the login below cannot take it down.
		#
		# This script re-enters itself through a PAM login, which puts the new
		# shell in a new session; a daemon left in the old one goes away with
		# it, and the second pass then found no bus answering and started
		# another. seatd showed the same thing plainly -- "Removing leftover
		# socket" on its restart. setsid detaches them from the session that
		# is about to be replaced.
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

# elogind is deliberately NOT started.
#
# kwin picks its session backend in order: logind, ConsoleKit, then its own
# direct session. With elogind running, the first of those succeeds in being
# FOUND and then fails to answer -- there is no registered user session on this
# machine, because there is no PAM login -- so kwin reports "Could not determine
# the active graphical session", Session::create() returns nothing, and every
# device open is refused before it reaches the driver. That is the whole
# explanation for "failed to open drm device" naming a node the shell opens
# perfectly well.
#
# With no logind on the bus at all, kwin falls through to the direct session,
# which is root opening the device itself -- which is exactly what this image
# is. A service that half-answers is worse here than one that is absent.

# A seat, for the DRM backend. kwin opens devices through libseat exactly as
# wlroots does, and with no logind on this system seatd is what answers. The
# virtual backend needs none of it, so a failure here is only fatal to the DRM
# path.
if [ -x /usr/sbin/seatd ] || [ -x /usr/bin/seatd ]; then
	export LIBSEAT_BACKEND=seatd
	SEATD_VTBOUND=0 seatd -g root > /tmp/kde-seatd.log 2>&1 &
	i=0
	while [ $i -lt 20 ] && [ ! -S /run/seatd.sock ]; do i=$((i + 1)); sleep 1; done
	[ -S /run/seatd.sock ] && echo "KDE: ok seatd t=$(up)" \
	                       || echo "KDE: no seatd socket t=$(up)"
	echo "KDE: seatd says: $(tail -3 /tmp/kde-seatd.log 2>/dev/null | tr '\n' ' ')"
fi

# Which card, and what is on offer.
#
# Several DRM minors exist here — b1nix's own device and the imported core's —
# and only some of them can be opened for modesetting. Naming one by hand hides
# which, so list what there is and let the compositor be pointed at each in turn
# until one works.
echo "KDE: dri nodes: $(ls -1 /dev/dri 2>/dev/null | tr '\n' ' ')"
echo "KDE: node modes: $(ls -l /dev/dri/ 2>/dev/null | tr -s ' ' | cut -d' ' -f1,3,4,10 | tr '\n' ' ')"
# Can the node be opened read-write at all, by this very shell? kwin reports
# "failed to open drm device" without saying whether the open or something
# after it failed, and those are different problems.
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

# Nested: kwin as a client of a compositor that already works here.
#
# kwin's DRM backend takes its devices from logind, and logind hands them out
# only to a registered user session — which needs PAM and a login, neither of
# which this image has. That is a userspace prerequisite, not a kernel gap, and
# it says nothing about whether KDE can draw. Nested says exactly that: sway
# provides the output, kwin renders into a surface on it, and grim — which
# already produces real screenshots here — takes the picture. It is also how
# KDE is developed: kwin_wayland --wayland inside another session.
# Wait for kwin's socket, do not guess at a delay.
#
# A sleep is enough for kwin to be RUNNING and not enough for it to have
# finished a modeset and published its Wayland socket -- on a real device that
# is EDID, an atomic commit and a page flip. Started too early, plasmashell
# says only "Failed to create wl_display (Connection refused)" and dies, and
# the run reports a compositor with no desktop.
# Is this program still running?
#
# Every launch here is `timeout N prog &`, so $! is the timeout, not the
# program -- and timeout exits on its own while its child keeps running (a
# kernel defect of ours, tracked separately). Testing the recorded pid
# therefore reports healthy programs as dead: it claimed kwin died at 19.7 s in
# runs whose own kwin log went on for another twenty seconds, and claimed the
# same of plasmashell. Ask after the program by name, and fall back to the pid
# only when the name is not found.
prog_alive() {
	pgrep -x "$1" > /dev/null 2>&1 && return 0
	[ -n "${2:-}" ] && kill -0 "$2" 2>/dev/null
}

# Shut the session down from the top.
#
# Killing the compositor first leaves plasmashell holding a Wayland connection
# that has gone away, and Qt does not survive that: it faulted in
# libQt6Widgets during teardown, a second after the run had already reported
# done. That fault says nothing about this kernel -- it is a client losing its
# server -- but it is noise in every log, and the order is wrong regardless.
# Clients first, then the compositor that serves them.
stop_session() {
	for c in plasmashell kactivitymanagerd foot; do
		pkill -TERM -x "$c" 2>/dev/null
	done
	# A moment to let them close their surfaces before the server goes.
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

# Start the desktop itself: the activity manager the shell refuses to load
# without, then plasmashell. Used by every backend -- the desktop does not
# depend on how kwin got its output, and a copy per path would drift.
# The session bus. Plasma's components find each other over it -- kwin
# registers org.kde.KWin, plasmashell asks that name for the screen to put a
# panel on, kactivitymanagerd owns a name of its own -- and with only a system
# bus present plasmashell exits during start-up instead of drawing. It has to
# be up BEFORE kwin, so kwin has a DBUS_SESSION_BUS_ADDRESS to inherit; started
# afterwards, kwin registers nothing and the shell reports "Did not find a valid
# screen to place a new panel".
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

# Where the memory and the seconds go.
#
# Plasma exhausts a 4 GB guest before it paints and needs 8; and it takes tens
# of seconds to get there. Neither number means anything on its own -- the
# question is whether the kernel is holding memory it should give back, and
# whether the time is spent in the kernel or in Qt. So sample the breakdown
# /proc/meminfo now reports (Cached is the page cache, Slab the kernel heap,
# AnonPages everything else) alongside the biggest resident processes, and
# stamp every sample with the uptime so the samples can be read as a curve.
#
# Cheap enough to leave running: one read of /proc/meminfo and one pass over
# /proc/*/status every few seconds.
memsnap() {
	__tag=$1
	__mi=$(cat /proc/meminfo 2>/dev/null | tr '\n' ' ' | sed 's/  */ /g')
	echo "KDE: MEM[$__tag] t=$(up) $__mi"
	# What the heap is holding, at a few chosen moments. Two samples far apart
	# are what separates a leak (one size class growing) from a high-water mark
	# left by fragmentation (mapped far above live).
	case "$__tag" in
	kwin-up|t5|t20|scanout-end)
		echo "KDE: KHEAP[$__tag] t=$(up)"
		cat /proc/b1nix-kheap > /dev/null 2>&1
		;;
	esac
	# The five largest resident processes, which is where an answer of the
	# form "Plasma simply wants this much" would show itself.
	for __p in /proc/[0-9]*; do
		__n=$(cat "$__p/comm" 2>/dev/null) || continue
		__r=$(awk '/^VmRSS:/{print $2}' "$__p/status" 2>/dev/null)
		[ -n "$__r" ] || continue
		echo "$__r $(basename "$__p") $__n"
	done 2>/dev/null | sort -rn | head -6 | while read -r __kb __pid __nm; do
		echo "KDE: RSS[$__tag] ${__kb}kB pid=$__pid $__nm"
	done
}

# A sampler that keeps going while the desktop starts, so the curve covers the
# part nobody is watching.
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

	# The activity manager. Not an optional extra: plasmashell refuses to
	# load without it -- "Aborting shell load: The activity manager daemon
	# (kactivitymanagerd) is not running" -- which is why the shell ran and
	# drew nothing at all. Normally D-Bus activates it from
	# org.kde.ActivityManager.service; start it directly so the run does
	# not depend on activation working.
	#
	# It is NOT in /usr/bin: Alpine installs it under /usr/lib/libexec,
	# so look where it actually is and say so when it is missing.
	KAMD=""
	for k in /usr/lib/libexec/kactivitymanagerd \
		 /usr/libexec/kactivitymanagerd \
		 /usr/lib/kactivitymanagerd \
		 /usr/bin/kactivitymanagerd; do
		[ -x "$k" ] && { KAMD="$k"; break; }
	done
	if [ -n "$KAMD" ]; then
		# offscreen, not wayland: it is a background service that owns a
		# D-Bus name and draws nothing. Started on the Wayland platform it
		# initialises wayland-egl like every other Qt client here, hits the
		# same Mesa fault, and dies -- taking the shell's start-up with it.
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

	# Qt Quick on the CPU.
	#
	# plasmashell draws its panel and desktop with QML, and Qt's Wayland
	# client picks the wayland-egl buffer integration by default. With no
	# GL driver here Mesa falls through to zink, Vulkan is absent, EGL
	# initialisation fails and the shell exits during start-up -- which is
	# exactly what it did. Qt ships a software scene graph for this case,
	# so ask for it rather than for an accelerated path that cannot exist.
	# The renderer selection in render-select.sh decides the same question
	# for the compositor; this is the client half of it.
	#
	# The buffer integration is named explicitly for the same reason. Qt
	# initialises wayland-egl before it knows the scene graph is software,
	# and that init faulted at a null pointer inside Mesa here -- a crash,
	# not a refusal. Asking for shm keeps Qt off that path entirely. The
	# crash itself is worth its own investigation and is recorded as such.
	WAYLAND_DISPLAY="${KWIN_SOCK:-wayland-2}" QT_QPA_PLATFORM=wayland \
	QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1 \
	QT_WAYLAND_CLIENT_BUFFER_INTEGRATION=shm \
		timeout 900 plasmashell --no-respawn \
			> /tmp/kde-plasmashell.log 2>&1 &
	PLASMAPID=$!
	# Plasma paints the panel well after its process exists, so wait for
	# the shell to say it is up rather than for a fixed number of seconds.
	i=0
	# Watch the shell, not the timeout that launched it.
	#
	# PLASMAPID is `timeout`'s pid, and timeout here exits on its own while
	# its child keeps running -- a kernel defect of ours, tracked separately.
	# Testing it reported the shell as dead at 32 s while the shell's own log
	# went on showing backingstore updates ten seconds later. Ask after the
	# process that matters instead.
	plasma_running() { prog_alive plasmashell $PLASMAPID; }
	# Wait for evidence that it PAINTED, not for the loop to run out.
	#
	# This has now been wrong twice in the same way, and each time the number
	# it produced was read as Plasma being slow. The first set of strings
	# ("Loading the desktop", "panel", ...) appears nowhere in this build; the
	# second set (backingstore, QQuickWindow) does not either, so the loop ran
	# its full ninety seconds while the desktop was on screen the whole time --
	# the photograph taken from the host proves it painted. Ninety seconds of a
	# three-minute start-up were this wait.
	#
	# Ask the compositor instead of the client. kwin logs the interfaces it
	# offers each client BY EXECUTABLE as that client binds them, so
	# `of "/bin/plasmashell"` in kwin's own log is kwin saying plasmashell is
	# connected and binding globals -- one process observing another, which is
	# what makes it worth more than plasmashell's own account of itself. The
	# cap is 45s because it now measures something that really happens.
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
	# A witness window, so a black picture can be read.
	#
	# If this appears and Plasma does not, kwin is presenting its nested
	# output correctly and the question is about the shell's surfaces. If
	# neither appears, the nested presentation is the problem. Without it
	# an empty frame says only "something is wrong somewhere".
	if [ -x /usr/bin/foot ]; then
		WAYLAND_DISPLAY="${KWIN_SOCK:-wayland-2}" foot > /tmp/kde-foot.log 2>&1 &
		# ... and one on the HOST compositor, which is the control.
		#
		# The picture is taken from sway. A terminal that is black inside
		# kwin AND black directly on sway means client content is not
		# reaching a compositor at all -- a shared-memory buffer that is
		# private to the writer, which this system has had before. Black
		# only inside kwin puts it on kwin's nested presentation. One extra
		# window separates those two answers in a single frame.
		[ -n "${HOST_SOCK:-}" ] && \
			WAYLAND_DISPLAY="$HOST_SOCK" foot > /tmp/kde-foot-host.log 2>&1 &
		sleep 8
	fi
	# Print what the shell said either way. Logging it only on death is
	# how a run that produced a black window told us nothing: the process
	# was alive, so the interesting output stayed inside the guest.
	echo "--- plasmashell log (last 20) ---"
	tail -20 /tmp/kde-plasmashell.log 2>/dev/null
	echo "--- kwin log (last 15) ---"
	tail -15 /tmp/kde-kwin.log 2>/dev/null
	echo "--- end logs ---"
	# Give the panel time to be composited into kwin's surface.
	sleep 10
else
	echo "KDE: no plasmashell in the image t=$(up)"
fi
}

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

	# What this build actually accepts, printed once: the nested backend is
	# selected by naming the host display, and there is no --wayland flag to go
	# with it. Guessing that cost a run.
	echo "KDE: kwin options: $(timeout 15 kwin_wayland --help 2>&1 | grep -aoE '^\s+--[a-z-]+' | tr -d ' ' | tr '\n' ' ')"
	# 90 s was the whole run when this started kwin and nothing else. With
	# plasmashell in the sequence the screenshot is taken past t=240, and a
	# compositor killed at t=115 leaves the host's background in the picture --
	# which is exactly the empty blue frame this produced, with plasmashell
	# still reported alive because IT had not died.
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

	# The desktop itself, not just the compositor.
	#
	# kwin_wayland is the window manager; what makes a screenshot look like
	# KDE -- panel, task manager, desktop wallpaper and its containment -- is
	# plasmashell, and it is an ordinary Wayland client of kwin. It needs a
	# SESSION bus of its own: Plasma's components find each other over it
	# (kactivitymanagerd, the shell's own service name), and with only a system
	# bus present plasmashell exits during start-up instead of drawing.
	start_plasma
	# The picture: sway composites kwin's surface, grim reads sway's output.
	if command -v grim >/dev/null 2>&1; then
		# 30 s was not enough with Plasma running: a software-composited
		# 1024x768 read back through wlr-screencopy on a loaded guest takes
		# longer than an idle one, and grim came back "Terminated" -- a
		# deadline, not a failure to capture.
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

# The DRM path needs a session; get one before looking at cards.
#
# This re-executes the script inside a PAM login and does not return. The
# second pass arrives with XDG_SESSION_ID set and falls straight through.
enter_session

if [ -n "${XDG_SESSION_ID:-}" ]; then
	echo "KDE: ok login-session id=$XDG_SESSION_ID t=$(up)"
	# What the compositor will be told about this session.
	#
	# A session is not enough on its own: logind hands out devices to a session
	# that is Active, on a Seat that owns them. A session with no seat and no
	# VT is created successfully and is still useless for this -- so print the
	# properties rather than treat "a session exists" as the answer.
	# Is logind still there at all?
	#
	# It created this session and then loginctl could not find the name on the
	# bus. Either it exited after answering -- nothing supervises it here --
	# or the name went away with the process that owned it. The two look the
	# same from loginctl and are different problems, so ask after the process
	# and the name separately.
	# Why it left, in its own words.
	#
	# It created this session and then its process was gone -- alive=0, not a
	# name lost by a running daemon. Whatever it printed on the way out is the
	# answer, and nothing was reading it.
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
		# Does logind consider THIS process part of the session?
		#
		# It refused kwin control of a session that is Active, on seat0 and
		# on VT 1 -- so what it doubts is the caller, not the session.
		# Membership is decided by the control group the session owns, and
		# logind put the session's processes into it by writing to
		# cgroup.procs. If a child does not inherit that group, everything
		# started inside the session is outside it as far as logind is
		# concerned.
		echo "KDE: my cgroup: $(cat /proc/self/cgroup 2>&1 | tr '\n' ' ' | cut -c1-90)"
		echo "KDE: session cgroup holds: $(cat /sys/fs/cgroup/c1/cgroup.procs 2>&1 | tr '\n' ' ' | cut -c1-90)"
		echo "KDE: my pid: $$"
		# Ask logind to hand over control, and print what it says.
		#
		# kwin reports only "Failed to take control ... maybe another
		# compositor is running", swallowing the reply that says why. The
		# session is Active on seat0 with VT 1, this process is in the
		# session's cgroup, and every VT ioctl logind uses is implemented --
		# so the refusal has a reason that only logind knows.
		# And where logind is stuck when it does not answer.
		#
		# The reply never comes -- not a refusal, a hang -- while simple calls
		# to the same daemon are answered, so it is stuck inside this one
		# method. The task dump says what it is waiting on, which is the
		# difference between a kernel primitive that never wakes and a D-Bus
		# call it made from inside its own handler.
		( sleep 12; echo "KDE: --- tasks while TakeControl hangs ---";
		  cat /proc/b1nix-tasks 2>/dev/null | grep -iE "elogind|dbus" | cut -c1-200 ) &
		# Can the bus say who is on the other end?
		#
		# logind answers TakeControl with InvalidArgs for every caller, and in
		# its code that method reads one boolean and then asks the bus for the
		# sender's credentials. The credentials come from the kernel, for the
		# local socket, so if they are missing or wrong the refusal is ours.
		echo "KDE: bus says logind is uid=$(dbus-send --system --print-reply \
			--dest=org.freedesktop.DBus /org/freedesktop/DBus \
			org.freedesktop.DBus.GetConnectionUnixUser \
			string:org.freedesktop.login1 2>&1 | tail -1 | tr -d ' \n')"
		echo "KDE: bus says logind is pid=$(dbus-send --system --print-reply \
			--dest=org.freedesktop.DBus /org/freedesktop/DBus \
			org.freedesktop.DBus.GetConnectionUnixProcessID \
			string:org.freedesktop.login1 2>&1 | tail -1 | tr -d ' \n')"
		# Another method on the same object, with the same argument type.
		#
		# TakeControl reads one boolean and then asks for credentials; the bus
		# supplies those correctly (uid=0 pid=185 above), so what is left is
		# reading the argument. SetIdleHint takes a boolean too. If it fails
		# the same way, nothing with a boolean argument is arriving intact; if
		# it works, the fault is in TakeControl alone.
		echo "KDE: SetIdleHint says: $(dbus-send --system --print-reply \
			--dest=org.freedesktop.login1 \
			/org/freedesktop/login1/session/c1 \
			org.freedesktop.login1.Session.SetIdleHint boolean:true 2>&1 \
			| tr '\n' ' ' | cut -c1-120)"
		# The lookup logind does for TakeDevice, by hand.
		#
		# It answers ENODEV, and the way it finds a device is by number:
		# /sys/dev/char/<major>:<minor>, then the name and the uevent inside.
		# If that entry is missing, or missing DEVNAME, the device does not
		# exist as far as it is concerned -- whatever /dev holds.
		echo "KDE: /sys/dev/char has: $(ls /sys/dev/char 2>&1 | tr '\n' ' ' | cut -c1-140)"
		for d in 226:0 226:1 226:128; do
			echo "KDE: char $d -> $(readlink -f /sys/dev/char/$d 2>&1 | cut -c1-70) uevent=[$(cat /sys/dev/char/$d/uevent 2>&1 | tr '\n' ' ' | cut -c1-80)]"
		done
		probe_logind_device "/org/freedesktop/login1/session/${XDG_SESSION_ID:-c1}"
		echo "KDE: logind log: $(tail -12 /tmp/kde-elogind.log 2>/dev/null | tr '\n' ' ' | cut -c1-320)"
		echo "KDE: TakeControl says: $(dbus-send --system --print-reply \
			--dest=org.freedesktop.login1 \
			/org/freedesktop/login1/session/c1 \
			org.freedesktop.login1.Session.TakeControl boolean:false 2>&1 \
			| tr '\n' ' ' | cut -c1-200)"
		break
	done
else
	echo "KDE: no login-session t=$(up) (kwin's DRM backend will not ask for a device)"
fi

# Only the cards the kernel actually publishes for discovery.
#
# Listing /dev/dri/card* offers every node, and one of them must not be chosen:
# /dev/dri/card0 is b1nix's own small DRM device, kept for the tests written
# against it, and the kernel deliberately leaves it out of /sys/class/drm so
# that discovery does not see two devices for one GPU. Handing it to kwin
# anyway is how it ended up asking logind for 226:0 -- a devnum with no sysfs
# entry, answered with ENODEV, which is the correct answer to the wrong
# question. Ask where the kernel publishes, the way any client's discovery
# does.
CARDS=""
for __c in /sys/class/drm/card[0-9]*; do
	__n=$(basename "$__c" 2>/dev/null)
	case "$__n" in *-*) continue ;; esac
	[ -e "/dev/dri/$__n" ] || continue
	# The test logind itself applies: it looks a device up by number, at
	# /sys/dev/char/<major>:<minor>. A card whose number has no entry there
	# does not exist as far as it is concerned, however plainly the node sits
	# in /dev -- and it answers ENODEV, which is the right answer to a
	# question that should not have been asked. Being listed under
	# /sys/class/drm is not enough: card0 is listed and has no such entry.
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
	echo "KDE: backend virtual t=$(up)"
fi

# The seat, and why it is not seat0.
#
# kwin's direct session — the one it uses when there is no logind — treats
# "seat0" as the seat that owns the machine's console, and then insists on a
# virtual terminal: it opens /dev/tty0, asks for its state and puts it in
# graphics mode. b1nix has no VTs, so that fails, no session object is created,
# and every device open is refused before it reaches the driver — which the
# compositor reports as "failed to open drm device", naming a file that opens
# perfectly well from a shell. Any other seat name skips the VT dance entirely,
# which is the same thing a container or an embedded session does.
export XDG_SEAT=seat1
echo "KDE: tty0 present: $([ -e /dev/tty0 ] && echo yes || echo no)"

export QT_PLUGIN_PATH=/usr/lib/qt6/plugins
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt6/plugins/platforms

# Does the Qt runtime work here at all?
#
# Bounded, because the first attempt at this hung: --version returned nothing
# and never came back, so the compositor was not failing to start — it could not
# get as far as printing its own name. Whatever blocks there blocks everything
# after it, and a hang with no output is the least informative failure there is.
if timeout 20 kwin_wayland --version > /tmp/kde-version.log 2>&1; then
	echo "KDE: kwin says: $(head -1 /tmp/kde-version.log 2>/dev/null)"
else
	echo "KDE: fail version-timeout (Qt cannot start) t=$(up)"
	echo "--- version attempt ---"
	head -20 /tmp/kde-version.log 2>/dev/null
fi



# On the DRM path, each candidate gets a turn: a card that cannot be opened for
# modesetting is not a failure of the compositor, and trying the next one is
# what any session manager would do.
if [ -n "${DRM_CANDIDATES:-}" ]; then
	start_session_bus
	for c in $DRM_CANDIDATES; do
		echo "KDE: trying $c t=$(up)"
		export KWIN_DRM_DEVICES=$c
		# A previous candidate may have created the socket before giving up,
		# and a leftover socket makes the next candidate look successful when
		# it never opened the card at all. Clear it, so the check below tests
		# this attempt rather than the last one's litter.
		rm -f /run/user/0/wayland-1
		# kwin reports "failed to open drm device" without saying what failed
		# in the open. Its own categories do say, so turn them on.
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
		# Two grepped lines are what this path used to report, and they were
		# the two least informative ones: they say the open failed without
		# saying what the open was. Print the whole thing.
		echo "--- kwin log (last attempt, in full) ---"
		cat /tmp/kde-kwin.log 2>/dev/null
		echo "--- end kwin log ---"
		echo "KDE: done t=$(up)"
		exit 0
	fi
	sleep 5
	prog_alive kwin_wayland $KWINPID && echo "KDE: ok alive t=$(up)" \
	                                 || echo "KDE: fail died t=$(up)"

	# The desktop itself. kwin is only the window manager; the panel, the task
	# manager and the desktop containment are plasmashell, an ordinary client
	# of the socket kwin just created.
	KWIN_SOCK=wayland-1
	wait_kwin_socket
	memsnap "kwin-up"
	memsnap_loop &
	__memloop=$!
	start_plasma
	memsnap "plasma-started"

	# The picture is taken from OUTSIDE, with the QEMU monitor's screendump on
	# the scanout kwin programmed. Nothing in the guest produces it, so nothing
	# in the guest can fake it -- and it sees exactly what a monitor would.
	# These two markers bracket the window in which the framebuffer is worth
	# capturing; the host watches the serial log for them.
	memsnap "scanout-ready"
	echo "KDE: SCANOUT-READY t=$(up)"
	sleep 90
	echo "KDE: SCANOUT-END t=$(up)"
	memsnap "scanout-end"
	kill $__memloop 2>/dev/null
	# All of it, minus the plugin loader's inventory. Every previous run of this
	# printed a grep, and every time the line that explained the refusal was one
	# the pattern did not match.
	# The DRM backend's own account, first and in full.
	#
	# The generic dump below is the head of the file, and the head of the file
	# is Qt's plugin inventory -- hundreds of lines of metadata that push every
	# line about outputs, connectors and page flips past the cut. Whether the
	# compositor programmed a scanout is the whole question here, so ask for
	# those lines by name before anything else.
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

echo "KDE: starting kwin_wayland t=$(up)"
if [ -n "$CLIENT" ]; then
	timeout 120 /usr/bin/kwin_wayland $BACKEND --socket wayland-1 --no-lockscreen \
		"$CLIENT" > /tmp/kde-kwin.log 2>&1 &
else
	/usr/bin/kwin_wayland $BACKEND --socket wayland-1 --no-lockscreen \
		> /tmp/kde-kwin.log 2>&1 &
fi
KWINPID=$!

# Give it time to bind its socket and paint a frame.
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

# What it drew. kwin does not speak wlr-screencopy, so grim cannot read it; its
# own interface is org.kde.KWin.ScreenShot2 over D-Bus, which needs a bus. On a
# DRM backend the picture is on the panel and the host can photograph it; on the
# virtual backend the frame is what the emulator's display shows, and the host
# takes it through the QEMU monitor. Either way the guest's job is to keep
# rendering long enough to be seen.
# The desktop on this backend too, not just the compositor: the same shell,
# started the same way, so a picture taken off the emulator's display shows a
# desktop rather than an empty compositor.
# kwin's own socket differs by backend: wayland-2 when it is nested inside
# sway, wayland-1 when it owns the display. The shell has to connect to KWIN,
# not to whatever compositor happens to be first.
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
