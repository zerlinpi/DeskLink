#!/usr/bin/env sh
set -eu

if [ "$#" -lt 4 ]; then
  echo "usage: ./bootstrap.sh <domain> <public-ip> <device-id> <controller-account>" >&2
  exit 2
fi

DOMAIN="$1"
PUBLIC_IP="$2"
DEVICE_ID="$3"
CONTROLLER_ACCOUNT="$4"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
SECRETS_DIR="$SCRIPT_DIR/secrets"
ENV_FILE="$SCRIPT_DIR/.env"

command -v docker >/dev/null 2>&1 || {
  echo "docker is required" >&2
  exit 1
}

random_secret() {
  od -An -N48 -tx1 /dev/urandom | tr -d ' \n'
}

mkdir -p "$SECRETS_DIR"
chmod 755 "$SECRETS_DIR"

SIGNAL_SECRET="$(random_secret)"
TURN_SECRET="$(random_secret)"

cat > "$ENV_FILE" <<EOF
DESKLINK_DOMAIN=$DOMAIN
DESKLINK_PUBLIC_IP=$PUBLIC_IP
DESKLINK_TURN_HOST=$DOMAIN
DESKLINK_SIGNAL_AUTH_SECRET=$SIGNAL_SECRET
DESKLINK_TURN_AUTH_SECRET=$TURN_SECRET
DESKLINK_SIGNAL_IMAGE=ghcr.io/zerlinpi/desklink-signal:latest
DESKLINK_CONTROLLER_SESSION_TTL=15m
DESKLINK_SIGNAL_TOKEN_TTL=15m
DESKLINK_TURN_CREDENTIAL_TTL_SECONDS=43200
DESKLINK_ICE_TRANSPORT_POLICY=all
DESKLINK_TURN_TLS_URL=
EOF
chmod 600 "$ENV_FILE"

run_go_tool() {
  tool="$1"
  shift
  # Write registry files as the invoking host user rather than container root.
  # This keeps later chmod/rotation operations usable on a normal non-root
  # deployment account. Go's HOME/GOPATH/GOCACHE live in container /tmp so the
  # unprivileged UID never needs write access to the image filesystem.
  docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e GOPATH=/tmp/go \
    -e GOCACHE=/tmp/go-cache \
    -v "$REPO_ROOT:/repo" \
    -w /repo \
    golang:1.23-alpine \
    go run "$tool" "$@"
}

DEVICE_CREDENTIAL="$(run_go_tool \
  ./tools/auth/rotate-device-registry-credential.go \
  ./infra/production/secrets/devices.json \
  "$DEVICE_ID")"

CONTROLLER_KEY="$(run_go_tool \
  ./tools/auth/set-controller-registry-key.go \
  ./infra/production/secrets/controllers.json \
  "$CONTROLLER_ACCOUNT" \
  "$DEVICE_ID")"

: > "$SECRETS_DIR/revoked-devices.txt"
chmod 444 \
  "$SECRETS_DIR/devices.json" \
  "$SECRETS_DIR/controllers.json" \
  "$SECRETS_DIR/revoked-devices.txt"

cat <<EOF

DeskLink production bootstrap complete.

Domain:             $DOMAIN
Public IP:          $PUBLIC_IP
Device ID:          $DEVICE_ID
Controller account: $CONTROLLER_ACCOUNT

DEVICE CREDENTIAL (store in DeskLink.exe on the Windows host):
$DEVICE_CREDENTIAL

CONTROLLER KEY (enter in the Web controller):
$CONTROLLER_KEY

Next:
  1. Point DNS for $DOMAIN to $PUBLIC_IP.
  2. Open TCP 80/443/3478, UDP 443/3478, and UDP 49160-49200.
  3. cd "$SCRIPT_DIR"
  4. docker compose up -d --build
  5. Open https://$DOMAIN and use controller account "$CONTROLLER_ACCOUNT".

The generated .env is mode 0600. Registry files contain credential hashes only.
Do not publish the device credential or controller key printed above.
EOF
