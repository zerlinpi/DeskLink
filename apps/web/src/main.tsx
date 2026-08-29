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

const SIGNAL_URL = import.meta.env.VITE_SIGNAL_URL ?? "ws://localhost:8080/ws";
const ICE_SERVERS: RTCIceServer[] = [
  { urls: "stun:stun.l.google.com:19302" },
  {
    urls: ["turn:localhost:3478?transport=udp", "turn:localhost:3478?transport=tcp"],
    username: "desklink",
    credential: "CHANGE_ME_NOW",
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
  const sessionRef = useRef(crypto.randomUUID());

  const sendSignal = (type: string, payload: any = {}) => {
    wsRef.current?.send(JSON.stringify({ type, target: targetId, session: sessionRef.current, payload }));
  };

  const createPeer = () => {
    const pc = new RTCPeerConnection({ iceServers: ICE_SERVERS, bundlePolicy: "max-bundle" });
    pcRef.current = pc;
    pc.ontrack = (event) => {
      if (videoRef.current) videoRef.current.srcObject = event.streams[0];
    };
    pc.onicecandidate = (event) => {
      if (event.candidate) sendSignal("ice", event.candidate.toJSON());
    };
    pc.onconnectionstatechange = () => setStatus(pc.connectionState);
    const control = pc.createDataChannel("control", { ordered: true });
    controlRef.current = control;
    return pc;
  };

  const connect = async () => {
    if (!targetId.trim()) return;
    setStatus("signaling");
    const ws = new WebSocket(`${SIGNAL_URL}?deviceId=${encodeURIComponent(localId)}`);
    wsRef.current = ws;

    ws.onopen = async () => {
      const pc = createPeer();
      pc.addTransceiver("video", { direction: "recvonly" });
      const offer = await pc.createOffer({ offerToReceiveVideo: true });
      await pc.setLocalDescription(offer);
      sendSignal("offer", offer);
    };

    ws.onmessage = async (event) => {
      const msg: SignalMessage = JSON.parse(event.data);
      const pc = pcRef.current;
      if (!pc) return;
      if (msg.type === "answer") {
        await pc.setRemoteDescription(msg.payload);
      } else if (msg.type === "ice" && msg.payload) {
        await pc.addIceCandidate(msg.payload);
      } else if (msg.type === "peer-offline") {
        setStatus("peer offline");
      }
    };

    ws.onerror = () => setStatus("signal error");
    ws.onclose = () => setStatus((s) => (s === "connected" ? "disconnected" : s));
  };

  const sendPointer = (e: React.PointerEvent<HTMLVideoElement>, kind: "move" | "down" | "up") => {
    const dc = controlRef.current;
    if (!dc || dc.readyState !== "open") return;
    const r = e.currentTarget.getBoundingClientRect();
    dc.send(JSON.stringify({
      t: "pointer",
      kind,
      x: Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)),
      y: Math.max(0, Math.min(1, (e.clientY - r.top) / r.height)),
      button: e.button,
      buttons: e.buttons,
    }));
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
        <input value={targetId} onChange={(e) => setTargetId(e.target.value)} placeholder="Remote device ID" />
        <button onClick={connect}>Connect</button>
      </section>

      <section className="stage">
        <video
          ref={videoRef}
          autoPlay
          playsInline
          tabIndex={0}
          onPointerMove={(e) => sendPointer(e, "move")}
          onPointerDown={(e) => {
            e.currentTarget.setPointerCapture(e.pointerId);
            sendPointer(e, "down");
          }}
          onPointerUp={(e) => sendPointer(e, "up")}
          onKeyDown={(e) => {
            if (controlRef.current?.readyState === "open") {
              e.preventDefault();
              controlRef.current.send(JSON.stringify({ t: "key", kind: "down", code: e.code, key: e.key }));
            }
          }}
          onKeyUp={(e) => {
            if (controlRef.current?.readyState === "open") {
              e.preventDefault();
              controlRef.current.send(JSON.stringify({ t: "key", kind: "up", code: e.code, key: e.key }));
            }
          }}
        />
        {status === "idle" && <div className="empty">Enter a device ID to start a low-latency session.</div>}
      </section>
    </main>
  );
}

createRoot(document.getElementById("root")!).render(<App />);
