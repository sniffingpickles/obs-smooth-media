#!/usr/bin/env bash

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
export TRANSPORT=rtmp
export LAB_ROOT="${LAB_ROOT:-/var/tmp/obs-smooth-wan-rtmp}"
export NETNS="${NETNS:-obs-smooth-rtmp}"
export HOST_VETH="${HOST_VETH:-smwanh-rtmp}"
export NS_VETH="${NS_VETH:-smwann-rtmp}"
export HOST_ADDRESS="${HOST_ADDRESS:-10.204.0.1/30}"
export NS_ADDRESS="${NS_ADDRESS:-10.204.0.2/30}"
export HOST_GATEWAY="${HOST_GATEWAY:-10.204.0.1}"
export NS_IP="${NS_IP:-10.204.0.2}"
export NETWORK_CIDR="${NETWORK_CIDR:-10.204.0.0/30}"
export PORT="${PORT:-1936}"

exec "${HERE}/wan-netem.sh" "$@"
