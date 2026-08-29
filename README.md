# DeskLink

DeskLink is a low-latency remote desktop project targeting a Sunlogin / NetEase UU Remote-like experience with a browser-first controller and native host agents.

## Architecture direction

- **Controller:** Web (React + TypeScript) first, so macOS/Windows/mobile browsers can control a remote device without installing a controller.
- **Windows host agent:** Native C++20 for DXGI Desktop Duplication, Media Foundation hardware H.264 encode, and Win32 input injection.
- **Realtime transport:** WebRTC (ICE/STUN/TURN, DTLS-SRTP, data channels) with P2P preferred and TURN relay fallback.
- **Signaling:** Go WebSocket/HTTP service. It only coordinates sessions; media should not flow through signaling.
- **Relay:** coturn.
- **Future native controllers:** Tauri/Swift/Kotlin can reuse the protocol and signaling layer.

> “No stutter” cannot be guaranteed on every network. DeskLink is structured to minimize capture/encode/network/decode/input latency and to degrade quality before interaction responsiveness.

## Repository layout

```text
apps/
  signal/       Go signaling service
  web/          Browser controller
  windows-agent/ Native Windows host skeleton
infra/
  coturn/       TURN configuration
  docker-compose.yml
packages/
  protocol/     Signaling and control protocol docs/types
```

## Milestones

1. **M0:** session signaling, browser UI, WebRTC negotiation, input channel protocol, TURN fallback infrastructure.
2. **M1:** Windows DXGI capture + hardware H.264 + browser playback + mouse/keyboard input.
3. **M2:** adaptive bitrate/FPS/resolution, reconnect, multi-monitor, clipboard/file transfer.
4. **M3:** macOS host, Android/iOS controllers, unattended service/agent architecture and hardened auth.

See `docs/ARCHITECTURE.md` after the initial scaffold lands.
