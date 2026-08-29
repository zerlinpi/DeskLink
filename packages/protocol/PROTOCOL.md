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

### Registration lifetime and live device revocation

Authenticated signaling registration is not perpetual. The server retains the verified expiry of both native-host signaling tokens and browser controller `ct1` tokens and actively closes the corresponding WebSocket when that registration token expires. A reconnect must therefore obtain/use a currently valid token again; an already-open socket cannot outlive the short-lived credential that authorized it.

Device revocation is also enforced against already-connected peers. The signaling service performs a shared revocation sweep every 5 seconds. The revocation source is loaded once per sweep and then applied to the current peer snapshot, so the revocation-file I/O cost does not grow linearly with the number of connected peers. A controller is checked against the target device encoded in its scoped token; a native host is checked against its own device ID. Authentication/token endpoints still check the revocation source directly so new registrations fail closed immediately.

When a live device/target becomes revoked, the server emits the server-owned event below before closing the affected signaling connection:

```json
{
  "type": "device-revoked",
  "target": "office-pc"
}
```

`device-revoked` is deliberately **not** an allowed client-originated signaling type. A controller or host attempting to send that type receives the normal unsupported-signal error; only the signaling service may originate the forced-termination event.

The browser treats `device-revoked` as a terminal state for the current remote-control attempt: it sends `release-all` when possible, stops telemetry and retry/wait timers, closes DataChannels/PeerConnection/WebSocket, clears the runtime controller token/session state, and does not automatically reconnect. The Windows host likewise releases injected keyboard/mouse state, closes the active PeerConnection, clears controller/session/challenge/one-time-offer authorization state, latches the registration as revoked, and suppresses signaling reconnect for the remainder of that `WebRtcSession` run. Explicit restart/reinitialization is required before a host can attempt registration again after administrative unrevocation.

The signaling E2E suite verifies both active-message and idle-peer revocation paths. In particular, an idle controller and idle host can be revoked without sending any subsequent signaling message and must receive `device-revoked` and a `device revoked` WebSocket close within the sweep interval plus scheduler/network overhead. The suite also verifies that clients cannot forge the event.

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
- `control` DataChannel: **bidirectional**, ordered/reliable. Controller -> host carries clicks/keyboard input that must not be lost, session safety commands, telemetry, video-quality preferences, monitor-switch requests and explicit clipboard operations. Host -> controller carries monitor state/switch results and explicit clipboard results/text.
- `pointer` DataChannel: controller -> host; unordered with `maxRetransmits=0`. Used for pointer movement and wheel events where stale input should be discarded rather than retransmitted.
- `file-transfer` DataChannel: ordered/reliable and isolated from input/control traffic. File-transfer v0 currently implements controller/browser -> Windows host uploads with chunk validation, flow control, cancellation and same-session network-reconnect resume.
- Telemetry: controller reports decoder/network observations over the reliable control channel for adaptive bitrate/FPS/resolution decisions.

ICE policy normally prefers direct candidates and uses TURN as fallback. A relay-only policy exists for validation/restrictive-network testing. Application signaling never carries file payload bytes; however, when ICE selects a TURN relay, encrypted WebRTC file traffic traverses that relay just like other relayed WebRTC traffic.

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

Reliable `control` channel, controller -> host:

```json
{"t":"pointer","kind":"down","button":0,"x":0.41,"y":0.63,"buttons":1}
{"t":"pointer","kind":"up","button":0,"x":0.41,"y":0.63,"buttons":0}
{"t":"key","kind":"down","code":"KeyA","key":"a"}
{"t":"key","kind":"up","code":"KeyA","key":"a"}
{"t":"video-profile","mode":"auto"}
{"t":"monitor-list-request"}
{"t":"monitor-switch","index":1}
{"t":"clipboard-read-request","requestId":"uuid"}
{"t":"clipboard-write","requestId":"uuid","text":"hello"}
{"t":"release-all"}
```

### Video quality profile

`video-profile.mode` is one of `auto`, `original`, `high`, or `clear`. Unknown values are ignored by the host. The current Windows implementation treats the four profiles as user-selected ceilings/bounds around the same congestion controller rather than bypassing network protection:

- `auto`: configured maximum (1080p/60/12 Mbps by default) with bitrate -> FPS -> resolution adaptation down through the existing quality tiers.
- `original`: retains the configured maximum spatial resolution; bitrate and FPS may still be reduced when network conditions require protection.
- `high`: begins at up to the 1600x900 tier, 45 fps and 8 Mbps, while severe congestion may still reduce it to lower resolution/FPS/bitrate tiers.
- `clear`: begins at up to the 1280x720 tier, 30 fps and 4 Mbps, while severe congestion may still reduce it to the 960x540 tier and lower transport targets.

The profile is session-local. The browser sends the currently selected profile when the reliable control channel opens, profile changes take effect immediately, and disconnect returns the Windows host to `auto`. Quality changes request a fresh keyframe. These values are product defaults, not a wire-protocol promise; compatible hosts may choose different numerical ceilings while preserving the four profile semantics.

### Runtime monitor switching

When the reliable control channel opens, the host may immediately publish its monitor state; the controller can also explicitly request it with `monitor-list-request`.

Host -> controller monitor state:

```json
{
  "t": "monitor-state",
  "activeIndex": 1,
  "monitors": [
    {"index":0,"name":"\\\\.\\DISPLAY1","left":0,"top":0,"width":1920,"height":1080,"primary":true},
    {"index":1,"name":"\\\\.\\DISPLAY2","left":1920,"top":0,"width":2560,"height":1440,"primary":false}
  ]
}
```

A controller requests a switch with `monitor-switch`. The host releases remotely injected keyboard/mouse state before accepting the request. The Windows implementation performs the actual DXGI duplication switch on the capture/main thread, updates the controlled desktop rectangle used by normalized pointer mapping, rebuilds the GPU color-conversion/H.264 pipeline for the new source size, requests a fresh keyframe and returns a result:

```json
{"t":"monitor-switch-result","index":1,"activeIndex":1,"ok":true}
```

Failure retains/restores the previous monitor whenever possible and includes a short machine-readable `error`, for example `monitor-unavailable` or `video-pipeline-rebuild-failed`. A fresh `monitor-state` follows a switch result so controllers should use that state as the source of truth rather than optimistically changing the active monitor before host confirmation.

The current Windows capture implementation enumerates and switches outputs attached to the **current DXGI adapter**. This covers normal single-GPU multi-monitor systems. Cross-GPU monitor switching and an all-monitors/split-screen composite stream are separate future capabilities and must not be inferred from `monitor-state` v0.

### Explicit bidirectional text clipboard

Clipboard transport is intentionally **text-only, explicit and session-local**. It never travels through application signaling, is not persisted by DeskLink, and does not continuously monitor either endpoint clipboard. On a direct ICE path it is peer-to-peer; on a TURN-selected path it remains WebRTC encrypted but traverses the relay.

Controller -> host:

```json
{"t":"clipboard-write","requestId":"uuid","text":"copy this to Windows"}
{"t":"clipboard-read-request","requestId":"uuid"}
```

Host -> controller:

```json
{"t":"clipboard-result","requestId":"uuid","direction":"local-to-remote","ok":true}
{"t":"clipboard-text","requestId":"uuid","text":"text read from Windows"}
```

Failures use `clipboard-result` with `ok:false` and a short `error`. The current Windows/browser implementation caps one UTF-8 clipboard payload at **128 KiB** and uses Windows `CF_UNICODETEXT` on the host. Large content, images, directories and files are not clipboard-v0 payloads; files use the dedicated `file-transfer` channel.

The browser UI only invokes local Clipboard APIs from explicit user actions. If browser permissions block `navigator.clipboard.readText()` or `writeText()`, the UI falls back to a visible text area so the user can manually paste/copy. Receiving remote text does not silently overwrite the controller's local clipboard.

## File transfer v0

File transfer uses a dedicated ordered/reliable WebRTC DataChannel named `file-transfer`. It is intentionally separated from the low-latency pointer channel and the small control-message channel, so large buffered file payloads do not become application-level input/control messages.

The current v0 direction is **controller/browser -> Windows host**. Host -> browser downloads and a remote file browser are separate capabilities and must not be inferred from this upload protocol.

A controller begins or resumes one file with a compact text message:

```json
{"t":"upload-begin","id":"4a8f...stable-transfer-id","name":"report.zip","size":734003200}
```

The host validates the transfer ID, sanitizes the filename, opens/creates its hidden partial file and replies with the byte offset it already has durably/previously accepted for this transfer ID:

```json
{"t":"upload-ready","id":"4a8f...stable-transfer-id","offset":104857600,"size":734003200}
```

The controller must treat the host `offset` as the source of truth. Buffered chunks that were counted as sent by the browser but never accepted by the host may therefore be retransmitted after a reconnect without corrupting the file.

Each binary DataChannel message is one independently verified chunk:

```text
bytes 0..7    unsigned 64-bit little-endian file offset
bytes 8..39   SHA-256(payload), exactly 32 raw digest bytes
bytes 40..N   payload bytes
```

The current browser/Windows implementation caps one payload chunk at **32 KiB**. The host accepts a chunk only when its offset exactly equals the next expected byte, the payload does not exceed the declared file size and the SHA-256 digest matches. A mismatched offset causes the host to re-advertise its current `upload-ready` offset; invalid hashes/overflow/write failures produce an error instead of silently advancing the partial file.

Progress and completion messages are host -> controller:

```json
{"t":"upload-progress","id":"...","received":105906176,"size":734003200}
{"t":"upload-complete","id":"...","name":"report (1).zip","size":734003200}
{"t":"upload-error","id":"...","error":"chunk-hash-mismatch"}
```

Cancellation is controller -> host:

```json
{"t":"upload-cancel","id":"..."}
```

On success the host replies:

```json
{"t":"upload-cancelled","id":"..."}
```

The current Windows receiver permits one active upload per `file-transfer` channel. The browser queues multiple selected/dropped files and sends them sequentially. The browser applies DataChannel backpressure with `bufferedAmount`: the current implementation stops adding chunks above a 2 MiB high-water mark and resumes after buffered data falls to the 512 KiB low-water mark.

Verified partial files are retained when the DataChannel/PeerConnection is interrupted. While the same browser page still holds the original `File` object, a replacement PeerConnection opens a new `file-transfer` channel, resends `upload-begin` with the same transfer ID and resumes from the host-confirmed offset. A failed job can likewise retry with the same transfer ID. Full browser page reload is **not** resumable in v0 because the browser cannot silently reacquire the user's original local `File` permission/object; persistent browser file handles require a separate user-consented design.

The current Windows product policy defaults the receive root to the interactive user's `Downloads\\DeskLink` and supports an administrator override via `DESKLINK_TRANSFER_DIR`. The controller does not provide an arbitrary remote filesystem path. Windows sanitizes unsafe/reserved filenames and finalizes to a non-overwriting unique destination name. The current Windows implementation caps a file at **20 GiB**; this is a product safety/resource limit, not a protocol-wide promise that other hosts must use the same maximum.

File payload bytes are never wrapped in DeskLink signaling messages. With direct ICE they travel directly between WebRTC peers; with TURN they are still encrypted WebRTC DataChannel traffic but traverse the selected relay.

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
9. Monitor switches are transactional: if the new capture/video pipeline cannot be established, retain or restore the previous monitor rather than reporting a false successful switch.
10. Clipboard v0 remains bounded text control traffic. Large/resumable file payloads use the separately flow-controlled `file-transfer` DataChannel with bounded chunk size and browser-side buffered-amount backpressure.
