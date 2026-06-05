#!/usr/bin/env bash
#
# run-all.sh — run every netsim scenario in sequence against one destination,
# with timestamped markers so the OBS log can be matched to each scenario.
#
#   ./run-all.sh <rtmp|srt|rist|full-push-url> [seconds-per-scenario]
#
# Each scenario is a separate publish, so the source will reconnect between
# scenarios (that's expected and exercises reconnect handling too). Identify
# scenarios in the OBS DBG log by behavior: slow->rate~0.97, veryslow->~0.93,
# fast->~1.03, burst->initial trim, stall->repeated reconnects.
#
set -u
DEST="${1:-}"
DUR="${2:-40}"
[ -z "$DEST" ] && { echo "usage: $0 <rtmp|srt|rist|push-url> [secs-per-scenario]"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
SCENARIOS="clean slow veryslow fast burst stall"

cleanup() { pkill -f "lavfi" 2>/dev/null; }
trap 'echo; echo "[$(date +%H:%M:%S)] aborted by user"; cleanup; exit 0' INT

echo "############################################################"
echo "# netsim run-all -> $DEST   (${DUR}s each)"
echo "# turn ON Verbose Debug Logging in the source first."
echo "############################################################"

for s in $SCENARIOS; do
	echo
	echo "[$(date +%H:%M:%S)] ===== SCENARIO: $s (${DUR}s) ====="
	"$HERE/netsim.sh" "$s" "$DEST" >/tmp/netsim_runall.log 2>&1 &
	PID=$!
	sleep "$DUR"
	kill "$PID" 2>/dev/null
	cleanup
	sleep 2
done

echo
echo "[$(date +%H:%M:%S)] ===== all scenarios complete ====="
