# DeskLink protocol v0

DeskLink separates **signaling**, **media**, and **control** paths.

## Signaling

WebSocket endpoint: `/ws?deviceId=<id>`

Generic envelope sent to signaling:

```json
{
  "type": "offer|answer|ice|ping",
  "target": "remote-device-id",
  "session": "uuid",
  "payload": {}
}
```

The signaling service forwards the type/session/payload and adds `from` with the sender device ID.

### Remote-control authorization

The first controller offer includes the user-entered device access code:

```json
{
  "type": "offer",
  "target": "office-pc",
  "session": "uuid",
  "payload": {
    "type": "offer",
    "sdp": "v=0...",
    "accessCode": "user-entered-secret"
  }
}
```

The Windows host compares the supplied code with `DESKLINK_ACCESS_CODE` before allocating a PeerConnection. Until that succeeds, ICE from the controller is ignored.

A rejected offer is answered through signaling only:

```json
{
  "type": "auth-rejected",
  "payload": {"reason": "invalid-access-code"}
}
```

or, when the host has not been configured with an access code:

```json
{
  "type": "auth-rejected",
  "payload": {"reason": "host-unconfigured"}
}
```

The development access-code exchange assumes a trusted LAN or **WSS**. Production authentication should replace the plaintext signaling-field proof with a challenge/response or account/device token flow, add attempt throttling, and use WSS exclusively.

## WebRTC topology

- Video: host -> controller, H.264 in M1.
- Audio: optional, later milestone.
- `control` DataChannel: controller -> host; ordered/reliable. Used for clicks and keyboard input that must not be lost.
- `pointer` DataChannel: controller -> host; unordered with `maxRetransmits=0`. Used for pointer-move and wheel events where stale input should be discarded rather than retransmitted.
- Future `telemetry` path: bidirectional network/decoder stats for adaptive bitrate/resolution.

ICE policy is `all`: direct candidates are tried first, TURN is available as fallback.

## Media

The M1 Windows host pipeline is:

```text
DXGI Desktop Duplication (BGRA D3D11 texture)
  -> D3D11 Video Processor (GPU scale + NV12)
  -> Media Foundation hardware H.264 encoder
  -> Annex-B access unit
  -> libdatachannel H.264 RTP packetizer
  -> WebRTC video track
  -> browser hardware/software H.264 decoder
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
```

Unreliable `pointer` channel:

```json
{"t":"pointer","kind":"move","x":0.41,"y":0.63,"button":0,"buttons":0}
{"t":"wheel","delta":-120}
```

Browser pointer-move events are coalesced to at most the display animation cadence with `requestAnimationFrame`; only the newest pending position is sent.

## Performance rules

1. Interaction latency wins over image quality.
2. Do not queue stale frames or stale pointer moves.
3. Target capture-to-display latency: < 100 ms on a healthy LAN; < 180 ms on normal WAN.
4. Start at up to 1080p/60 and 12 Mbps by default when hardware encoding and bandwidth allow it.
5. On congestion, reduce bitrate first, then FPS/resolution; recover gradually.
6. Default keyframe/GOP target is one second, plus an immediate keyframe after connection/reconnect or when the video track was not ready for a frame.
7. Do not run the encoder while no authenticated controller is connected.
