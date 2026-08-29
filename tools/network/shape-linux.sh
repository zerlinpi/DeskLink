#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sudo ./tools/network/shape-linux.sh <interface> [rate_mbit] [delay_ms] [loss_pct] [jitter_ms]

Examples:
  sudo ./tools/network/shape-linux.sh eth0 8 60 0 5
  sudo ./tools/network/shape-linux.sh eth0 5 120 2 15
  sudo ./tools/network/shape-linux.sh eth0 3 220 8 30

This shapes EGRESS traffic on the selected interface. For bidirectional tests,
apply equivalent shaping on both endpoints (or on both directions of a routed
test gateway). Run reset-linux.sh afterwards.
EOF
}

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "error: run as root (sudo)" >&2
  exit 1
fi

IFACE=${1:-}
RATE_MBIT=${2:-8}
DELAY_MS=${3:-60}
LOSS_PCT=${4:-0}
JITTER_MS=${5:-5}

if [[ -z "$IFACE" ]]; then
  usage
  exit 2
fi

if ! ip link show "$IFACE" >/dev/null 2>&1; then
  echo "error: interface '$IFACE' does not exist" >&2
  exit 2
fi

for value in "$RATE_MBIT" "$DELAY_MS" "$LOSS_PCT" "$JITTER_MS"; do
  if ! [[ "$value" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "error: numeric parameters must be non-negative numbers" >&2
    exit 2
  fi
done

# netem provides repeatable latency/loss/rate pressure suitable for validating
# DeskLink's bitrate -> FPS -> resolution degradation policy. `replace` makes
# rerunning the command idempotent for the selected interface.
tc qdisc replace dev "$IFACE" root netem \
  delay "${DELAY_MS}ms" "${JITTER_MS}ms" distribution normal \
  loss "${LOSS_PCT}%" \
  rate "${RATE_MBIT}mbit"

echo "DeskLink network shaping active on $IFACE"
echo "  egress rate : ${RATE_MBIT} Mbit/s"
echo "  delay       : ${DELAY_MS} ms (+/- ${JITTER_MS} ms distribution)"
echo "  packet loss : ${LOSS_PCT}%"
echo
echo "Reset with: sudo ./tools/network/reset-linux.sh $IFACE"
