#!/usr/bin/env bash
#
# Isolate bitrate collapse from latency, jitter, and packet loss. The source
# feed is approximately 4.1 Mbps; the shaped stages cap it progressively lower.

set -euo pipefail

LAB_SCRIPT="${LAB_SCRIPT:-/root/wan-netem.sh}"
LAB_ROOT="${LAB_ROOT:-/var/tmp/obs-smooth-wan-lab}"
LOG="${LOG:-${LAB_ROOT}/bitrate-cliffs.log}"

run_for()
{
	local scenario="$1"
	local seconds="$2"
	printf '[%s] %s %ss\n' \
		"$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)" "${scenario}" "${seconds}" |
		tee -a "${LOG}"
	"${LAB_SCRIPT}" apply "${scenario}" >>"${LOG}" 2>&1
	sleep "${seconds}"
	"${LAB_SCRIPT}" stats >>"${LOG}" 2>&1
}

: >"${LOG}"
run_for clean 30
run_for rate-2m 20
run_for clean 20
run_for rate-1m 15
run_for clean 25
run_for rate-384k 10
run_for clean 30

for _ in {1..3}; do
	run_for rate-384k 5
	run_for clean 15
done

run_for clean 45
printf '[%s] bitrate-cliff matrix complete\n' \
	"$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)" | tee -a "${LOG}"
