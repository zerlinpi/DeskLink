# DeskLink protocol v0

DeskLink separates **signaling**, **media**, and **control** paths.

## Signaling

WebSocket endpoint: `/ws?deviceId=<id>`

Envelope sent to signaling:

```json
{
  "type": "offer|answer|ice|session-request|session-accept|ping",
  "target": "remote-device-id",
  "session": "uuid",
  "payload": {}
}
```

The signaling service forwards the same type and payload with `from` populated.

## WebRTC topology

- Video: host -> controller, H.264 preferred.
- Audio: optional, later milestone.
- `control` data channel: controller -> host; ordered, low-latency messages.
- `telemetry` data channel: bidirectional stats; unordered where supported.

ICE policy is `all`: direct candidates are tried first, TURN is available as fallback.

## Control messages

Coordinates are normalized to `[0, 1]` so controller and host resolutions can differ.

```json
{"t":"pointer","kind":"move","x":0.41,"y":0.63,"buttons":0}
{"t":"pointer","kind":"down","button":0,"x":0.41,"y":0.63}
{"t":"pointer","kind":"up","button":0,"x":0.41,"y":0.63}
{"t":"wheel","dx":0,"dy":-120}
{"t":"key","kind":"down","code":"KeyA","key":"a","mods":[]}
{"t":"key","kind":"up","code":"KeyA","key":"a","mods":[]}
```

High-frequency pointer-move events should be coalesced to the latest unsent position.

## Performance rules

1. Interaction latency wins over image quality.
2. Do not queue stale frames or stale pointer moves.
3. Target capture-to-display latency: < 100 ms on a healthy LAN; < 180 ms on normal WAN.
4. Start at 1080p/60 when hardware encoding and bandwidth allow it.
5. On congestion, reduce bitrate first, then FPS/resolution; recover gradually.
6. Keyframe interval target: 1-2 seconds, plus immediate keyframe after decoder loss/reconnect.
