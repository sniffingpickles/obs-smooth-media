#!/usr/bin/env bash
#
# Timed hostile-network matrix for the isolated wan-netem lab.
# Run while the Windows obs-e2e harness polls the source once per second.

set -euo pipefail

LAB_SCRIPT="${LAB_SCRIPT:-/root/wan-netem.sh}"
LAB_ROOT="${LAB_ROOT:-/var/tmp/obs-smooth-wan-lab}"
FEED_START_ARGS="${FEED_START_ARGS:-}"
RESTART_FEED_AFTER_BLACKOUT="${RESTART_FEED_AFTER_BLACKOUT:-0}"
MATRIX_LOG="${MATRIX_LOG:-${LAB_ROOT}/matrix.log}"

snapshot()
{
	{
		echo "utc=$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)"
		"${LAB_SCRIPT}" stats
	} >>"${MATRIX_LOG}" 2>&1
}

run_for()
{
	local scenario="$1"
	local seconds="$2"
	echo "[$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)] ${scenario} ${seconds}s" |
		tee -a "${MATRIX_LOG}"
	"${LAB_SCRIPT}" apply "${scenario}" >>"${MATRIX_LOG}" 2>&1
	sleep "${seconds}"
	snapshot
}

ensure_feed()
{
	if ! "${LAB_SCRIPT}" stats 2>/dev/null | grep -q "obs-smooth-feed"; then
		echo "[$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)] restarting feed" |
			tee -a "${MATRIX_LOG}"
		# shellcheck disable=SC2086
		"${LAB_SCRIPT}" start-feed ${FEED_START_ARGS} \
			>>"${MATRIX_LOG}" 2>&1
	fi
}

restart_feed()
{
	echo "[$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)] restarting feed after blackout" |
		tee -a "${MATRIX_LOG}"
	"${LAB_SCRIPT}" stop-feed >>"${MATRIX_LOG}" 2>&1
	# shellcheck disable=SC2086
	"${LAB_SCRIPT}" start-feed ${FEED_START_ARGS} \
		>>"${MATRIX_LOG}" 2>&1
}

: >"${MATRIX_LOG}"
run_for clean 45
run_for mild 45
run_for clean 15
run_for high-rtt 45
run_for clean 20
run_for jitter-reorder 45
run_for clean 20
run_for loss-burst 45
run_for clean 30
run_for collapse 45
run_for clean 30
run_for extreme 45
run_for clean 45

for _ in {1..5}; do
	run_for high-rtt 5
	run_for clean 10
done

for _ in {1..5}; do
	run_for collapse 5
	run_for mild 10
done

# Stay dark longer than the SRT transport timeout, then restart the listener
# if FFmpeg exited after its peer timed out.
run_for blackout 40
run_for clean 5
if [[ "${RESTART_FEED_AFTER_BLACKOUT}" == "1" ]]; then
	# FFmpeg's single-client RTMP listen muxer can remain blocked writing
	# the dead TCP connection and therefore never accept OBS's reconnect.
	# A real ingest service accepts a fresh client, so explicitly re-arm
	# one-shot test servers before measuring client recovery.
	restart_feed
else
	ensure_feed
fi
run_for clean 90

echo "[$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)] matrix complete" |
	tee -a "${MATRIX_LOG}"
