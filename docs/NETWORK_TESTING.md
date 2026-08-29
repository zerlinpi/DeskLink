# DeskLink network and latency validation

DeskLink should be tuned with repeatable measurements rather than only subjective visual checks. The browser diagnostics HUD is the primary live view for route, RTT, loss, jitter, decode FPS and estimated available incoming bitrate.

## Acceptance priorities

Remote-control quality is evaluated in this order:

1. Pointer and keyboard input remain responsive.
2. No unbounded video queue or multi-second stale-frame playback.
3. A lost H.264 reference frame recovers quickly through NACK/PLI + IDR.
4. Video quality degrades before interaction latency becomes unacceptable.
5. Quality recovers slowly after the network becomes healthy to avoid oscillation.
6. Temporary signaling loss does not tear down an otherwise healthy P2P session.
7. A real network-path failure can recover through signaling reconnect + ICE restart.

## Baseline targets

These are engineering targets, not guarantees for every device/network.

| Scenario | Route | RTT target | Decode target | Expected behavior |
| --- | --- | ---: | ---: | --- |
| Same LAN | Direct P2P | < 30 ms | ~60 FPS when desktop changes | 1080p/high profile, no visible input lag |
| Good regional WAN | Direct P2P | < 100 ms | 45-60 FPS | High bitrate, little/no adaptive degradation |
| TURN/UDP regional | Relay | < 150 ms | 30-60 FPS | Slightly lower quality acceptable; control stays responsive |
| Constrained WAN | Direct/Relay | 150-250 ms | 24-45 FPS | Bitrate/FPS should fall before frame backlog grows |
| Severe congestion | Direct/Relay | > 250 ms or > 8% loss | 24 FPS or lower effective decode | Resolution may step down to 900p/720p/540p; input still prioritized |

## Test matrix

Run the following in order and record the HUD values plus the Agent console statistics.

### A. LAN Direct P2P

- Host and controller on the same LAN.
- Confirm HUD says `Direct P2P`.
- Move windows rapidly, scroll a long page, drag a window continuously and type quickly.
- Verify pointer movement does not become sticky while the desktop is changing heavily.
- Leave the desktop completely static for 10 seconds, reconnect, and confirm the first decodable frame appears without waiting for new desktop motion.

### B. WAN Direct P2P

- Put host and controller on different networks.
- Confirm `Direct P2P` when NAT traversal succeeds.
- Record RTT, loss, jitter, decode FPS and available bitrate.
- Repeat heavy window movement/scrolling and compare against LAN.

### C. Forced TURN/UDP

Force or isolate a test where Direct P2P cannot be established and confirm the HUD reports `TURN relay` using UDP.

Verify:

- Session connects reliably behind CGNAT/restrictive NAT.
- Input remains responsive.
- RTP pacing prevents large H.264 frames from appearing as extreme burst loss.
- Adaptive quality does not immediately overreact to a healthy relay path.

### D. TURN/TCP compatibility path

Test separately from TURN/UDP. TCP relay is a compatibility fallback, not the preferred performance path.

Watch for:

- Increased jitter/RTT compared with UDP.
- Head-of-line effects during packet loss.
- Whether quality reduction keeps control usable even when video becomes less smooth.

### E. Controlled weak network

Apply impairment at an endpoint interface for Direct P2P tests, or at the relay path for TURN tests.

Suggested profiles:

| Profile | Added one-way delay | Loss | Bandwidth | Expected adaptation |
| --- | ---: | ---: | ---: | --- |
| Mild | 25 ms | 1% | 12 Mbps | Mostly bitrate-only response |
| Moderate | 60 ms | 3% | 8 Mbps | Bitrate reduction; may reach 45/30 FPS |
| Poor | 100 ms | 5% | 5 Mbps | 30/24 FPS and possible 900p/720p |
| Severe | 150 ms | 8% | 3 Mbps | 24 FPS, 720p/540p, rapid bitrate drop |
| Recovery | Remove impairment | 0% | unrestricted | Slow stepwise recovery rather than instant oscillation |

On a Linux test gateway/interface, `tc netem` can provide repeatable impairment. Example only — replace `eth0` with the actual test interface and avoid running this on a production server:

```bash
sudo tc qdisc add dev eth0 root handle 1: netem delay 60ms 10ms loss 3%
sudo tc qdisc add dev eth0 parent 1:1 handle 10: tbf rate 8mbit burst 64kb latency 100ms
```

Remove the test rule after the run:

```bash
sudo tc qdisc del dev eth0 root
```

If the test uses Direct P2P, shaping only the signaling server does not meaningfully impair the media path after connection. Shape an endpoint/gateway that actually carries the WebRTC packets.

## Network-switch recovery

Test at least these path changes while a session is active:

- Wi-Fi disconnect/reconnect.
- Switch controller from Wi-Fi to another network where practical.
- Restart the signaling reverse proxy/service while P2P remains healthy.
- Temporarily block the selected UDP path, then restore it.

Expected behavior:

- Signaling-only outage: existing P2P media/control continues if the peer path is healthy.
- Signaling WebSocket reconnects automatically.
- `disconnected` waits briefly for natural ICE recovery before restarting ICE.
- `failed` attempts ICE restart immediately.
- Same controller/session renegotiates the existing authorized PeerConnection rather than creating a new control session.
- Stuck Ctrl/Alt/Shift/mouse buttons are released when control actually closes.

## Video recovery tests

### PLI/IDR recovery

Under packet loss, watch for a temporary decode artifact/freeze and verify the stream recovers quickly instead of remaining broken until a long GOP boundary.

### Display change

While connected:

- Change host display resolution.
- Reorder monitors.
- Test a secondary monitor positioned left of the primary monitor (negative virtual-desktop X coordinates).

Expected behavior:

- DXGI duplication recovers.
- GPU conversion + H.264 pipeline rebuilds when dimensions change.
- A fresh IDR is sent after rebuild.
- Pointer coordinates remain aligned with the selected monitor.

## CPU/GPU load test

On the Windows host, create realistic background CPU/GPU load while remote controlling the machine.

Verify:

- Capture/encode cadence remains stable enough for interaction.
- `DESKLINK_PERFORMANCE_TUNING=1` is the normal test mode.
- If a driver/system shows unusual behavior, compare with `DESKLINK_PERFORMANCE_TUNING=0` and record the difference.

Do not use `REALTIME_PRIORITY_CLASS` as a tuning shortcut; remote desktop must not starve critical Windows threads.

## What to record for every run

Record at minimum:

- Host GPU and driver version.
- Host display resolution/refresh rate.
- Browser and browser version.
- Direct P2P vs TURN relay.
- UDP/TCP relay protocol when known.
- RTT, loss, jitter, decode FPS and available bitrate from the HUD.
- Agent console capture FPS, encode FPS, media Mbps, target bitrate, FPS target, resolution and tier.
- Whether an ICE restart or signaling reconnect occurred.
- Whether any manual reconnect was required.

A performance change should not be considered an improvement unless it either reduces interaction latency/stalls or preserves the same responsiveness with lower bandwidth/resource usage across the matrix above.
