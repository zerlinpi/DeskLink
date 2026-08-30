import React, { useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { accessProofAlgorithm, createAccessProof } from "./access_proof";
import { requestControllerSession, resolveControllerSessionUrl } from "./controller_session";
import {
  hostWaitRefreshDelayMs,
  iceRestartDelayMs,
  shouldScheduleIceRestart,
  signalReconnectDelayMs,
} from "./session_recovery";
import { shouldAcceptSignalMessage } from "./signal_scope";
import "./styles.css";

type SignalMessage = {
  type: string;
  from?: string;
  target?: string;
  session?: string;
  payload?: any;
  message?: string;
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

type NetworkView = {
  route: "direct" | "relay" | "unknown";
  protocol: string;
  rttMs: number | null;
  lossPct: number;
  jitterMs: number | null;
  decodeFps: number;
  availableIncomingBitrate: number | null;
};

type TurnCredentialResponse = {
  username: string;
  password: string;
  expiresAt: number;
};

const EMPTY_NETWORK_VIEW: NetworkView = {
  route: "unknown",
  protocol: "-",
  rttMs: null,
  lossPct: 0,
  jitterMs: null,
  decodeFps: 0,
  availableIncomingBitrate: null,
};

const SIGNAL_URL = import.meta.env.VITE_SIGNAL_URL ?? "ws://localhost:8080/ws";
const SIGNAL_DEVICE_ID = import.meta.env.VITE_SIGNAL_DEVICE_ID ?? "";
const STATIC_SIGNAL_AUTH_TOKEN = import.meta.env.VITE_SIGNAL_AUTH_TOKEN ?? "";
const CONTROLLER_SESSION_URL = import.meta.env.VITE_CONTROLLER_SESSION_URL ?? "";
const CONTROLLER_AUTH_REQUIRED = import.meta.env.VITE_CONTROLLER_AUTH_REQUIRED === "1";
const STUN_URL = import.meta.env.VITE_STUN_URL ?? "stun:stun.l.google.com:19302";
const TURN_URL = import.meta.env.VITE_TURN_URL ?? "turn:localhost:3478";
const TURN_TLS_URL = import.meta.env.VITE_TURN_TLS_URL ?? "";
const TURN_USERNAME = import.meta.env.VITE_TURN_USERNAME ?? "desklink";
const TURN_PASSWORD = import.meta.env.VITE_TURN_PASSWORD ?? "CHANGE_ME_NOW";
const TURN_CREDENTIALS_URL = import.meta.env.VITE_TURN_CREDENTIALS_URL ?? "";
const TURN_RUNTIME_REQUIRED = import.meta.env.VITE_TURN_RUNTIME_REQUIRED === "1";
const FORCE_RELAY = import.meta.env.VITE_ICE_TRANSPORT_POLICY === "relay";
const CONTROLLER_TOKEN_REFRESH_MARGIN_SECONDS = 90;
const SIGNAL_PROTOCOL = "desklink-v1";
const CONTROLLER_AUTH_PROTOCOL_PREFIX = "desklink-auth.";
const TURN_URLS = TURN_URL.startsWith("turns:")
  ? [TURN_URL]
  : [`${TURN_URL}?transport=udp`, `${TURN_URL}?transport=tcp`];
if (TURN_TLS_URL) TURN_URLS.push(TURN_TLS_URL);

function staticIceServers(username = TURN_USERNAME, password = TURN_PASSWORD): RTCIceServer[] {
  const servers: RTCIceServer[] = [];
  if (STUN_URL) servers.push({ urls: STUN_URL });
  if (TURN_URLS.length && username && password) {
    servers.push({
      urls: TURN_URLS,
      username,
      credential: password,
    });
  }
  return servers;
}

async function resolveIceServers(deviceId: string, signalAuthToken: string): Promise<RTCIceServer[]> {
  if (!TURN_CREDENTIALS_URL) return staticIceServers();

  if (!signalAuthToken) {
    if (TURN_RUNTIME_REQUIRED) {
      throw new Error("runtime TURN credentials require a signal auth token");
    }
    console.warn("DeskLink runtime TURN credentials unavailable: signal auth token is missing");
    return staticIceServers();
  }

  try {
    const url = new URL(TURN_CREDENTIALS_URL, window.location.href);
    url.searchParams.set("deviceId", deviceId);
    const response = await fetch(url, {
      method: "GET",
      headers: {
        Authorization: `Bearer ${signalAuthToken}`,
        Accept: "application/json",
      },
      cache: "no-store",
      credentials: "omit",
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);

    const credentials = await response.json() as TurnCredentialResponse;
    if (!credentials.username || !credentials.password || !Number.isFinite(credentials.expiresAt)) {
      throw new Error("credential response is incomplete");
    }
    return staticIceServers(credentials.username, credentials.password);
  } catch (error) {
    if (TURN_RUNTIME_REQUIRED) throw error;
    console.warn("DeskLink runtime TURN credential fetch failed; using configured fallback", error);
    return staticIceServers();
  }
}

function buildSignalUrl(deviceId: string, legacySignalAuthToken = "") {
  const url = new URL(SIGNAL_URL, window.location.href);
  if (url.protocol === "http:") url.protocol = "ws:";
  if (url.protocol === "https:") url.protocol = "wss:";
  url.searchParams.set("deviceId", deviceId);
  if (legacySignalAuthToken) url.searchParams.set("auth", legacySignalAuthToken);
  return url.toString();
}

function buildSignalProtocols(runtimeControllerAuth: boolean, signalAuthToken: string) {
  const protocols = [SIGNAL_PROTOCOL];
  if (runtimeControllerAuth && signalAuthToken) {
    protocols.push(`${CONTROLLER_AUTH_PROTOCOL_PREFIX}${signalAuthToken}`);
  }
  return protocols;
}

function App() {
  const localId = useMemo(
    () => SIGNAL_DEVICE_ID || `web-${crypto.randomUUID().slice(0, 8)}`,
    [],
  );
  const [controllerAccount, setControllerAccount] = useState("");
  const [controllerKey, setControllerKey] = useState("");
  const [targetId, setTargetId] = useState("");
  const [accessCode, setAccessCode] = useState("");
  const [status, setStatus] = useState("idle");
  const [networkView, setNetworkView] = useState<NetworkView>(EMPTY_NETWORK_VIEW);
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
  const hostWaitRefreshTimerRef = useRef<number | null>(null);
  const manualDisconnectRef = useRef(false);
  const initialPeerStartingRef = useRef(false);
  const controllerTokenRef = useRef(STATIC_SIGNAL_AUTH_TOKEN);
  const controllerTokenExpiryRef = useRef(STATIC_SIGNAL_AUTH_TOKEN ? Number.MAX_SAFE_INTEGER : 0);
  const controllerTokenTargetRef = useRef("");
  const controllerTokenRequestRef = useRef<Promise<string> | null>(null);

  const runtimeControllerAuthEnabled = Boolean(CONTROLLER_SESSION_URL);

  const clearRuntimeControllerSession = () => {
    if (runtimeControllerAuthEnabled) {
      controllerTokenRef.current = "";
      controllerTokenExpiryRef.current = 0;
      controllerTokenTargetRef.current = "";
    }
    controllerTokenRequestRef.current = null;
  };

  const ensureSignalAuthToken = async (forceRefresh = false): Promise<string> => {
    if (!runtimeControllerAuthEnabled) {
      if (CONTROLLER_AUTH_REQUIRED && !STATIC_SIGNAL_AUTH_TOKEN) {
        throw new Error("runtime controller authentication is required but not configured");
      }
      return STATIC_SIGNAL_AUTH_TOKEN;
    }

    const normalizedTarget = targetId.trim();
    if (!normalizedTarget || !controllerAccount.trim() || !controllerKey) {
      throw new Error("controller account, controller key and target device are required");
    }

    const nowSeconds = Math.floor(Date.now() / 1000);
    if (!forceRefresh &&
        controllerTokenRef.current &&
        controllerTokenTargetRef.current === normalizedTarget &&
        controllerTokenExpiryRef.current > nowSeconds + CONTROLLER_TOKEN_REFRESH_MARGIN_SECONDS) {
      return controllerTokenRef.current;
    }

    if (controllerTokenRequestRef.current) return controllerTokenRequestRef.current;

    const request = (async () => {
      const endpoint = resolveControllerSessionUrl(SIGNAL_URL, CONTROLLER_SESSION_URL);
      const session = await requestControllerSession(endpoint, {
        accountId: controllerAccount.trim(),
        controllerId: localId,
        targetDeviceId: normalizedTarget,
        accessKey: controllerKey,
      });
      controllerTokenRef.current = session.token;
      controllerTokenExpiryRef.current = session.expiresAt;
      controllerTokenTargetRef.current = normalizedTarget;
      return session.token;
    })();
    controllerTokenRequestRef.current = request;
    try {
      return await request;
    } finally {
      if (controllerTokenRequestRef.current === request) controllerTokenRequestRef.current = null;
    }
  };

  const sendSignal = (type: string, payload: any = {}) => {
    const ws = wsRef.current;
    if (ws?.readyState !== WebSocket.OPEN) return false;
    ws.send(JSON.stringify({ type, target: targetId.trim(), session: sessionRef.current, payload }));
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

  const clearHostWaitRefreshTimer = () => {
    if (hostWaitRefreshTimerRef.current !== null) {
      window.clearTimeout(hostWaitRefreshTimerRef.current);
    }
    hostWaitRefreshTimerRef.current = null;
  };

  const scheduleHostWaitRefresh = (expiresInMs: unknown) => {
    clearHostWaitRefreshTimer();
    const delay = hostWaitRefreshDelayMs(expiresInMs);
    if (delay === null) return;
    const expectedSession = sessionRef.current;
    const expectedTarget = targetId.trim();

    hostWaitRefreshTimerRef.current = window.setTimeout(() => {
      hostWaitRefreshTimerRef.current = null;
      if (manualDisconnectRef.current ||
          pcRef.current ||
          wsRef.current?.readyState !== WebSocket.OPEN ||
          sessionRef.current !== expectedSession ||
          targetId.trim() !== expectedTarget) {
        return;
      }

      setStatus("host offline · refreshing wait");
      if (!sendSignal("auth-request", { version: 1 })) {
        setStatus("reconnecting signaling");
      }
    }, delay);
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
      clearHostWaitRefreshTimer();
      clearRuntimeControllerSession();
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

      const localCandidate = selectedPair?.localCandidateId
        ? report.get(selectedPair.localCandidateId) as any
        : null;
      const remoteCandidate = selectedPair?.remoteCandidateId
        ? report.get(selectedPair.remoteCandidateId) as any
        : null;
      const usesRelay = localCandidate?.candidateType === "relay" || remoteCandidate?.candidateType === "relay";
      const hasCandidatePath = Boolean(localCandidate || remoteCandidate);
      const route: NetworkView["route"] = usesRelay
        ? "relay"
        : (hasCandidatePath ? "direct" : "unknown");
      const protocol = [
        localCandidate?.protocol,
        localCandidate?.relayProtocol,
      ].filter(Boolean).join("/") || selectedPair?.protocol || "-";

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

      setNetworkView({
        route,
        protocol,
        rttMs,
        lossPct,
        jitterMs,
        decodeFps,
        availableIncomingBitrate,
      });

      sendReliable({
        t: "telemetry",
        rttMs,
        lossPct,
        decodeFps,
        jitterMs,
        framesDropped: Number(inboundVideo.framesDropped ?? 0),
        availableIncomingBitrate,
        route,
        protocol,
        localCandidateType: localCandidate?.candidateType ?? null,
        remoteCandidateType: remoteCandidate?.candidateType ?? null,
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
    if (!shouldScheduleIceRestart({
      manualDisconnect: manualDisconnectRef.current,
      currentPeer: pcRef.current === pc,
      restartInFlight: iceRestartInFlightRef.current,
      timerScheduled: iceRestartTimerRef.current !== null,
      connectionState: pc.connectionState,
    })) return;

    iceRestartTimerRef.current = window.setTimeout(() => {
      iceRestartTimerRef.current = null;
      if (manualDisconnectRef.current || pcRef.current !== pc) return;
      if (pc.connectionState === "connected" || pc.connectionState === "closed") return;
      void restartIce(pc);
    }, iceRestartDelayMs(immediate));
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
      if (TURN_CREDENTIALS_URL) {
        const authToken = await ensureSignalAuthToken(false);
        const iceServers = await resolveIceServers(localId, authToken);
        if (pcRef.current !== pc || manualDisconnectRef.current) return;
        pc.setConfiguration({
          ...pc.getConfiguration(),
          iceServers,
        });
      }

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

  const createPeer = (iceServers: RTCIceServer[]) => {
    clearHostWaitRefreshTimer();
    offerSentRef.current = false;
    negotiationPendingRef.current = false;
    pendingLocalIceRef.current = [];
    pendingRemoteIceRef.current = [];

    const pc = new RTCPeerConnection({
      iceServers,
      bundlePolicy: "max-bundle",
      iceTransportPolicy: FORCE_RELAY ? "relay" : "all",
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

  const startInitialPeer = async () => {
    if (initialPeerStartingRef.current || pcRef.current || manualDisconnectRef.current) return;
    initialPeerStartingRef.current = true;
    setStatus("negotiating WebRTC");
    try {
      const authToken = await ensureSignalAuthToken(false);
      const iceServers = await resolveIceServers(localId, authToken);
      if (manualDisconnectRef.current || pcRef.current) return;

      const pc = createPeer(iceServers);
      const transceiver = pc.addTransceiver("video", { direction: "recvonly" });
      const videoCapabilities = RTCRtpReceiver.getCapabilities("video");
      const h264Codecs = videoCapabilities?.codecs.filter(
        (codec) => codec.mimeType.toLowerCase() === "video/h264",
      );
      if (h264Codecs?.length) transceiver.setCodecPreferences(h264Codecs);

      if (!await sendOffer(pc, false)) setStatus("signal error");
    } catch (error) {
      console.debug("DeskLink initial connection setup failed", error);
      disconnect(TURN_RUNTIME_REQUIRED ? "TURN credential error" : "signal error");
    } finally {
      initialPeerStartingRef.current = false;
    }
  };

  const handleSignalMessage = async (event: MessageEvent) => {
    let msg: SignalMessage;
    try {
      msg = JSON.parse(event.data);
    } catch {
      return;
    }

    if (!shouldAcceptSignalMessage(msg, {
      hostId: targetId.trim(),
      sessionId: sessionRef.current,
    })) {
      console.debug("DeskLink ignored signal outside the active target/session scope", msg.type);
      return;
    }

    if (msg.type === "auth-challenge") {
      clearHostWaitRefreshTimer();
      const algorithm = msg.payload?.algorithm;
      const nonce = msg.payload?.nonce;
      if (algorithm !== accessProofAlgorithm() || typeof nonce !== "string") {
        disconnect("unsupported host authentication challenge");
        return;
      }
      try {
        setStatus("proving host access");
        const proof = await createAccessProof(
          accessCode,
          localId,
          targetId.trim(),
          sessionRef.current,
          nonce,
        );
        if (manualDisconnectRef.current) return;
        if (!sendSignal("auth-proof", { algorithm, proof })) {
          disconnect("host authentication signaling failed");
        }
      } catch (error) {
        console.debug("DeskLink host access proof failed", error);
        disconnect("host access authentication failed");
      }
      return;
    }

    if (msg.type === "auth-accepted") {
      clearHostWaitRefreshTimer();
      await startInitialPeer();
      return;
    }

    if (msg.type === "auth-rejected") {
      clearHostWaitRefreshTimer();
      const reason = msg.payload?.reason;
      if (reason === "host-unconfigured") {
        disconnect("host access code not configured");
      } else if (reason === "auth-busy") {
        disconnect("host authentication busy");
      } else if (reason === "auth-unavailable") {
        disconnect("host authentication unavailable");
      } else {
        disconnect("access code rejected");
      }
      return;
    }

    if (msg.type === "device-revoked") {
      disconnect("device revoked");
      return;
    }

    if (msg.type === "peer-offline") {
      const pc = pcRef.current;
      if (!pc) {
        if (msg.payload?.authQueued === true) {
          scheduleHostWaitRefresh(msg.payload?.expiresInMs);
          setStatus("host offline · waiting for host");
        } else {
          clearHostWaitRefreshTimer();
          setStatus("host offline");
        }
        return;
      }
      if (pc.connectionState === "connected") {
        setStatus("control ready · host signaling offline");
      } else {
        setStatus("host offline · retrying");
        scheduleIceRestart(pc, false);
      }
      return;
    }

    if (msg.type === "error" && msg.message?.includes("authorization scope")) {
      clearHostWaitRefreshTimer();
      disconnect("controller authorization rejected");
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
    }
  };

  const scheduleSignalReconnect = () => {
    if (manualDisconnectRef.current || signalReconnectTimerRef.current !== null) return;
    const current = wsRef.current;
    if (current?.readyState === WebSocket.OPEN || current?.readyState === WebSocket.CONNECTING) return;

    const attempt = signalReconnectAttemptRef.current++;
    const delay = signalReconnectDelayMs(attempt);
    signalReconnectTimerRef.current = window.setTimeout(() => {
      signalReconnectTimerRef.current = null;
      if (!manualDisconnectRef.current) void openSignalSocket(false);
    }, delay);
  };

  const openSignalSocket = async (initial: boolean) => {
    if (manualDisconnectRef.current) return;

    let authToken = "";
    try {
      authToken = await ensureSignalAuthToken(false);
    } catch (error) {
      console.debug("DeskLink controller authorization failed", error);
      clearHostWaitRefreshTimer();
      setStatus(error instanceof Error ? error.message : "controller authorization failed");
      manualDisconnectRef.current = true;
      return;
    }
    if (manualDisconnectRef.current) return;

    const legacyAuthToken = runtimeControllerAuthEnabled ? "" : authToken;
    const ws = new WebSocket(
      buildSignalUrl(localId, legacyAuthToken),
      buildSignalProtocols(runtimeControllerAuthEnabled, authToken),
    );
    wsRef.current = ws;

    ws.onmessage = (event) => {
      void handleSignalMessage(event);
    };

    ws.onopen = () => {
      if (wsRef.current !== ws || manualDisconnectRef.current) return;
      signalReconnectAttemptRef.current = 0;
      clearSignalReconnectTimer();

      if (initial) {
        clearHostWaitRefreshTimer();
        setStatus("authorizing host");
        if (!sendSignal("auth-request", { version: 1 })) {
          disconnect("host authentication signaling failed");
        }
        return;
      }

      const pc = pcRef.current;
      if (!pc) {
        clearHostWaitRefreshTimer();
        setStatus("authorizing host");
        if (!sendSignal("auth-request", { version: 1 })) {
          disconnect("host authentication signaling failed");
        }
        return;
      }
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
      clearHostWaitRefreshTimer();
      if (pc && pc.connectionState !== "closed") {
        setStatus(pc.connectionState === "connected"
          ? "control ready · signaling reconnecting"
          : "reconnecting signaling");
        scheduleSignalReconnect();
      } else {
        setStatus("reconnecting signaling");
        scheduleSignalReconnect();
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
    clearHostWaitRefreshTimer();
    iceRestartInFlightRef.current = false;
    negotiationPendingRef.current = false;
    signalReconnectAttemptRef.current = 0;
    initialPeerStartingRef.current = false;

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
    setNetworkView(EMPTY_NETWORK_VIEW);
    clearRuntimeControllerSession();
    setStatus(nextStatus);
  };

  const connect = async () => {
    if (!targetId.trim() || !accessCode || status !== "idle") return;
    if (runtimeControllerAuthEnabled && (!controllerAccount.trim() || !controllerKey)) return;
    manualDisconnectRef.current = false;
    signalReconnectAttemptRef.current = 0;
    setNetworkView(EMPTY_NETWORK_VIEW);
    setStatus(runtimeControllerAuthEnabled ? "authorizing controller" : "signaling");
    await openSignalSocket(true);
  };

  const controllerFieldsReady = !runtimeControllerAuthEnabled ||
    (Boolean(controllerAccount.trim()) && Boolean(controllerKey));
  const canConnect = status === "idle" && Boolean(targetId.trim()) && Boolean(accessCode) && controllerFieldsReady;
  const routeLabel = networkView.route === "relay"
    ? "TURN relay"
    : networkView.route === "direct" ? "Direct P2P" : "Route pending";
  const bitrateMbps = networkView.availableIncomingBitrate == null
    ? "-"
    : (networkView.availableIncomingBitrate / 1_000_000).toFixed(1);

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
        {runtimeControllerAuthEnabled && (
          <>
            <input
              value={controllerAccount}
              onChange={(e) => setControllerAccount(e.target.value)}
              placeholder="Controller account"
              autoComplete="username"
              disabled={status !== "idle"}
            />
            <input
              type="password"
              value={controllerKey}
              onChange={(e) => setControllerKey(e.target.value)}
              placeholder="Controller key"
              autoComplete="current-password"
              disabled={status !== "idle"}
            />
          </>
        )}
        <input
          value={targetId}
          onChange={(e) => {
            setTargetId(e.target.value);
            if (status === "idle") clearRuntimeControllerSession();
          }}
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

        {status !== "idle" && (
          <div className="network-hud" aria-label="WebRTC network diagnostics">
            <span className={`route route-${networkView.route}`}>{routeLabel}</span>
            <span>{networkView.protocol.toUpperCase()}</span>
            <span>RTT {networkView.rttMs == null ? "-" : `${networkView.rttMs.toFixed(0)} ms`}</span>
            <span>Loss {networkView.lossPct.toFixed(1)}%</span>
            <span>Jitter {networkView.jitterMs == null ? "-" : `${networkView.jitterMs.toFixed(1)} ms`}</span>
            <span>Decode {networkView.decodeFps.toFixed(0)} fps</span>
            <span>Avail {bitrateMbps} Mbps</span>
          </div>
        )}

        {status === "idle" && (
          <div className="empty">
            {runtimeControllerAuthEnabled
              ? "Enter controller credentials, device ID and access code to start a scoped session."
              : "Enter the device ID and access code to start a low-latency session."}
          </div>
        )}
      </section>
    </main>
  );
}

createRoot(document.getElementById("root")!).render(<App />);
