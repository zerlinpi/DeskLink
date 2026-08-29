# DeskLink protocol v0

DeskLink separates **signaling**, **media**, and **control** paths. Signaling never carries remote desktop video/audio payloads.

## Signaling

WebSocket endpoint:

```text
/ws?deviceId=<id>
```

A normal client-to-server signaling envelope is:

```json
{
  "type": "offer|answer|ice|auth-request|auth-challenge|auth-proof|auth-accepted|auth-rejected|ping",
  "target": "remote-device-id",
  "session": "uuid",
  "payload": {}
}
```

For routed signaling messages the server preserves `type`, `session` and `payload`, and adds `from` with the authenticated sender ID.

Browser runtime controller authorization is carried during the WebSocket handshake with the `desklink-v1` subprotocol plus a `desklink-auth.<short-lived-token>` subprotocol entry. Runtime controller tokens are intentionally not placed in the WebSocket URL. The older `auth` query parameter remains only for compatible native/development registration paths.

## Remote-control Access Code authorization

A reusable Access Code is **never sent in an offer**. A new remote-control session must complete a challenge/response before the initial PeerConnection is created.

### 1. Controller requests a challenge

```json
{
  "type": "auth-request",
  "target": "office-pc",
  "session": "uuid",
  "payload": {"version": 1}
}
```

If the host is temporarily offline, signaling still returns `peer-offline` so the controller can show the state. For `auth-request` only, signaling keeps a bounded in-memory pending request for up to 10 minutes. The request is bound to the exact authenticated controller WebSocket connection plus session, deduplicated on that connection, capped at 32 requests per target and 512 globally, and removed immediately when that controller connection closes. When the host registers/reconnects, still-valid requests are automatically forwarded. SDP, ICE and arbitrary application messages are never queued this way.

When the request was actually accepted into that queue, `peer-offline` explicitly reports the wait contract:

```json
{
  "type": "peer-offline",
  "target": "office-pc",
  "payload": {
    "authQueued": true,
    "expiresInMs": 600000
  }
}
```

`authQueued` must be omitted/false when the request was not queued, including scope rejection, revocation, non-auth signaling or queue-capacity failure. Controllers must not display an automatic-wait state unless the server explicitly returns `authQueued: true`.

The browser keeps that wait recoverable in two ways. While the signaling socket stays open it refreshes the same `auth-request` shortly before the advertised pending lifetime expires, which refreshes the existing connection/session entry rather than creating a high-frequency polling loop. If the signaling WebSocket itself closes before a PeerConnection exists, the browser reconnects with exponential backoff and sends a fresh `auth-request`; the old connection-bound pending entry is discarded by the server and the new authenticated connection establishes its own wait entry.

Offline queuing is available only to a peer authenticated with controller scope. Host identities and unauthenticated local-development peers still receive `peer-offline` but cannot occupy the pending controller-auth queue. A short dispatch guard also suppresses duplicate `auth-request` delivery from the same live controller connection/session when queue flush and direct forwarding overlap. A genuinely new controller connection is a different queue/dispatch identity and cannot inherit stale pending state from the previous connection.

If signaling finds a target online but the WebSocket write fails during forwarding, it removes that stale target registration and reports `peer-offline`. For a controller `auth-request`, the dispatch claim is released and the request is returned to the connection-bound pending queue so the next host registration can resume authentication immediately.

The signaling CI includes a real-process WebSocket end-to-end test for this path. It verifies controller subprotocol authentication, offline queue acknowledgement, automatic delivery after host registration, queued/delivered/forwarded metrics, target-scope rejection without queue growth, and the rule that a reconnected logical controller cannot inherit the previous socket's pending request.

### 2. Host issues a one-time challenge

```json
{
  "type": "auth-challenge",
  "target": "web-controller-id",
  "session": "uuid",
  "payload": {
    "algorithm": "hmac-sha256-v1",
    "nonce": "64-lowercase-hex-characters"
  }
}
```

The host nonce is 32 cryptographically random bytes encoded as 64 lowercase hexadecimal characters. A challenge is short-lived (currently 15 seconds), bound to the exact controller ID + host ID + session ID, and must be treated as one-time material.

### 3. Controller computes the proof locally

The Access Code is used as the raw HMAC-SHA256 key. The UTF-8 message is exactly these five fields joined with a single LF byte (`0x0a`) and **no trailing newline**:

```text
DeskLink access proof v1
<controller-id>
<host-id>
<session-id>
<nonce>
```

Equivalent construction:

```text
"DeskLink access proof v1" + "\n" +
controllerId + "\n" +
hostId + "\n" +
sessionId + "\n" +
nonce
```

The resulting 32-byte HMAC is encoded as 64 lowercase hexadecimal characters.

The controller then sends:

```json
{
  "type": "auth-proof",
  "target": "office-pc",
  "session": "uuid",
  "payload": {
    "algorithm": "hmac-sha256-v1",
    "proof": "64-lowercase-hex-characters"
  }
}
```

The browser implementation uses Web Crypto. The Windows host uses Windows BCrypt. CI contains a fixed cross-language proof vector so canonical-string or encoding drift causes a build failure.

### 4. Host accepts or rejects

On success:

```json
{
  "type": "auth-accepted",
  "target": "web-controller-id",
  "session": "uuid",
  "payload": {"version": 1}
}
```

Only after `auth-accepted` does the controller create/send the initial WebRTC offer. The authorization grant is itself short-lived/one-time for the initial offer. Once that exact session owns an active PeerConnection, ICE restarts/renegotiation can continue without repeating the Access Code proof unless a new session is created.

Failures use:

```json
{
  "type": "auth-rejected",
  "payload": {"reason": "invalid-access-code"}
}
```

Current host-side reasons also include `host-unconfigured`, `auth-busy` and `auth-unavailable`.

The host ignores unauthenticated ICE/offer traffic for a new session. The browser also accepts host-scoped challenge/answer/ICE/auth messages only when both `from` and `session` match the selected host and current session.

### Security boundary

This challenge/response prevents the reusable Access Code itself from being exposed to the signaling service or ordinary signaling logs, and captured proofs cannot simply be replayed against a new nonce/session.

However, plain HMAC with a human-memorable low-entropy Access Code is not a PAKE. An observer that can capture challenge + proof can perform offline guesses. Production deployments should therefore use a long random high-entropy Access Code. A future low-entropy password UX should use a PAKE such as OPAQUE/SPAKE2-class design rather than weakening this protocol to plaintext password transport.

## WebRTC topology

- Video: host -> controller, H.264.
- Audio: optional, later milestone.
- `control` DataChannel: controller -> host; ordered/reliable. Used for clicks and keyboard input that must not be lost.
- `pointer` DataChannel: controller -> host; unordered with `maxRetransmits=0`. Used for pointer movement and wheel events where stale input should be discarded rather than retransmitted.
- Telemetry: controller reports decoder/network observations over the reliable control channel for adaptive bitrate/FPS/resolution decisions.

ICE policy normally prefers direct candidates and uses TURN as fallback. A relay-only policy exists for validation/restrictive-network testing.

## Offer / answer / ICE ordering

The browser buffers local ICE candidates until its offer has been sent over the ordered signaling WebSocket. This prevents pre-offer ICE from reaching a host that has not yet bound the controller/session.

Remote ICE received before the browser has installed the answer is buffered and flushed after `setRemoteDescription` succeeds.

A new initial offer must never contain an `accessCode` field. Any implementation that places a reusable Access Code in offer/SDP/ICE signaling is incompatible with the current security protocol.

## Media

The Windows host pipeline is:

```text
DXGI Desktop Duplication (BGRA D3D11 texture)
  -> D3D11 Video Processor (GPU scale + NV12)
  -> Media Foundation hardware H.264 encoder
  -> Annex-B access unit
  -> libdatachannel H.264 RTP packetizer
  -> WebRTC video track
  -> browser H.264 decoder
```

The browser advertises H.264 receive codecs and the host reads the negotiated H.264 RTP payload type from the offer instead of hard-coding payload type 96/102.

## Control messages

Coordinates are normalized to `[0, 1]` so controller and host resolutions can differ.

Reliable `control` channel:

```json
{"t":"pointer","kind":"down","button":0,"x":0.41,"y":0.63,"buttons":1}
{"t":"pointer","kind":"up","button":0,"x":0.41,"y":0.63,"buttons":0}
{"t":"key","kind":"down","code":"KeyA","key":"a"}
{"t":"key","kind":"up","code":"KeyA","key":"a"}
{"t":"release-all"}
```

Unreliable `pointer` channel:

```json
{"t":"pointer","kind":"move","x":0.41,"y":0.63,"button":0,"buttons":0}
{"t":"wheel","delta":-120}
```

Browser pointer-move events are coalesced to at most display animation cadence with `requestAnimationFrame`; only the newest pending position is sent.

`release-all` is sent on browser blur/visibility loss/disconnect and is also applied by the host when the reliable control channel closes, preventing remotely injected modifiers/buttons from remaining pressed after an abnormal disconnect.

## Performance rules

1. Interaction latency wins over image quality.
2. Do not queue stale video frames or stale pointer moves.
3. Target capture-to-display latency: <100 ms on a healthy LAN and <180 ms on a normal WAN where route/codec performance permits it; these are targets, not guarantees.
4. Start at up to 1080p/60 and 12 Mbps by default when hardware encoding and bandwidth allow it.
5. On congestion, reduce bitrate first, then FPS/resolution; recover gradually to avoid oscillation.
6. Default keyframe/GOP target is about one second, plus immediate keyframes after connection/recovery/PLI when needed.
7. Do not continuously encode while no authenticated controller is connected; retain only the latest converted GPU frame for fast static-desktop recovery.
8. Keep input/control traffic independent from video queue pressure.
