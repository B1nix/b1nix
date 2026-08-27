#!/bin/sh
# render-select.sh — which renderer does the compositor use on this machine?
#
# Composition on b1nix has been software since it started working: pixman, in
# the CPU, into a dumb buffer. That path is not a stopgap and does not go away
# — it is the only one that works on a machine whose GPU we do not drive, and
# it is what proves the display pipeline in isolation when acceleration breaks.
#
# The accelerated path sits beside it: a real GL/GLES driver in the guest on
# top of the imported DRM core, which wlroots reaches through EGL and gbm on a
# render node. It is chosen at run time when it is actually there and actually
# works, and when it is not, the compositor starts on pixman rather than
# failing to start. "Falls back" has to mean the compositor still comes up.
#
# The decision is made by asking, never by assuming:
#
#   1. an explicit request wins           (b1nix.renderer=, B1NIX_RENDERER)
#   2. a render node has to exist         (/dev/dri/renderD*)
#   3. a Mesa DRI driver has to be there  (*_dri.so in the loader's path)
#   4. that driver has to render          (/bin/gl_probe: EGL context, a
#                                          compiled shader, a drawn triangle
#                                          and the pixels read back)
#
# Only a run that clears all four gets WLR_RENDERER=gles2. Steps 3 and 4 are
# the ones that matter: libEGL.so is in every image, so "EGL exists" says
# nothing, and a driver that loads but cannot submit a batch reports every
# initialisation as a success.
#
# Sourced by the compositor launchers; also runnable on its own to print the
# decision. After render_select() the caller's environment carries
# WLR_RENDERER (and WLR_RENDER_DRM_DEVICE on the accelerated path), and
# /run/render-selection records what was decided and why.

RENDER_STATE_FILE=/run/render-selection

# What was asked for: auto (default), pixman, or gles2. b1nix.no-accel is the
# short way to say pixman, and it is what the fallback test uses to force the
# accelerated path off without removing the driver from the image.
render_requested() {
	if [ -n "$B1NIX_RENDERER" ]; then
		echo "$B1NIX_RENDERER"
		return
	fi
	for tok in $(cat /proc/cmdline 2>/dev/null); do
		case "$tok" in
		b1nix.no-accel) echo pixman; return ;;
		b1nix.renderer=*) echo "${tok#b1nix.renderer=}"; return ;;
		esac
	done
	echo auto
}

# Acceleration turned off without touching the image. A machine that has the
# driver still has to come up on pixman when this is set, which is the whole
# claim the fallback makes.
render_accel_forced_off() {
	[ "$B1NIX_ACCEL_OFF" = "1" ] && return 0
	for tok in $(cat /proc/cmdline 2>/dev/null); do
		[ "$tok" = "b1nix.accel-off" ] && return 0
	done
	return 1
}

# The first render node that exists and can be opened. Opening it matters:
# a node whose permissions or driver refuse the open is not a usable device,
# and finding that out here rather than inside EGL keeps the reason readable.
render_find_node() {
	for n in $B1NIX_RENDER_NODE /dev/dri/renderD128 /dev/dri/renderD129 \
	         /dev/dri/renderD130; do
		[ -c "$n" ] || continue
		if dd if="$n" bs=1 count=0 >/dev/null 2>&1; then
			echo "$n"
			return 0
		fi
	done
	return 1
}

# Where Mesa's DRI drivers live in this image. Alpine's mesa-dri-gallium
# installs into /usr/lib/xorg/modules/dri; a plain Mesa build uses /usr/lib/dri.
# An empty directory is not a driver, so the search is for the .so itself.
render_find_driver_dir() {
	for d in $LIBGL_DRIVERS_PATH /usr/lib/xorg/modules/dri /usr/lib/dri \
	         /usr/local/lib/dri; do
		[ -d "$d" ] || continue
		for so in "$d"/*_dri.so; do
			if [ -e "$so" ]; then
				echo "$d"
				return 0
			fi
		done
	done
	return 1
}

# Run gl_probe with a bound: a driver that wedges must not take the boot with
# it, and a wedged probe is a failed probe as far as the choice goes.
render_run_gl_probe() {
	_node=$1
	_log=${2:-/tmp/render-gl-probe.log}
	if command -v timeout >/dev/null 2>&1; then
		timeout 60 /bin/gl_probe "$_node" > "$_log" 2>&1
	else
		/bin/gl_probe "$_node" > "$_log" 2>&1
	fi
}

# The decision. Sets RENDER_MODE (accelerated|software), RENDER_REASON,
# RENDER_DEVICE and RENDER_GL_RENDERER, exports what wlroots reads, and prints
# one line that says all of it.
render_select() {
	RENDER_MODE=software
	RENDER_REASON=""
	RENDER_DEVICE=""
	RENDER_GL_RENDERER=""

	_want=$(render_requested)
	case "$_want" in
	pixman|software)
		RENDER_REASON=requested-software
		;;
	*)
		if render_accel_forced_off; then
			# The same branch an image with no driver takes, reached on
			# purpose: this is how the fallback is tested on a machine
			# where acceleration would otherwise work.
			RENDER_REASON=forced-off
		elif ! RENDER_DEVICE=$(render_find_node); then
			RENDER_REASON=no-render-node
		elif ! _dridir=$(render_find_driver_dir); then
			RENDER_REASON=no-dri-driver
		else
			# Name the search path for the loader. Mesa's default is a
			# build-time path that need not be this image's.
			export LIBGL_DRIVERS_PATH="$_dridir"
			[ -n "$B1NIX_MESA_DRIVER" ] && \
				export MESA_LOADER_DRIVER_OVERRIDE="$B1NIX_MESA_DRIVER"
			if [ ! -x /bin/gl_probe ]; then
				RENDER_REASON=no-gl-probe
			elif render_run_gl_probe "$RENDER_DEVICE"; then
				RENDER_MODE=accelerated
				RENDER_REASON=gl-probe-passed
				RENDER_GL_RENDERER=$(sed -n 's/^GL-PROBE: renderer //p' \
					/tmp/render-gl-probe.log 2>/dev/null | head -1)
			else
				RENDER_REASON=gl-probe-failed
				RENDER_GL_RENDERER=$(sed -n 's/^GL-PROBE: renderer //p' \
					/tmp/render-gl-probe.log 2>/dev/null | head -1)
				# Why it failed, on the record. "acceleration is unavailable"
				# with no reason behind it is the state this whole file exists
				# to avoid: the driver, the node and the compositor all get
				# blamed for it in turn, and the probe already knows.
				sed -n 's/^/RENDER-SELECT: probe: /p' \
					/tmp/render-gl-probe.log 2>/dev/null | tail -12
			fi
		fi
		;;
	esac

	if [ "$RENDER_MODE" = accelerated ]; then
		export WLR_RENDERER=gles2
		export WLR_RENDER_DRM_DEVICE="$RENDER_DEVICE"
	else
		export WLR_RENDERER=pixman
		unset WLR_RENDER_DRM_DEVICE
		# Nothing below pixman reads these, and leaving a driver override
		# behind would follow the process into an unrelated GL client.
		unset MESA_LOADER_DRIVER_OVERRIDE
	fi

	mkdir -p "$(dirname $RENDER_STATE_FILE)" 2>/dev/null
	{
		echo "renderer=$WLR_RENDERER"
		echo "mode=$RENDER_MODE"
		echo "reason=$RENDER_REASON"
		echo "device=$RENDER_DEVICE"
		echo "requested=$_want"
		echo "gl_renderer=$RENDER_GL_RENDERER"
	} > "$RENDER_STATE_FILE" 2>/dev/null

	echo "RENDER-SELECT: renderer=$WLR_RENDERER mode=$RENDER_MODE" \
	     "reason=$RENDER_REASON device=${RENDER_DEVICE:-none}" \
	     "requested=$_want gl=${RENDER_GL_RENDERER:-none}"
}

# Run directly (rather than sourced) to print the decision and nothing else.
case "$0" in
*render-select.sh)
	render_select
	;;
esac
