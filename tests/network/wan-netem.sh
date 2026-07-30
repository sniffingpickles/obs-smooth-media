#!/usr/bin/env bash
#
# Isolated Linux WAN impairment lab for Smooth Media Source.
#
# The media endpoint runs in a network namespace. Only traffic for the
# dedicated test transport crosses the shaped veth pair, so netem never
# affects SSH or unrelated host traffic.
#
# Usage:
#   sudo ./wan-netem.sh setup [external-interface]
#   sudo ./wan-netem.sh make-fixture
#   sudo ./wan-netem.sh start-feed [RIST destination host]
#   sudo ./wan-netem.sh apply <scenario>
#   sudo ./wan-netem.sh stats
#   sudo ./wan-netem.sh stop-feed
#   sudo ./wan-netem.sh cleanup
#
# Scenarios:
#   clean mild high-rtt jitter-reorder loss-burst rate-2m rate-1m
#   rate-384k collapse extreme blackout

set -euo pipefail

LAB_ROOT="${LAB_ROOT:-/var/tmp/obs-smooth-wan-lab}"
TRANSPORT="${TRANSPORT:-srt}"
NETNS="${NETNS:-obs-smooth-wan}"
HOST_VETH="${HOST_VETH:-smwan-host}"
NS_VETH="${NS_VETH:-smwan-ns}"
HOST_ADDRESS="${HOST_ADDRESS:-10.203.0.1/30}"
NS_ADDRESS="${NS_ADDRESS:-10.203.0.2/30}"
HOST_GATEWAY="${HOST_GATEWAY:-10.203.0.1}"
NS_IP="${NS_IP:-10.203.0.2}"
NETWORK_CIDR="${NETWORK_CIDR:-10.203.0.0/30}"
PORT="${PORT:-19000}"
SRT_LATENCY="${SRT_LATENCY:-3000}"
RTMP_RW_TIMEOUT_US="${RTMP_RW_TIMEOUT_US:-10000000}"
FIXTURE_SECONDS="${FIXTURE_SECONDS:-60}"
FIXTURE_VIDEO_BITRATE="${FIXTURE_VIDEO_BITRATE:-3500k}"
EXTERNAL_IF_FILE="${LAB_ROOT}/external-interface"
FORWARDING_FILE="${LAB_ROOT}/original-ip-forward"
FEED_PID_FILE="${LAB_ROOT}/ffmpeg.pid"
FEED_LOG="${LAB_ROOT}/ffmpeg-${TRANSPORT}.log"
EVENT_LOG="${LAB_ROOT}/events.jsonl"
FIXTURE="${LAB_ROOT}/fixture.mkv"

case "${TRANSPORT}" in
srt|rist) L4_PROTOCOL="udp" ;;
rtmp) L4_PROTOCOL="tcp" ;;
*)
	echo "error: unsupported TRANSPORT=${TRANSPORT}" >&2
	exit 2
	;;
esac

usage()
{
	echo "usage: $0 setup [external-interface] | make-fixture | start-feed [RIST destination host] | stop-feed | apply <scenario> | stats | cleanup" >&2
	exit 2
}

require_root()
{
	if [[ "${EUID}" -ne 0 ]]; then
		echo "error: root is required" >&2
		exit 1
	fi
}

log_event()
{
	local action="$1"
	local detail="$2"
	mkdir -p "${LAB_ROOT}"
	printf '{"utc":"%s","action":"%s","detail":"%s"}\n' \
		"$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)" "${action}" "${detail}" |
		tee -a "${EVENT_LOG}"
}

external_interface()
{
	if [[ -f "${EXTERNAL_IF_FILE}" ]]; then
		cat "${EXTERNAL_IF_FILE}"
	else
		ip route show default | awk 'NR == 1 { print $5 }'
	fi
}

delete_firewall_rules()
{
	local external_if
	external_if="$(external_interface)"
	if [[ "${TRANSPORT}" != "rist" ]]; then
		iptables -t nat -D PREROUTING -i "${external_if}" \
			-p "${L4_PROTOCOL}" --dport "${PORT}" -j DNAT \
			--to-destination "${NS_IP}:${PORT}" 2>/dev/null || true
		iptables -D FORWARD -i "${external_if}" -o "${HOST_VETH}" \
			-p "${L4_PROTOCOL}" -d "${NS_IP}" --dport "${PORT}" \
			-j ACCEPT 2>/dev/null || true
		iptables -D FORWARD -i "${HOST_VETH}" -o "${external_if}" \
			-p "${L4_PROTOCOL}" -s "${NS_IP}" --sport "${PORT}" \
			-j ACCEPT 2>/dev/null || true
	else
		iptables -D FORWARD -i "${HOST_VETH}" -o "${external_if}" \
			-p udp -s "${NS_IP}" --dport "${PORT}" -j ACCEPT \
			2>/dev/null || true
		iptables -D FORWARD -i "${external_if}" -o "${HOST_VETH}" \
			-p udp -d "${NS_IP}" --sport "${PORT}" -j ACCEPT \
			2>/dev/null || true
	fi
	iptables -t nat -D POSTROUTING -s "${NETWORK_CIDR}" -o "${external_if}" \
		-j MASQUERADE 2>/dev/null || true
}

setup_lab()
{
	local external_if="${1:-$(external_interface)}"

	mkdir -p "${LAB_ROOT}"
	printf '%s\n' "${external_if}" >"${EXTERNAL_IF_FILE}"
	if [[ ! -f "${FORWARDING_FILE}" ]]; then
		sysctl -n net.ipv4.ip_forward >"${FORWARDING_FILE}"
	fi

	stop_feed
	delete_firewall_rules
	ip netns delete "${NETNS}" 2>/dev/null || true
	ip link delete "${HOST_VETH}" 2>/dev/null || true

	ip netns add "${NETNS}"
	ip link add "${HOST_VETH}" type veth peer name "${NS_VETH}"
	ip link set "${NS_VETH}" netns "${NETNS}"
	ip address add "${HOST_ADDRESS}" dev "${HOST_VETH}"
	ip link set "${HOST_VETH}" up
	ip netns exec "${NETNS}" ip address add "${NS_ADDRESS}" dev "${NS_VETH}"
	ip netns exec "${NETNS}" ip link set lo up
	ip netns exec "${NETNS}" ip link set "${NS_VETH}" up
	ip netns exec "${NETNS}" ip route add default via "${HOST_GATEWAY}"

	sysctl -q -w net.ipv4.ip_forward=1
	if [[ "${TRANSPORT}" != "rist" ]]; then
		iptables -t nat -A PREROUTING -i "${external_if}" \
			-p "${L4_PROTOCOL}" --dport "${PORT}" -j DNAT \
			--to-destination "${NS_IP}:${PORT}"
		iptables -A FORWARD -i "${external_if}" -o "${HOST_VETH}" \
			-p "${L4_PROTOCOL}" -d "${NS_IP}" --dport "${PORT}" \
			-j ACCEPT
		iptables -A FORWARD -i "${HOST_VETH}" -o "${external_if}" \
			-p "${L4_PROTOCOL}" -s "${NS_IP}" --sport "${PORT}" \
			-j ACCEPT
	else
		iptables -A FORWARD -i "${HOST_VETH}" -o "${external_if}" \
			-p udp -s "${NS_IP}" --dport "${PORT}" -j ACCEPT
		iptables -A FORWARD -i "${external_if}" -o "${HOST_VETH}" \
			-p udp -d "${NS_IP}" --sport "${PORT}" -j ACCEPT
	fi
	iptables -t nat -A POSTROUTING -s "${NETWORK_CIDR}" -o "${external_if}" \
		-j MASQUERADE
	apply_scenario clean
	log_event setup \
		"transport=${TRANSPORT},interface=${external_if},port=${PORT}"
}

make_fixture()
{
	mkdir -p "${LAB_ROOT}"
	log_event fixture-start \
		"seconds=${FIXTURE_SECONDS},video=${FIXTURE_VIDEO_BITRATE}"
	ffmpeg -hide_banner -nostdin -y -loglevel warning \
		-f lavfi -i "testsrc2=size=1280x720:rate=30" \
		-f lavfi -i "sine=frequency=880:sample_rate=48000" \
		-t "${FIXTURE_SECONDS}" \
		-c:v libx264 -preset ultrafast -tune zerolatency -profile:v high \
		-b:v "${FIXTURE_VIDEO_BITRATE}" -maxrate "${FIXTURE_VIDEO_BITRATE}" \
		-bufsize 7000k -g 60 -pix_fmt yuv420p \
		-c:a aac -b:a 160k -ar 48000 -ac 2 \
		"${FIXTURE}"
	log_event fixture-complete "$(stat -c %s "${FIXTURE}")-bytes"
}

feed_is_running()
{
	[[ -f "${FEED_PID_FILE}" ]] &&
		kill -0 "$(cat "${FEED_PID_FILE}")" 2>/dev/null
}

start_feed()
{
	local rist_destination="${1:-}"
	if [[ "${TRANSPORT}" == "rist" && -z "${rist_destination}" ]]; then
		echo "error: RIST start-feed requires a destination host" >&2
		exit 2
	fi
	if [[ ! -f "${FIXTURE}" ]]; then
		make_fixture
	fi
	stop_feed
	: >"${FEED_LOG}"
	nohup setsid ip netns exec "${NETNS}" bash -c '
		while true; do
			case "$1" in
			srt)
				ffmpeg -hide_banner -nostdin -loglevel info \
					-re -stream_loop -1 -i "$2" \
					-map 0:v:0 -map 0:a:0 -c copy -f mpegts \
					"srt://0.0.0.0:$3?mode=listener&latency=$4&transtype=live"
				;;
			rtmp)
				ffmpeg -hide_banner -nostdin -loglevel info \
					-re -stream_loop -1 -i "$2" \
					-map 0:v:0 -map 0:a:0 -c copy \
					-rw_timeout "$6" -listen 1 -f flv \
					"rtmp://0.0.0.0:$3/live/test"
				;;
			rist)
				ffmpeg -hide_banner -nostdin -loglevel info \
					-re -stream_loop -1 -i "$2" \
					-map 0:v:0 -map 0:a:0 -c copy \
					-buffer_size "$4" -f mpegts \
					"rist://$5:$3"
				;;
			esac
			status=$?
			printf "supervisor: ffmpeg exited status=%d utc=%s; rearming\n" \
				"${status}" "$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)"
			sleep 1
		done
	' obs-smooth-feed "${TRANSPORT}" "${FIXTURE}" "${PORT}" \
		"${SRT_LATENCY}" "${rist_destination}" \
		"${RTMP_RW_TIMEOUT_US}" \
		>"${FEED_LOG}" 2>&1 &
	printf '%s\n' "$!" >"${FEED_PID_FILE}"
	sleep 1
	if ! feed_is_running; then
		tail -100 "${FEED_LOG}" >&2
		echo "error: ffmpeg listener failed to start" >&2
		exit 1
	fi
	local detail="transport=${TRANSPORT},pid=$(cat "${FEED_PID_FILE}")"
	if [[ "${TRANSPORT}" == "rtmp" ]]; then
		detail+=",rw-timeout-us=${RTMP_RW_TIMEOUT_US}"
	fi
	log_event feed-start "${detail}"
}

stop_feed()
{
	if feed_is_running; then
		kill -- "-$(cat "${FEED_PID_FILE}")" 2>/dev/null || true
		for _ in {1..20}; do
			feed_is_running || break
			sleep 0.1
		done
		if feed_is_running; then
			kill -9 -- "-$(cat "${FEED_PID_FILE}")" 2>/dev/null || true
		fi
		log_event feed-stop "stopped"
	fi
	rm -f "${FEED_PID_FILE}"
}

replace_netem()
{
	local -a arguments=("$@")
	# Delete before add so options omitted by the next scenario cannot leak
	# from the previous netem instance (notably a prior rate ceiling).
	tc qdisc delete dev "${HOST_VETH}" root 2>/dev/null || true
	ip netns exec "${NETNS}" tc qdisc delete dev "${NS_VETH}" root \
		2>/dev/null || true
	tc qdisc add dev "${HOST_VETH}" root netem limit 4000 "${arguments[@]}"
	ip netns exec "${NETNS}" tc qdisc add dev "${NS_VETH}" root \
		netem limit 4000 "${arguments[@]}"
}

apply_scenario()
{
	local scenario="${1:-}"
	case "${scenario}" in
	clean)
		tc qdisc delete dev "${HOST_VETH}" root 2>/dev/null || true
		ip netns exec "${NETNS}" tc qdisc delete dev "${NS_VETH}" root \
			2>/dev/null || true
		;;
	mild)
		replace_netem delay 80ms 40ms distribution normal \
			loss 1% 25%
		;;
	high-rtt)
		replace_netem delay 750ms 250ms distribution normal \
			loss 2% 25%
		;;
	jitter-reorder)
		replace_netem delay 220ms 200ms distribution normal \
			loss 5% 40% duplicate 1% reorder 25% 50%
		;;
	loss-burst)
		replace_netem delay 120ms 80ms distribution normal \
			loss 20% 65%
		;;
	rate-2m)
		replace_netem rate 2mbit
		;;
	rate-1m)
		replace_netem rate 1mbit
		;;
	rate-384k)
		replace_netem rate 384kbit
		;;
	collapse)
		replace_netem delay 250ms 150ms distribution normal \
			loss 5% 40% rate 384kbit
		;;
	extreme)
		replace_netem delay 1000ms 500ms distribution normal \
			loss 30% 60% duplicate 2% reorder 25% 50% rate 512kbit
		;;
	blackout)
		replace_netem loss 100%
		;;
	*)
		echo "unknown scenario: ${scenario}" >&2
		exit 2
		;;
	esac
	log_event scenario "${scenario}"
}

show_stats()
{
	echo "host-to-namespace:"
	tc -s qdisc show dev "${HOST_VETH}"
	echo "namespace-to-host:"
	ip netns exec "${NETNS}" tc -s qdisc show dev "${NS_VETH}"
	echo "feed:"
	if feed_is_running; then
		ps -ww -o pid,etime,rss,%cpu,cmd -p "$(cat "${FEED_PID_FILE}")"
	else
		echo "not running"
	fi
}

cleanup_lab()
{
	stop_feed
	delete_firewall_rules
	ip netns delete "${NETNS}" 2>/dev/null || true
	ip link delete "${HOST_VETH}" 2>/dev/null || true
	if [[ -f "${FORWARDING_FILE}" ]]; then
		sysctl -q -w "net.ipv4.ip_forward=$(cat "${FORWARDING_FILE}")"
	fi
	log_event cleanup "complete"
}

require_root
command="${1:-}"
case "${command}" in
setup) setup_lab "${2:-}" ;;
make-fixture) make_fixture ;;
start-feed) start_feed "${2:-}" ;;
stop-feed) stop_feed ;;
apply) apply_scenario "${2:-}" ;;
stats) show_stats ;;
cleanup) cleanup_lab ;;
*) usage ;;
esac
