#!/usr/bin/env bash

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
export TRANSPORT=rist
export LAB_ROOT="${LAB_ROOT:-/var/tmp/obs-smooth-wan-rist}"
export NETNS="${NETNS:-obs-smooth-rist}"
export HOST_VETH="${HOST_VETH:-smwanh-rist}"
export NS_VETH="${NS_VETH:-smwann-rist}"
export HOST_ADDRESS="${HOST_ADDRESS:-10.205.0.1/30}"
export NS_ADDRESS="${NS_ADDRESS:-10.205.0.2/30}"
export HOST_GATEWAY="${HOST_GATEWAY:-10.205.0.1}"
export NS_IP="${NS_IP:-10.205.0.2}"
export NETWORK_CIDR="${NETWORK_CIDR:-10.205.0.0/30}"
export PORT="${PORT:-19001}"

exec "${HERE}/wan-netem.sh" "$@"
