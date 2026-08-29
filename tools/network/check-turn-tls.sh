#!/usr/bin/env bash
set -euo pipefail

HOST=${1:-}
PORT=${2:-5349}

if [[ -z "$HOST" ]]; then
  echo "Usage: ./tools/network/check-turn-tls.sh <turn-hostname> [port]" >&2
  exit 2
fi

if ! command -v openssl >/dev/null 2>&1; then
  echo "error: openssl is required" >&2
  exit 1
fi

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

# This verifies TCP reachability, TLS negotiation, hostname/SNI handling and the
# certificate chain. It does not perform a TURN allocation/authentication.
if ! openssl s_client \
  -connect "${HOST}:${PORT}" \
  -servername "$HOST" \
  -verify_hostname "$HOST" \
  -verify_return_error \
  -brief </dev/null >"$TMP" 2>&1; then
  cat "$TMP" >&2
  echo "TURN/TLS endpoint check failed" >&2
  exit 1
fi

cat "$TMP"
echo "TURN/TLS endpoint is reachable and its certificate matches $HOST"
