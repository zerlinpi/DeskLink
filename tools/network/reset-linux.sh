#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "error: run as root (sudo)" >&2
  exit 1
fi

IFACE=${1:-}
if [[ -z "$IFACE" ]]; then
  echo "Usage: sudo ./tools/network/reset-linux.sh <interface>" >&2
  exit 2
fi

if ! ip link show "$IFACE" >/dev/null 2>&1; then
  echo "error: interface '$IFACE' does not exist" >&2
  exit 2
fi

# `del` returns a non-zero status when no root qdisc is present; that is already
# the desired end state, so treat it as success.
tc qdisc del dev "$IFACE" root 2>/dev/null || true

echo "DeskLink network shaping removed from $IFACE"
