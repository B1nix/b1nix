#!/bin/sh
# render-smoke.sh — both composition paths, each proved on its own.
#
# Composition has two paths on b1nix and both are supported: pixman in the CPU,
# and GLES through EGL and gbm on a DRM render node. A suite that only ever
# exercised whichever one the machine happened to pick would let a regression in
# the other sit undiscovered for months, so each is run deliberately and each
# gets its own marker:
#
#   RENDER-SMOKE: ok software-frame    pixman composited a frame
#   RENDER-SMOKE: ok accel-frame       GLES on a render node composited a frame
#   RENDER-SMOKE: ok fallback-engaged  acceleration forced off, the compositor
#                                      still came up and still composited
#
# A frame here is not "sway started" and not "grim exited 0". sway is told what
# colour to paint, the frame is pulled back out through wlr-screencopy, and
# /bin/framecheck reads the pixels and compares them with the colour asked for.
# Two colours per run, because one uniform image proves only that a buffer was
# filled; two that follow the request prove the compositor rendered what it was
# told. A marker is printed only after that check passes.
#
# Driven from /etc/local.d/00-smoke.start on the gfx instance, and reachable on
# its own with b1nix.render-smoke for a boot on an image that carries a real
# driver (make B1NIX_GPU_DRV=1 …).

[ -f /run/render-smoke.ran ] && exit 0
: > /run/render-smoke.ran 2>/dev/null

. /etc/render-select.sh

export HOME=/root
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p /run/user/0 /etc/sway /tmp
chmod 700 /run/user/0

# Everything below needs all four. Say which one is missing rather than
# printing a skip that reads like a pass.
for tool in sway swaymsg grim; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "RENDER-SMOKE: fail missing-$tool"
		echo "RENDER-SMOKE: done"
		exit 0
	fi
done
if [ ! -x /bin/framecheck ]; then
	echo "RENDER-SMOKE: fail missing-framecheck"
	echo "RENDER-SMOKE: done"
	exit 0
fi

cat > /etc/sway/render-smoke.conf <<'EOF'
# No bar, no keybindings, no clients of our own: the subject is the
# compositor's own rendering. swaybg, which sway starts itself, is the client
# that paints the background it is told to paint.
output HEADLESS-1 mode 800x600
EOF

if command -v timeout >/dev/null 2>&1; then
	TMO="timeout"
else
	TMO=""
fi

# One compositor run: start sway with the renderer already chosen in the
# environment, paint two colours, capture and check each. Echoes nothing that
# could be mistaken for a marker — the caller decides what the result means.
# $1 label, $2..$3 the two colours as rrggbb.
render_run_case() {
	_label=$1
	_c1=$2
	_c2=$3
	_ok=1

	rm -f "$XDG_RUNTIME_DIR"/wayland-* "$XDG_RUNTIME_DIR"/sway-ipc.*.sock \
		2>/dev/null
	unset WAYLAND_DISPLAY SWAYSOCK
	export WLR_BACKENDS=headless
	export WLR_HEADLESS_OUTPUTS=1
	export WLR_LIBINPUT_NO_DEVICES=1
	# libseat's builtin backend is not compiled into Alpine's build and there
	# is no seatd here; a headless compositor needs neither.
	export LIBSEAT_BACKEND=noop

	echo "RENDER-SMOKE: $_label starting sway renderer=$WLR_RENDERER" \
	     "device=${WLR_RENDER_DRM_DEVICE:-none}"
	# -d on the accelerated case only: that is the path still being brought
	# up, and "the colour never arrived" says nothing about which half — the
	# compositor's render or the client's buffer — did not happen. The other
	# two cases stay quiet so their logs remain readable.
	_swayargs=""
	[ "$_label" = accel ] && _swayargs="-d"
	sway $_swayargs -c /etc/sway/render-smoke.conf \
		> "/tmp/render-$_label-sway.log" 2>&1 &
	_swaypid=$!

	_i=0
	_sock=""
	while [ $_i -lt 40 ]; do
		_sock=$(ls -1 "$XDG_RUNTIME_DIR" 2>/dev/null |
			grep '^wayland-[0-9]*$' | head -1)
		[ -n "$_sock" ] && break
		kill -0 $_swaypid 2>/dev/null || break
		_i=$((_i + 1))
		sleep 1
	done
	if [ -z "$_sock" ]; then
		echo "RENDER-SMOKE: $_label no-socket after ${_i}s"
		tail -20 "/tmp/render-$_label-sway.log"
		kill $_swaypid 2>/dev/null
		wait $_swaypid 2>/dev/null
		return 1
	fi
	export WAYLAND_DISPLAY="$_sock"
	SWAYSOCK=$(ls -1 "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
	export SWAYSOCK
	# The clipboard check needs a live compositor and therefore runs after the
	# socket is ready, not before render-smoke starts sway.
	if [ "$_label" = "software" ] && [ -x /bin/m51_clipboard_smoke ]; then
		/bin/m51_clipboard_smoke
	fi

	for _c in "$_c1" "$_c2"; do
		# The compositor repaints on its own clock, so the frame carrying
		# this colour arrives some time after swaymsg returns. Waiting a
		# fixed number of seconds guesses at that delay and gets it wrong
		# under load -- one capture came back with the grey sway paints
		# before the first repaint. Capture until the colour is there, or
		# until the deadline says it never will be; the last attempt is
		# the one that reports, so a real failure still prints the pixels.
		_try=0
		_shot=0
		while [ $_try -lt 10 ]; do
			_try=$((_try + 1))
			[ $_try -eq 10 ] && _last=1 || _last=0
			# Ask for the colour on every attempt, and re-resolve the
			# IPC socket first. The command used to be sent once,
			# before the loop: sway answers Wayland clients as soon as
			# its display socket exists but creates its IPC socket
			# separately, so the first swaymsg can be delivered to
			# nothing and the request is simply lost -- the capture
			# then reports the grey of an unpainted output for as long
			# as anyone is willing to wait, which is what it did.
			SWAYSOCK=$(ls -1 "$XDG_RUNTIME_DIR"/sway-ipc.*.sock \
				2>/dev/null | head -1)
			export SWAYSOCK
			${TMO:+$TMO 20} swaymsg output '*' background "#$_c" \
				solid_color >> "/tmp/render-$_label-sway.log" 2>&1
			sleep 2
			if ! ${TMO:+$TMO 30} grim -t ppm \
				"/tmp/render-$_label-$_c.ppm" \
				2>>"/tmp/render-$_label-sway.log"; then
				[ $_last -eq 1 ] && \
					echo "RENDER-SMOKE: $_label grim-failed colour=$_c"
				continue
			fi
			if [ $_last -eq 1 ]; then
				/bin/framecheck "/tmp/render-$_label-$_c.ppm" "$_c" 24 \
					&& _shot=1
			elif /bin/framecheck "/tmp/render-$_label-$_c.ppm" "$_c" 24 \
				> /dev/null 2>&1; then
				/bin/framecheck "/tmp/render-$_label-$_c.ppm" "$_c" 24
				_shot=1
			fi
			rm -f "/tmp/render-$_label-$_c.ppm"
			[ $_shot -eq 1 ] && break
		done
		if [ $_shot -ne 1 ]; then
			_ok=0
			echo "RENDER-SMOKE: $_label colour=$_c never-arrived; sway log:"
			grep -iE 'error|fail|renderer|texture|shm|dmabuf|swaybg' \
				"/tmp/render-$_label-sway.log" | tail -30
			tail -10 "/tmp/render-$_label-sway.log"
		fi
	done

	kill $_swaypid 2>/dev/null
	_w=0
	while kill -0 $_swaypid 2>/dev/null && [ $_w -lt 10 ]; do
		sleep 1
		_w=$((_w + 1))
	done
	kill -9 $_swaypid 2>/dev/null
	wait $_swaypid 2>/dev/null
	[ "$_ok" = "1" ]
}

echo "RENDER-SMOKE: start"

# ── 1. What would this machine choose on its own? ──
# Run first, so the rest of the run knows whether the accelerated path is a
# real option here, and so the reason is on the record when it is not.
unset B1NIX_RENDERER B1NIX_ACCEL_OFF
render_select
ACCEL_MODE=$RENDER_MODE
ACCEL_REASON=$RENDER_REASON
ACCEL_DEVICE=$RENDER_DEVICE
if [ "$ACCEL_MODE" = accelerated ]; then
	echo "RENDER-SMOKE: accel-status available device=$ACCEL_DEVICE" \
	     "gl=${RENDER_GL_RENDERER:-unknown}"
else
	echo "RENDER-SMOKE: accel-status unavailable reason=$ACCEL_REASON"
fi
if [ -r "$RENDER_STATE_FILE" ]; then
	echo "RENDER-SMOKE: ok selection $(tr '\n' ' ' < $RENDER_STATE_FILE)"
else
	echo "RENDER-SMOKE: fail selection no-state-file"
fi

# ── 2. The software path, asked for by name ──
# Not "whatever was left over": pixman is a supported configuration and is
# tested as one, on every machine, whether or not acceleration works here.
unset B1NIX_ACCEL_OFF
B1NIX_RENDERER=pixman
render_select
unset B1NIX_RENDERER
if [ "$WLR_RENDERER" = pixman ] && render_run_case software ff0000 00ff00; then
	echo "RENDER-SMOKE: ok software-frame"
else
	echo "RENDER-SMOKE: fail software-frame"
fi

# ── 3. The fallback, with acceleration forced off ──
# The selection is left on auto — this is the automatic path deciding, not a
# request for pixman — and acceleration is taken away underneath it. The claim
# is not merely that pixman gets chosen but that the compositor still comes up
# and still paints, which is what "falls back rather than fails" means.
unset B1NIX_RENDERER
B1NIX_ACCEL_OFF=1
render_select
unset B1NIX_ACCEL_OFF
if [ "$WLR_RENDERER" = pixman ] && [ "$RENDER_REASON" = forced-off ] &&
   render_run_case fallback 0000ff ffff00; then
	echo "RENDER-SMOKE: ok fallback-engaged"
else
	echo "RENDER-SMOKE: fail fallback-engaged renderer=$WLR_RENDERER" \
	     "reason=$RENDER_REASON"
fi

# ── 4. The accelerated path ──
# Only where a driver answered the probe. Nothing is printed as a pass when it
# did not: the status line above already said why, and the host script turns
# that into a skip rather than a silence.
unset B1NIX_RENDERER B1NIX_ACCEL_OFF
if [ "$ACCEL_MODE" = accelerated ]; then
	render_select
	if [ "$WLR_RENDERER" = gles2 ] && render_run_case accel 00ffff ff00ff; then
		echo "RENDER-SMOKE: ok accel-frame"
	else
		echo "RENDER-SMOKE: fail accel-frame renderer=$WLR_RENDERER"
	fi
else
	echo "RENDER-SMOKE: skip accel-frame reason=$ACCEL_REASON"
fi

echo "RENDER-SMOKE: done"
exit 0
