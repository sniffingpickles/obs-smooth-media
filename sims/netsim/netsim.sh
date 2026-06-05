#!/usr/bin/env bash
#
# netsim.sh — bad-network test streamer for the Smooth Media Source plugin.
#
# Publishes a test pattern (on-screen timer + steady audio tone) with IRL-style
# impairments, either:
#   - PUSH mode: send it to a remote ingest server (rtmp/srt/rist), then point
#     the source at that server's playback URL; or
#   - LISTEN mode: run a local listener and point the source straight at it.
#
#   ./netsim.sh <scenario> <destination>
#
#     scenario   : clean | slow | veryslow | fast | burst | stall | latency
#     destination: rtmp | srt | rist            -> local listener (self-contained)
#                  <full push URL>              -> push to a remote server
#
# Examples:
#   ./netsim.sh slow  rtmp://172.238.7.67:1935/publish/test
#   ./netsim.sh burst "srt://1.2.3.4:8282?streamid=publish/live/test&mode=caller"
#   ./netsim.sh stall rist://1.2.3.4:9001
#   ./netsim.sh slow  srt                      # local listener
#
# Override defaults via env, e.g.:  SIZE=1280x720 FPS=30 ./netsim.sh slow rtmp
#
set -u

SCENARIO="${1:-}"
DEST="${2:-}"

BIND_IP="${BIND_IP:-0.0.0.0}"
RTMP_PORT="${RTMP_PORT:-1936}"     # local-listener port (avoid a real nginx on 1935)
RTMP_APP="${RTMP_APP:-live}"
RTMP_KEY="${RTMP_KEY:-test}"
SRT_PORT="${SRT_PORT:-9000}"
RIST_PORT="${RIST_PORT:-9001}"

SIZE="${SIZE:-1920x1080}"
FPS="${FPS:-60}"
VB="${VB:-4000k}"
AB="${AB:-160k}"
TONE_HZ="${TONE_HZ:-440}"
STALL_ON="${STALL_ON:-25}"
STALL_OFF="${STALL_OFF:-6}"
SRT_LATENCY="${SRT_LATENCY:-120}"  # ms; the 'latency' scenario overrides this

if ! command -v ffmpeg >/dev/null 2>&1; then
	echo "error: ffmpeg not found in PATH. Install it first." >&2
	exit 1
fi
if [ -z "$SCENARIO" ] || [ -z "$DEST" ]; then
	echo "usage: $0 <clean|slow|veryslow|fast|burst|stall|latency> <rtmp|srt|rist | full-push-url>" >&2
	exit 1
fi

# ── PUSH (remote URL) vs LISTEN (bare protocol) ─────────────────────────────
MODE="listen"
PROTO=""
PUSH_URL=""
case "$DEST" in
	*://*) MODE="push"; PUSH_URL="$DEST"; PROTO="${DEST%%://*}" ;;
	rtmp|srt|rist) MODE="listen"; PROTO="$DEST" ;;
	*) echo "destination must be rtmp|srt|rist or a full rtmp://|srt://|rist:// URL" >&2; exit 1 ;;
esac
if [ "$PROTO" != "rtmp" ] && [ "$PROTO" != "srt" ] && [ "$PROTO" != "rist" ]; then
	echo "unsupported protocol: $PROTO" >&2; exit 1
fi

# ── Scenario -> input pacing ────────────────────────────────────────────────
RATE_ARGS="-re"
DURATION_ARGS=""
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

# ── Output target + the URL to point the source at ──────────────────────────
MUX="mpegts"
[ "$PROTO" = "rtmp" ] && MUX="flv"

if [ "$MODE" = "push" ]; then
	OUT=(-f "$MUX" "$PUSH_URL")
	PLAY_HINT="point the source at your server's PLAYBACK url for this stream"
else
	case "$PROTO" in
		rtmp) OUT=(-f flv -listen 1 "rtmp://${BIND_IP}:${RTMP_PORT}/${RTMP_APP}/${RTMP_KEY}")
		      PLAY_HINT="rtmp://<host>:${RTMP_PORT}/${RTMP_APP}/${RTMP_KEY}" ;;
		srt)  OUT=(-f mpegts "srt://${BIND_IP}:${SRT_PORT}?mode=listener&latency=${SRT_LATENCY}")
		      PLAY_HINT="srt://<host>:${SRT_PORT}" ;;
		rist) OUT=(-f mpegts "rist://@${BIND_IP}:${RIST_PORT}")
		      PLAY_HINT="rist://<host>:${RIST_PORT}" ;;
	esac
fi

run_once() {
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
echo " netsim: ${SCENARIO} / ${PROTO} / ${MODE}"
echo " video : ${SIZE}@${FPS}  ${VB}    audio: ${TONE_HZ}Hz tone ${AB}"
if [ "$MODE" = "push" ]; then
	echo " pushing to: ${PUSH_URL}"
	echo " ${PLAY_HINT}"
else
	echo " point the Smooth Media Source at:"
	echo "     ${PLAY_HINT}"
	echo "     (host = 127.0.0.1 if same box, else this machine's IP)"
	[ "$PROTO" = "srt" ] && echo " SRT receive latency: ${SRT_LATENCY} ms"
fi
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
