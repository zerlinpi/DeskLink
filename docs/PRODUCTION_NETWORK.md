# DeskLink production network deployment

This document focuses on the network path required for a Sunlogin/UU Remote-like experience. The priority order is always:

1. direct ICE/P2P when possible;
2. TURN over UDP when relay is required;
3. TURN over TCP for restrictive NAT/firewalls;
4. TURN over TLS (`turns:`) as the final compatibility path.

The media path must never be proxied through the signaling WebSocket service.

## Recommended public topology

Use separate DNS names even if services initially share one VM:

- `control.example.com` -> HTTPS/WSS signaling reverse proxy.
- `turn.example.com` -> coturn public address.

For production, place TURN nodes geographically close to users. A single distant relay can make a correctly implemented remote desktop feel slow because every media/control packet detours through that relay.

## Ports

Recommended baseline:

- TCP 443: HTTPS/WSS signaling.
- UDP/TCP 3478: STUN/TURN.
- TCP 5349: TURN/TLS (`turns:`).
- UDP 49160-49299: TURN relay allocation range in the supplied TLS example.

A provider may also expose TURN/TLS on TCP 443 on a dedicated IP when enterprise/hotel networks only permit outbound 443. Do not place a normal HTTPS reverse proxy in front of TURN unless the proxy explicitly supports raw TCP/TLS forwarding; TURN is not HTTP.

## coturn

Start from `infra/coturn/turnserver.tls.example.conf` and replace all placeholders. Important requirements:

- use a real DNS hostname and publicly trusted certificate;
- set `external-ip=PUBLIC_IP/PRIVATE_IP` when the TURN node is behind 1:1 NAT;
- open the complete relay UDP range in the cloud security group and host firewall;
- use `use-auth-secret` + `static-auth-secret` only with a long random secret stored outside source control;
- mint short-lived TURN credentials in the application backend rather than distributing the shared secret;
- monitor relay bandwidth because remote desktop video can consume several Mbps per active session.

Validate the TLS listener before browser testing:

```bash
./tools/network/check-turn-tls.sh turn.example.com 5349
```

## Browser controller

Normal profile should prefer direct/UDP paths:

```dotenv
VITE_SIGNAL_URL=wss://control.example.com/ws
VITE_STUN_URL=stun:turn.example.com:3478
VITE_TURN_URL=turn:turn.example.com:3478
VITE_TURN_USERNAME=TEMPORARY_USERNAME
VITE_TURN_PASSWORD=TEMPORARY_PASSWORD
```

For a dedicated restrictive-network validation build, copy `apps/web/.env.restrictive.example` to `.env.local`. That profile uses `turns:turn.example.com:5349` so the same controller can be tested in networks that block ordinary TURN/UDP/TCP.

The current browser code accepts one TURN base URL at build/runtime environment configuration. A later controller improvement should register UDP/TCP/TLS TURN URLs simultaneously so ICE can fall through all relay transports in a single session without rebuilding the controller.

## Windows host

The Windows host currently registers STUN plus TURN/UDP and TURN/TCP candidates through libdatachannel. TURN/TLS is the next native-host transport addition. Do not claim full restrictive-network coverage for the native host until `TurnTls` is wired into `SessionConfig` and validated in CI/runtime testing.

## Regional relay policy

For low latency, route devices to the nearest healthy TURN region. A practical initial layout for East Asia could be:

- Japan/Korea region;
- North/East China-adjacent region where legally/operationally appropriate;
- South China/Hong Kong-adjacent region where legally/operationally appropriate;
- Southeast Asia region.

The exact hosting regions depend on the product's legal, network and user-distribution requirements. Measure real RTT instead of assuming geographic distance equals network distance.

## Acceptance targets

Use the HUD and `docs/NETWORK_TESTING.md` rather than subjective visual checks. For each route, record:

- selected route: Direct or Relay;
- relay protocol: UDP/TCP/TLS where observable;
- RTT, jitter and packet loss;
- decoded FPS;
- resolution tier and bitrate behavior;
- input responsiveness during congestion;
- time to recover after a network path change.

A relay path is acceptable only if it remains interactive under the target region's real RTT. If TURN/TCP/TLS is functional but consistently sluggish, the fix is usually relay placement/capacity rather than increasing video bitrate.
