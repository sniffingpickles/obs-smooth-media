#!/usr/bin/env bash
#
# netsim.sh — bad-network test streamer for the Smooth Media Source plugin.
#
# Publishes a test pattern (on-screen timer + steady audio tone) as an SRT or
# RTMP listener with IRL-style impairments. Point the source at the printed URL
# and capture the OBS log.
#
#   ./netsim.sh <rtmp|srt> <clean|slow|veryslow|fast|burst|stall|latency>
#
# Override defaults via env, e.g.:  SIZE=1280x720 FPS=30 ./netsim.sh srt slow
#
set -u

PROTO="${1:-}"
SCENARIO="${2:-clean}"

BIND_IP="${BIND_IP:-0.0.0.0}"
RTMP_PORT="${RTMP_PORT:-1936}"     # 1936 so we don't fight a real nginx on 1935
RTMP_APP="${RTMP_APP:-live}"
RTMP_KEY="${RTMP_KEY:-test}"
SRT_PORT="${SRT_PORT:-9000}"

SIZE="${SIZE:-1920x1080}"
FPS="${FPS:-60}"
VB="${VB:-4000k}"
AB="${AB:-160k}"
TONE_HZ="${TONE_HZ:-440}"
STALL_ON="${STALL_ON:-25}"         # seconds streaming before each simulated drop
STALL_OFF="${STALL_OFF:-6}"        # seconds of outage between drops
SRT_LATENCY="${SRT_LATENCY:-120}"  # ms; the 'latency' scenario overrides this

if ! command -v ffmpeg >/dev/null 2>&1; then
	echo "error: ffmpeg not found in PATH. Install it first." >&2
	exit 1
fi

if [ "$PROTO" != "rtmp" ] && [ "$PROTO" != "srt" ]; then
	echo "usage: $0 <rtmp|srt> <clean|slow|veryslow|fast|burst|stall|latency>" >&2
	exit 1
fi

# ── Map scenario -> input pacing ────────────────────────────────────────────
RATE_ARGS="-re"        # default: real time
DURATION_ARGS=""       # used by 'stall' to end each run
case "$SCENARIO" in
	clean)    RATE_ARGS="-re" ;;
	slow)     RATE_ARGS="-readrate 0.97" ;;
	veryslow) RATE_ARGS="-readrate 0.93" ;;
	fast)     RATE_ARGS="-readrate 1.03" ;;
	burst)    RATE_ARGS="-re -readrate_initial_burst 4" ;;
	stall)    RATE_ARGS="-re"; DURATION_ARGS="-t $STALL_ON" ;;
	latency)  RATE_ARGS="-re"; SRT_LATENCY=2000 ;;
	*) echo "unknown scenario: $SCENARIO" >&2; exit 1 ;;
esac

# ── Build the output target + the URL to point the source at ────────────────
if [ "$PROTO" = "rtmp" ]; then
	OUT=(-f flv -listen 1 "rtmp://${BIND_IP}:${RTMP_PORT}/${RTMP_APP}/${RTMP_KEY}")
	PLAY_URL="rtmp://<host>:${RTMP_PORT}/${RTMP_APP}/${RTMP_KEY}"
	[ "$SCENARIO" = "latency" ] && echo "note: 'latency' only affects SRT; running RTMP clean-paced."
else
	OUT=(-f mpegts "srt://${BIND_IP}:${SRT_PORT}?mode=listener&latency=${SRT_LATENCY}")
	PLAY_URL="srt://<host>:${SRT_PORT}"
fi

run_once() {
	# testsrc2 has a built-in timer/counter; sine is a steady tone.
	# shellcheck disable=SC2086
	ffmpeg -hide_banner -loglevel warning \
		$RATE_ARGS -f lavfi -i "testsrc2=size=${SIZE}:rate=${FPS}" \
		$RATE_ARGS -f lavfi -i "sine=frequency=${TONE_HZ}:sample_rate=44100" \
		-c:v libx264 -preset veryfast -tune zerolatency -profile:v high \
		-b:v "$VB" -maxrate "$VB" -bufsize "$VB" -g $((FPS * 2)) -pix_fmt yuv420p \
		-c:a aac -b:a "$AB" -ar 44100 -ac 2 \
		$DURATION_ARGS "${OUT[@]}"
}

echo "============================================================"
echo " netsim: ${PROTO} / ${SCENARIO}"
echo " video : ${SIZE}@${FPS}  ${VB}    audio: ${TONE_HZ}Hz tone ${AB}"
echo " point the Smooth Media Source at:"
echo "     ${PLAY_URL}"
echo "     (host = 127.0.0.1 if same box, else this machine's IP)"
[ "$PROTO" = "srt" ] && echo " SRT receive latency: ${SRT_LATENCY} ms"
echo " turn ON 'Verbose Debug Logging' in the source's Advanced settings."
echo " Ctrl-C to stop."
echo "============================================================"

if [ "$SCENARIO" = "stall" ]; then
	echo "stall mode: ~${STALL_ON}s up, ~${STALL_OFF}s down, repeating."
	trap 'echo; echo "stopped."; exit 0' INT
	while true; do
		run_once
		echo ">>> simulated outage for ${STALL_OFF}s <<<"
		sleep "$STALL_OFF"
	done
else
	run_once
fi
