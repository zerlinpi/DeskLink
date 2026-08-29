import React, { useMemo, useRef, useState } from "react";
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
  const [status, setStatus] = useState("idle");
  const videoRef = useRef<HTMLVideoElement>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const pcRef = useRef<RTCPeerConnection | null>(null);
  const controlRef = useRef<RTCDataChannel | null>(null);
  const pointerRef = useRef<RTCDataChannel | null>(null);
  const sessionRef = useRef(crypto.randomUUID());
  const pendingMoveRef = useRef<PointerPayload | null>(null);
  const pointerRafRef = useRef<number | null>(null);

  const sendSignal = (type: string, payload: any = {}) => {
    if (wsRef.current?.readyState !== WebSocket.OPEN) return;
    wsRef.current.send(JSON.stringify({ type, target: targetId, session: sessionRef.current, payload }));
  };

  const sendReliable = (payload: object) => {
    const channel = controlRef.current;
    if (channel?.readyState === "open") channel.send(JSON.stringify(payload));
  };

  const sendPointerFast = (payload: object) => {
    const channel = pointerRef.current;
    if (channel?.readyState === "open") channel.send(JSON.stringify(payload));
  };

  const pointFromEvent = (
    e: React.PointerEvent<HTMLVideoElement>,
    kind: PointerPayload["kind"],
  ): PointerPayload => {
    const r = e.currentTarget.getBoundingClientRect();
    return {
      t: "pointer",
      kind,
      x: Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)),
      y: Math.max(0, Math.min(1, (e.clientY - r.top) / r.height)),
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

  const createPeer = () => {
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
      if (event.candidate) sendSignal("ice", event.candidate.toJSON());
    };
    pc.onconnectionstatechange = () => setStatus(pc.connectionState);

    const control = pc.createDataChannel("control", { ordered: true });
    controlRef.current = control;
    control.onopen = () => setStatus("control ready");

    // Mouse movement can discard stale packets. This prevents head-of-line blocking
    // from making the remote cursor feel sticky when a packet is lost.
    const pointer = pc.createDataChannel("pointer", {
      ordered: false,
      maxRetransmits: 0,
    });
    pointerRef.current = pointer;

    return pc;
  };

  const disconnect = () => {
    if (pointerRafRef.current !== null) cancelAnimationFrame(pointerRafRef.current);
    pointerRafRef.current = null;
    pendingMoveRef.current = null;
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
    setStatus("idle");
  };

  const connect = async () => {
    if (!targetId.trim() || status !== "idle") return;
    setStatus("signaling");

    const ws = new WebSocket(`${SIGNAL_URL}?deviceId=${encodeURIComponent(localId)}`);
    wsRef.current = ws;

    ws.onopen = async () => {
      const pc = createPeer();
      const transceiver = pc.addTransceiver("video", { direction: "recvonly" });

      // Keep M0/M1 deterministic: the native Windows host sends hardware H.264.
      // Payload type numbers are still negotiated dynamically from the generated SDP.
      const videoCapabilities = RTCRtpReceiver.getCapabilities("video");
      const h264Codecs = videoCapabilities?.codecs.filter(
        (codec) => codec.mimeType.toLowerCase() === "video/h264",
      );
      if (h264Codecs?.length) {
        transceiver.setCodecPreferences(h264Codecs);
      }

      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      sendSignal("offer", offer);
    };

    ws.onmessage = async (event) => {
      const msg: SignalMessage = JSON.parse(event.data);
      const pc = pcRef.current;
      if (!pc) return;

      if (msg.type === "answer" && msg.payload) {
        await pc.setRemoteDescription(msg.payload as RTCSessionDescriptionInit);
      } else if (msg.type === "ice" && msg.payload) {
        await pc.addIceCandidate(msg.payload as RTCIceCandidateInit);
      } else if (msg.type === "peer-offline") {
        setStatus("peer offline");
      }
    };

    ws.onerror = () => setStatus("signal error");
    ws.onclose = () => setStatus((current) => (current === "idle" ? current : "disconnected"));
  };

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
          disabled={status !== "idle"}
        />
        {status === "idle" ? (
          <button onClick={connect}>Connect</button>
        ) : (
          <button onClick={disconnect}>Disconnect</button>
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
        {status === "idle" && <div className="empty">Enter a device ID to start a low-latency session.</div>}
      </section>
    </main>
  );
}

createRoot(document.getElementById("root")!).render(<App />);
