#!/bin/sh
# Hunt the intermittent display: run the passthrough over and over, watch the
# monitor through a webcam, and keep only the evidence from the runs that lit it.
#
# The picture has been seen once, by eye, and never in eight recorded runs — so
# the question is not "does it work" but "how often, and does anything change
# the odds". Runs alternate between two images: one built normally and one with
# every delay in the shim multiplied (b1nix.slow-phy). If a step in the PHY
# enable sequence is losing a race, waiting longer should change the hit rate,
# and that is what this measures.
#
# Disk is the constraint over a night of runs, so a recording is kept only when
# it caught something: the analysis reduces each run to one number, and the
# video is deleted unless that number stands out from the run's own baseline.
#
# Usage: sh tools/hunt-display.sh [runs]        (default 40)
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT_DIR/smoke_run/hunt"
RUNS="${1:-40}"
CAM="${CAM:-/dev/video0}"
# A lit screen is far brighter than the wall behind it. Rather than a fixed
# threshold, which depends on the room's light, a run counts as a hit when its
# brightest frame stands this far above its own median.
MARGIN="${MARGIN:-25}"

mkdir -p "$OUT"
SUMMARY="$OUT/summary.txt"
: > "$SUMMARY"

hits=0
for i in $(seq 1 "$RUNS"); do
	if [ $((i % 2)) -eq 1 ]; then
		iso=/tmp/iso-normal.iso; variant=normal
	else
		iso=/tmp/iso-slow.iso;   variant=slow-phy
	fi

	vid="$OUT/run-$i.mkv"
	log="$OUT/run-$i.log"
	lum="$OUT/run-$i.luma"

	ISO="$iso" CAPTURE_GRACE=5 TIMEOUT=200 MEM_MB=2048 \
		sh "$ROOT_DIR/tools/run-i915-passthrough.sh" >/dev/null 2>&1 &
	runner=$!

	# The camera's own exposure control has to be off, or it defeats the
	# measurement: pointed at a dark room it winds the gain up until the wall
	# is bright, which reads exactly like a screen that came on. Fixed exposure
	# makes frames comparable to each other and to other runs.
	v4l2-ctl -d "$CAM" -c auto_exposure=1 -c exposure_time_absolute=300 \
		-c white_balance_automatic=0 >/dev/null 2>&1 || true

	# Recording starts after the guest has had time to reach the modeset and
	# stops before the runner does, so the camera never outlives the picture.
	sleep 8
	timeout 90 ffmpeg -loglevel error -f v4l2 -input_format yuyv422 \
		-video_size 640x480 -framerate 4 -i "$CAM" \
		-c:v mpeg4 -q:v 8 -y "$vid" 2>/dev/null || true
	wait "$runner" 2>/dev/null || true

	cp -f "$ROOT_DIR/smoke_run/i915-passthrough.log" "$log" 2>/dev/null || true

	ffmpeg -loglevel error -i "$vid" \
		-vf "signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=$lum" \
		-f null - 2>/dev/null || true

	stats="$(awk -F= '/YAVG/ { v[n++] = $2; if ($2 > max) max = $2 }
		END {
			if (n == 0) { print "0 0 0"; exit }
			asort(v)
			printf "%.1f %.1f %d\n", max, v[int(n/2)], n
		}' "$lum" 2>/dev/null || echo "0 0 0")"
	# asort is a gawk extension; fall back to sort when it is not available.
	case "$stats" in
	"0 0 0")
		max="$(awk -F= '/YAVG/{if($2>x)x=$2}END{printf "%.1f", x+0}' "$lum")"
		med="$(awk -F= '/YAVG/{print $2}' "$lum" | sort -n |
		       awk '{a[NR]=$1} END{printf "%.1f", a[int(NR/2)]+0}')"
		cnt="$(grep -c YAVG "$lum" 2>/dev/null || echo 0)"
		;;
	*)
		max="${stats%% *}"; rest="${stats#* }"
		med="${rest%% *}"; cnt="${rest##* }"
		;;
	esac

	# A real picture lasts: the frame hold is tens of seconds, so a genuine
	# lighting shows up in many consecutive frames. A single bright frame is the
	# camera or a passing headlight, and is not worth a night of disk.
	runlen="$(awk -F= -v d="$med" -v k="$MARGIN" '
		/YAVG/ { if ($2 - d > k) { r++; if (r > best) best = r } else r = 0 }
		END { print best + 0 }' "$lum")"

	verdict=dark
	if [ "$cnt" -gt 0 ] && [ "$runlen" -ge 8 ] && \
	   awk -v m="$max" -v d="$med" -v k="$MARGIN" 'BEGIN{exit !(m - d > k)}'; then
		verdict=LIT
		hits=$((hits + 1))
		# Keep the brightest frame beside the video, so the morning starts with
		# a picture rather than a number.
		frame="$(awk -F= '/YAVG/{n++; if($2>x){x=$2; f=n}} END{print f-1}' "$lum")"
		ffmpeg -loglevel error -i "$vid" -vf "select=eq(n\,$frame)" \
			-vsync 0 -frames:v 1 -y "$OUT/hit-$i.png" 2>/dev/null || true
	else
		rm -f "$vid"
	fi
	rm -f "$lum"

	printf '%s run %-3s %-9s max %-7s median %-7s bright-frames %-4s %s\n' \
		"$(date +%H:%M:%S)" "$i" "$variant" "$max" "$med" "${runlen:-0}" \
		"$verdict" >> "$SUMMARY"

	# Logs are small; keep the ones that matter and drop the rest so a night of
	# runs does not fill the disk.
	[ "$verdict" = LIT ] || rm -f "$log"
done

printf 'done: %d hits in %d runs\n' "$hits" "$RUNS" >> "$SUMMARY"
