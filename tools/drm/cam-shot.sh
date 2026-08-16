#!/bin/sh
# Photograph the physical monitor with a USB webcam.
#
# The only proof that a modeset put a picture on a panel is a picture of the
# panel: the guest's own screenshot says what it painted, not what the display
# engine sent down the cable. A run under passthrough drives the monitor the
# host is no longer driving, so nothing on this desktop can show it.
#
# Usage:
#   sh tools/drm/cam-shot.sh                  # one frame, now
#   sh tools/drm/cam-shot.sh --for 180        # a frame every 10s for 180s
#   sh tools/drm/cam-shot.sh --for 180 --every 5 --tag chromium
#
# Environment:
#   CAM      video device (default: /dev/video0)
#   CAM_RES  capture resolution (default: 1280x720)
#
# Frames land in smoke_run/cam/<tag>-<seq>.png, and the script prints the mean
# brightness of each one: a dark panel and a lit one are not distinguishable
# from the filename, and a run that produced 30 identical black frames should
# say so without anyone opening them.
set -eu

SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
ROOT_DIR="$(cd "$(dirname "$SELF")/../.." && pwd)"
OUT_DIR="$ROOT_DIR/smoke_run/cam"

CAM="${CAM:-/dev/video0}"
CAM_RES="${CAM_RES:-1280x720}"
DURATION=0
EVERY=10
TAG="shot"

while [ $# -gt 0 ]; do
	case "$1" in
	--for) DURATION="$2"; shift 2 ;;
	--every) EVERY="$2"; shift 2 ;;
	--tag) TAG="$2"; shift 2 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

[ -e "$CAM" ] || { echo "no camera at $CAM" >&2; exit 1; }
command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg not installed" >&2; exit 1; }
mkdir -p "$OUT_DIR"

# -ss 1 discards the first second: this camera's auto-exposure starts wherever
# it left off, and its first frames are routinely black on a lit panel — which
# reads exactly like the failure being looked for.
shoot() {
	out="$1"
	ffmpeg -hide_banner -loglevel error -y \
		-f v4l2 -input_format mjpeg -video_size "$CAM_RES" -i "$CAM" \
		-ss 1 -frames:v 1 "$out" </dev/null >/dev/null 2>&1 || return 1
	mean=$(ffprobe -v quiet -f lavfi -i "movie=$out,signalstats" \
		-show_entries frame_tags=lavfi.signalstats.YAVG \
		-of default=nw=1:nk=1 2>/dev/null | tail -1)
	echo "$out  brightness=${mean:-?}"
}

if [ "$DURATION" -eq 0 ]; then
	shoot "$OUT_DIR/$TAG.png"
	exit 0
fi

seq=0
end=$(( $(date +%s) + DURATION ))
while [ "$(date +%s)" -lt "$end" ]; do
	seq=$((seq + 1))
	shoot "$(printf '%s/%s-%03d.png' "$OUT_DIR" "$TAG" "$seq")" || echo "capture failed"
	sleep "$EVERY"
done
