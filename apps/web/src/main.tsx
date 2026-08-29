import React, { useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import "./styles.css";

type SignalMessage = {
  type: string;
  from?: string;
  target?: string;
  session?: string;
  payload?: any;
};

type PointerPayload = {
  t: "pointer";
  kind: "move" | "down" | "up";
  x: number;
  y: number;
  button: number;
  buttons: number;
};

type StatsBaseline = {
  atMs: number;
  packetsReceived: number;
  packetsLost: number;
  framesDecoded: number;
};

const SIGNAL_URL = import.meta.env.VITE_SIGNAL_URL ?? "ws://localhost:8080/ws";
const STUN_URL = import.meta.env.VITE_STUN_URL ?? "stun:stun.l.google.com:19302";
const TURN_URL = import.meta.env.VITE_TURN_URL ?? "turn:localhost:3478";
const TURN_USERNAME = import.meta.env.VITE_TURN_USERNAME ?? "desklink";
const TURN_PASSWORD = import.meta.env.VITE_TURN_PASSWORD ?? "CHANGE_ME_NOW";

const ICE_SERVERS: RTCIceServer[] = [
  { urls: STUN_URL },
  {
    urls: [`${TURN_URL}?transport=udp`, `${TURN_URL}?transport=tcp`],
    username: TURN_USERNAME,
    credential: TURN_PASSWORD,
  },
];

function App() {
  const localId = useMemo(() => `web-${crypto.randomUUID().slice(0, 8)}`, []);
  const [targetId, setTargetId] = useState("");
  const [accessCode, setAccessCode] = useState("");
  const [status, setStatus] = useState("idle");
  const videoRef = useRef<HTMLVideoElement>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const pcRef = useRef<RTCPeerConnection | null>(null);
  const controlRef = useRef<RTCDataChannel | null>(null);
  const pointerRef = useRef<RTCDataChannel | null>(null);
  const sessionRef = useRef(crypto.randomUUID());
  const pendingMoveRef = useRef<PointerPayload | null>(null);
  const pointerRafRef = useRef<number | null>(null);
  const statsTimerRef = useRef<number | null>(null);
  const statsBaselineRef = useRef<StatsBaseline | null>(null);
  const offerSentRef = useRef(false);
  const negotiationPendingRef = useRef(false);
  const pendingLocalIceRef = useRef<RTCIceCandidateInit[]>([]);
  const pendingRemoteIceRef = useRef<RTCIceCandidateInit[]>([]);
  const iceRestartTimerRef = useRef<number | null>(null);
  const iceRestartWatchdogRef = useRef<number | null>(null);
  const iceRestartInFlightRef = useRef(false);
  const signalReconnectTimerRef = useRef<number | null>(null);
  const signalReconnectAttemptRef = useRef(0);
  const manualDisconnectRef = useRef(false);

  const sendSignal = (type: string, payload: any = {}) => {
    const ws = wsRef.current;
    if (ws?.readyState !== WebSocket.OPEN) return false;
    ws.send(JSON.stringify({ type, target: targetId, session: sessionRef.current, payload }));
    return true;
  };

  const sendReliable = (payload: object) => {
    const channel = controlRef.current;
    if (channel?.readyState === "open") channel.send(JSON.stringify(payload));
  };

  const sendPointerFast = (payload: object) => {
    const channel = pointerRef.current;
    if (channel?.readyState === "open") channel.send(JSON.stringify(payload));
  };

  const clearIceRestartTimer = () => {
    if (iceRestartTimerRef.current !== null) window.clearTimeout(iceRestartTimerRef.current);
    iceRestartTimerRef.current = null;
  };

  const clearIceRestartWatchdog = () => {
    if (iceRestartWatchdogRef.current !== null) window.clearTimeout(iceRestartWatchdogRef.current);
    iceRestartWatchdogRef.current = null;
  };

  const clearSignalReconnectTimer = () => {
    if (signalReconnectTimerRef.current !== null) window.clearTimeout(signalReconnectTimerRef.current);
    signalReconnectTimerRef.current = null;
  };

  useEffect(() => {
    const releaseAll = () => sendReliable({ t: "release-all" });
    const onVisibilityChange = () => {
      if (document.visibilityState === "hidden") releaseAll();
    };

    window.addEventListener("blur", releaseAll);
    document.addEventListener("visibilitychange", onVisibilityChange);
    return () => {
      window.removeEventListener("blur", releaseAll);
      document.removeEventListener("visibilitychange", onVisibilityChange);
      clearIceRestartTimer();
      clearIceRestartWatchdog();
      clearSignalReconnectTimer();
    };
  }, []);

  const stopTelemetry = () => {
    if (statsTimerRef.current !== null) window.clearInterval(statsTimerRef.current);
    statsTimerRef.current = null;
    statsBaselineRef.current = null;
  };

  const collectTelemetry = async (pc: RTCPeerConnection) => {
    if (pc.connectionState !== "connected" || controlRef.current?.readyState !== "open") return;

    try {
      const report = await pc.getStats();
      let inboundVideo: any = null;
      let selectedPair: any = null;
      let selectedPairId = "";

      report.forEach((raw) => {
        const stat = raw as any;
        if (stat.type === "transport" && stat.selectedCandidatePairId) {
          selectedPairId = stat.selectedCandidatePairId;
        }
        if (stat.type === "inbound-rtp" && stat.kind === "video" && !stat.isRemote) {
          inboundVideo = stat;
        }
      });

      report.forEach((raw) => {
        const stat = raw as any;
        if (stat.type !== "candidate-pair" || stat.state !== "succeeded") return;
        if (selectedPairId ? stat.id === selectedPairId : stat.nominated) selectedPair = stat;
      });

      if (!inboundVideo) return;

      const nowMs = performance.now();
      const current: StatsBaseline = {
        atMs: nowMs,
        packetsReceived: Number(inboundVideo.packetsReceived ?? 0),
        packetsLost: Number(inboundVideo.packetsLost ?? 0),
        framesDecoded: Number(inboundVideo.framesDecoded ?? 0),
      };
      const previous = statsBaselineRef.current;
      statsBaselineRef.current = current;
      if (!previous) return;

      const elapsedSeconds = Math.max(0.25, (current.atMs - previous.atMs) / 1000);
      const receivedDelta = Math.max(0, current.packetsReceived - previous.packetsReceived);
      const lostDelta = Math.max(0, current.packetsLost - previous.packetsLost);
      const packetDelta = receivedDelta + lostDelta;
      const lossPct = packetDelta > 0 ? (lostDelta / packetDelta) * 100 : 0;
      const decodeFps = Math.max(0, current.framesDecoded - previous.framesDecoded) / elapsedSeconds;
      const rttMs = selectedPair?.currentRoundTripTime != null
        ? Number(selectedPair.currentRoundTripTime) * 1000
        : null;
      const jitterMs = inboundVideo.jitter != null ? Number(inboundVideo.jitter) * 1000 : null;
      const availableIncomingBitrate = selectedPair?.availableIncomingBitrate != null
        ? Number(selectedPair.availableIncomingBitrate)
        : null;

      sendReliable({
        t: "telemetry",
        rttMs,
        lossPct,
        decodeFps,
        jitterMs,
        framesDropped: Number(inboundVideo.framesDropped ?? 0),
        availableIncomingBitrate,
      });
    } catch (error) {
      console.debug("DeskLink telemetry collection failed", error);
    }
  };

  const startTelemetry = (pc: RTCPeerConnection) => {
    stopTelemetry();
    statsTimerRef.current = window.setInterval(() => {
      void collectTelemetry(pc);
    }, 1000);
  };

  const pointFromEvent = (
    e: React.PointerEvent<HTMLVideoElement>,
    kind: PointerPayload["kind"],
  ): PointerPayload => {
    const video = e.currentTarget;
    const r = video.getBoundingClientRect();

    let contentLeft = r.left;
    let contentTop = r.top;
    let contentWidth = r.width;
    let contentHeight = r.height;

    if (video.videoWidth > 0 && video.videoHeight > 0 && r.width > 0 && r.height > 0) {
      const scale = Math.min(r.width / video.videoWidth, r.height / video.videoHeight);
      contentWidth = video.videoWidth * scale;
      contentHeight = video.videoHeight * scale;
      contentLeft = r.left + (r.width - contentWidth) / 2;
      contentTop = r.top + (r.height - contentHeight) / 2;
    }

    return {
      t: "pointer",
      kind,
      x: Math.max(0, Math.min(1, (e.clientX - contentLeft) / Math.max(1, contentWidth))),
      y: Math.max(0, Math.min(1, (e.clientY - contentTop) / Math.max(1, contentHeight))),
      button: e.button,
      buttons: e.buttons,
    };
  };

  const queuePointerMove = (payload: PointerPayload) => {
    pendingMoveRef.current = payload;
    if (pointerRafRef.current !== null) return;
    pointerRafRef.current = requestAnimationFrame(() => {
      pointerRafRef.current = null;
      const latest = pendingMoveRef.current;
      pendingMoveRef.current = null;
      if (latest) sendPointerFast(latest);
    });
  };

  const sendOffer = async (pc: RTCPeerConnection, iceRestart = false) => {
    if (pcRef.current !== pc) return false;
    if (wsRef.current?.readyState !== WebSocket.OPEN) return false;

    offerSentRef.current = false;
    negotiationPendingRef.current = true;
    pendingLocalIceRef.current = [];
    if (iceRestart) pendingRemoteIceRef.current = [];

    const offer = await pc.createOffer(iceRestart ? { iceRestart: true } : undefined);
    if (pcRef.current !== pc) return false;
    await pc.setLocalDescription(offer);
    if (pcRef.current !== pc || wsRef.current?.readyState !== WebSocket.OPEN) {
      negotiationPendingRef.current = false;
      return false;
    }

    if (!sendSignal("offer", {
      type: offer.type,
      sdp: offer.sdp,
      accessCode,
      iceRestart,
    })) {
      negotiationPendingRef.current = false;
      return false;
    }

    offerSentRef.current = true;
    for (const candidate of pendingLocalIceRef.current) {
      sendSignal("ice", candidate);
    }
    pendingLocalIceRef.current = [];
    return true;
  };

  const scheduleIceRestart = (pc: RTCPeerConnection, immediate = false) => {
    if (manualDisconnectRef.current || pcRef.current !== pc) return;
    if (iceRestartInFlightRef.current || iceRestartTimerRef.current !== null) return;
    if (pc.connectionState === "connected" || pc.connectionState === "closed") return;

    iceRestartTimerRef.current = window.setTimeout(() => {
      iceRestartTimerRef.current = null;
      if (manualDisconnectRef.current || pcRef.current !== pc) return;
      if (pc.connectionState === "connected" || pc.connectionState === "closed") return;
      void restartIce(pc);
    }, immediate ? 0 : 2000);
  };

  const restartIce = async (pc: RTCPeerConnection) => {
    if (manualDisconnectRef.current || pcRef.current !== pc) return;
    if (iceRestartInFlightRef.current || pc.connectionState === "closed") return;

    if (wsRef.current?.readyState !== WebSocket.OPEN) {
      scheduleSignalReconnect();
      return;
    }

    iceRestartInFlightRef.current = true;
    clearIceRestartTimer();
    setStatus("reconnecting network");

    try {
      const sent = await sendOffer(pc, true);
      if (!sent) {
        iceRestartInFlightRef.current = false;
        scheduleSignalReconnect();
        return;
      }

      clearIceRestartWatchdog();
      iceRestartWatchdogRef.current = window.setTimeout(() => {
        iceRestartWatchdogRef.current = null;
        if (pcRef.current !== pc || pc.connectionState === "connected") {
          iceRestartInFlightRef.current = false;
          return;
        }
        iceRestartInFlightRef.current = false;
        scheduleIceRestart(pc, false);
      }, 8000);
    } catch (error) {
      console.debug("DeskLink ICE restart failed", error);
      iceRestartInFlightRef.current = false;
      scheduleIceRestart(pc, false);
    }
  };

  const createPeer = () => {
    offerSentRef.current = false;
    negotiationPendingRef.current = false;
    pendingLocalIceRef.current = [];
    pendingRemoteIceRef.current = [];

    const pc = new RTCPeerConnection({
      iceServers: ICE_SERVERS,
      bundlePolicy: "max-bundle",
    });
    pcRef.current = pc;

    pc.ontrack = (event) => {
      if (!videoRef.current) return;
      videoRef.current.srcObject = event.streams[0] ?? new MediaStream([event.track]);
      void videoRef.current.play().catch(() => undefined);
    };
    pc.onicecandidate = (event) => {
      if (!event.candidate) return;
      const candidate = event.candidate.toJSON();
      if (offerSentRef.current) {
        sendSignal("ice", candidate);
      } else {
        pendingLocalIceRef.current.push(candidate);
      }
    };
    pc.onconnectionstatechange = () => {
      const state = pc.connectionState;
      if (state === "connected") {
        clearIceRestartTimer();
        clearIceRestartWatchdog();
        iceRestartInFlightRef.current = false;
        setStatus(wsRef.current?.readyState === WebSocket.OPEN
          ? "control ready"
          : "control ready · signaling reconnecting");
        if (controlRef.current?.readyState === "open") startTelemetry(pc);
      } else if (state === "disconnected") {
        setStatus("reconnecting network");
        scheduleIceRestart(pc, false);
      } else if (state === "failed") {
        stopTelemetry();
        setStatus("reconnecting network");
        scheduleIceRestart(pc, true);
      } else if (state === "closed") {
        stopTelemetry();
        clearIceRestartTimer();
        clearIceRestartWatchdog();
        iceRestartInFlightRef.current = false;
      } else {
        setStatus(state);
      }
    };

    const control = pc.createDataChannel("control", { ordered: true });
    controlRef.current = control;
    control.onopen = () => {
      setStatus(wsRef.current?.readyState === WebSocket.OPEN
        ? "control ready"
        : "control ready · signaling reconnecting");
      startTelemetry(pc);
    };
    control.onclose = stopTelemetry;

    const pointer = pc.createDataChannel("pointer", {
      ordered: false,
      maxRetransmits: 0,
    });
    pointerRef.current = pointer;

    return pc;
  };

  const handleSignalMessage = async (event: MessageEvent) => {
    let msg: SignalMessage;
    try {
      msg = JSON.parse(event.data);
    } catch {
      return;
    }

    const pc = pcRef.current;
    if (!pc) return;

    if (msg.type === "answer" && msg.payload) {
      try {
        await pc.setRemoteDescription(msg.payload as RTCSessionDescriptionInit);
        negotiationPendingRef.current = false;
        iceRestartInFlightRef.current = false;
        clearIceRestartWatchdog();

        const pending = pendingRemoteIceRef.current;
        pendingRemoteIceRef.current = [];
        for (const candidate of pending) {
          try {
            await pc.addIceCandidate(candidate);
          } catch (error) {
            console.debug("DeskLink queued remote ICE rejected", error);
          }
        }
      } catch (error) {
        console.debug("DeskLink remote answer rejected", error);
        negotiationPendingRef.current = false;
        iceRestartInFlightRef.current = false;
        scheduleIceRestart(pc, false);
      }
    } else if (msg.type === "ice" && msg.payload) {
      const candidate = msg.payload as RTCIceCandidateInit;
      if (pc.remoteDescription && !negotiationPendingRef.current) {
        try {
          await pc.addIceCandidate(candidate);
        } catch (error) {
          console.debug("DeskLink remote ICE rejected", error);
        }
      } else {
        pendingRemoteIceRef.current.push(candidate);
      }
    } else if (msg.type === "peer-offline") {
      if (pc.connectionState === "connected") {
        setStatus("control ready · host signaling offline");
      } else {
        setStatus("host offline · retrying");
        scheduleIceRestart(pc, false);
      }
    } else if (msg.type === "auth-rejected") {
      const reason = msg.payload?.reason;
      disconnect(reason === "host-unconfigured" ? "host access code not configured" : "access code rejected");
    }
  };

  const scheduleSignalReconnect = () => {
    if (manualDisconnectRef.current || signalReconnectTimerRef.current !== null) return;
    const current = wsRef.current;
    if (current?.readyState === WebSocket.OPEN || current?.readyState === WebSocket.CONNECTING) return;

    const attempt = signalReconnectAttemptRef.current++;
    const delay = Math.min(10000, 500 * (2 ** Math.min(attempt, 5)));
    signalReconnectTimerRef.current = window.setTimeout(() => {
      signalReconnectTimerRef.current = null;
      if (!manualDisconnectRef.current) openSignalSocket(false);
    }, delay);
  };

  const openSignalSocket = (initial: boolean) => {
    if (manualDisconnectRef.current) return;

    const ws = new WebSocket(`${SIGNAL_URL}?deviceId=${encodeURIComponent(localId)}`);
    wsRef.current = ws;

    ws.onmessage = (event) => {
      void handleSignalMessage(event);
    };

    ws.onopen = async () => {
      if (wsRef.current !== ws || manualDisconnectRef.current) return;
      signalReconnectAttemptRef.current = 0;
      clearSignalReconnectTimer();

      if (initial) {
        const pc = createPeer();
        const transceiver = pc.addTransceiver("video", { direction: "recvonly" });
        const videoCapabilities = RTCRtpReceiver.getCapabilities("video");
        const h264Codecs = videoCapabilities?.codecs.filter(
          (codec) => codec.mimeType.toLowerCase() === "video/h264",
        );
        if (h264Codecs?.length) transceiver.setCodecPreferences(h264Codecs);

        try {
          if (!await sendOffer(pc, false)) {
            setStatus("signal error");
          }
        } catch (error) {
          console.debug("DeskLink initial offer failed", error);
          setStatus("signal error");
        }
        return;
      }

      const pc = pcRef.current;
      if (!pc) return;
      if (pc.connectionState === "connected") {
        setStatus("control ready");
      } else if (pc.connectionState !== "closed") {
        setStatus("reconnecting network");
        scheduleIceRestart(pc, true);
      }
    };

    ws.onerror = () => {
      if (wsRef.current !== ws) return;
      if (pcRef.current?.connectionState !== "connected") setStatus("signal error");
    };

    ws.onclose = () => {
      if (wsRef.current !== ws) return;
      wsRef.current = null;
      if (manualDisconnectRef.current) return;

      const pc = pcRef.current;
      if (pc && pc.connectionState !== "closed") {
        setStatus(pc.connectionState === "connected"
          ? "control ready · signaling reconnecting"
          : "reconnecting signaling");
        scheduleSignalReconnect();
      } else {
        setStatus("disconnected");
      }
    };
  };

  const disconnect = (nextStatus = "idle") => {
    manualDisconnectRef.current = true;
    sendReliable({ t: "release-all" });
    stopTelemetry();
    clearIceRestartTimer();
    clearIceRestartWatchdog();
    clearSignalReconnectTimer();
    iceRestartInFlightRef.current = false;
    negotiationPendingRef.current = false;
    signalReconnectAttemptRef.current = 0;

    if (pointerRafRef.current !== null) cancelAnimationFrame(pointerRafRef.current);
    pointerRafRef.current = null;
    pendingMoveRef.current = null;
    offerSentRef.current = false;
    pendingLocalIceRef.current = [];
    pendingRemoteIceRef.current = [];
    controlRef.current?.close();
    pointerRef.current?.close();
    pcRef.current?.close();
    wsRef.current?.close();
    controlRef.current = null;
    pointerRef.current = null;
    pcRef.current = null;
    wsRef.current = null;
    if (videoRef.current) videoRef.current.srcObject = null;
    sessionRef.current = crypto.randomUUID();
    setStatus(nextStatus);
  };

  const connect = async () => {
    if (!targetId.trim() || !accessCode || status !== "idle") return;
    manualDisconnectRef.current = false;
    signalReconnectAttemptRef.current = 0;
    setStatus("signaling");
    openSignalSocket(true);
  };

  const canConnect = status === "idle" && Boolean(targetId.trim()) && Boolean(accessCode);

  return (
    <main className="shell">
      <header>
        <div>
          <strong>DeskLink</strong>
          <span>Browser controller · {localId}</span>
        </div>
        <span className="status">{status}</span>
      </header>

      <section className="connect-card">
        <input
          value={targetId}
          onChange={(e) => setTargetId(e.target.value)}
          placeholder="Remote device ID"
          autoComplete="off"
          disabled={status !== "idle"}
        />
        <input
          type="password"
          value={accessCode}
          onChange={(e) => setAccessCode(e.target.value)}
          placeholder="Access code"
          autoComplete="current-password"
          disabled={status !== "idle"}
          onKeyDown={(e) => {
            if (e.key === "Enter" && canConnect) void connect();
          }}
        />
        {status === "idle" ? (
          <button onClick={() => void connect()} disabled={!canConnect}>Connect</button>
        ) : (
          <button onClick={() => disconnect()}>Disconnect</button>
        )}
      </section>

      <section className="stage">
        <video
          ref={videoRef}
          autoPlay
          playsInline
          tabIndex={0}
          onContextMenu={(e) => e.preventDefault()}
          onPointerMove={(e) => queuePointerMove(pointFromEvent(e, "move"))}
          onPointerDown={(e) => {
            e.currentTarget.focus();
            e.currentTarget.setPointerCapture(e.pointerId);
            sendReliable(pointFromEvent(e, "down"));
          }}
          onPointerUp={(e) => sendReliable(pointFromEvent(e, "up"))}
          onPointerCancel={() => sendReliable({ t: "release-all" })}
          onWheel={(e) => {
            e.preventDefault();
            const magnitude = Math.max(1, Math.min(5, Math.round(Math.abs(e.deltaY) / 100)));
            sendPointerFast({ t: "wheel", delta: (e.deltaY < 0 ? 120 : -120) * magnitude });
          }}
          onKeyDown={(e) => {
            if (controlRef.current?.readyState === "open") {
              e.preventDefault();
              sendReliable({ t: "key", kind: "down", code: e.code, key: e.key });
            }
          }}
          onKeyUp={(e) => {
            if (controlRef.current?.readyState === "open") {
              e.preventDefault();
              sendReliable({ t: "key", kind: "up", code: e.code, key: e.key });
            }
          }}
        />
        {status === "idle" && <div className="empty">Enter the device ID and access code to start a low-latency session.</div>}
      </section>
    </main>
  );
}

createRoot(document.getElementById("root")!).render(<App />);
