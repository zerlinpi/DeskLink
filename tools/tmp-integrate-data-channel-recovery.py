from pathlib import Path

path = Path("apps/web/src/main.tsx")
text = path.read_text(encoding="utf-8")

replacements = [
    (
        'import { AsyncAttemptCoordinator } from "./async_attempt";\nimport { resolveControllerSessionUrl } from "./controller_session";\n',
        'import { AsyncAttemptCoordinator } from "./async_attempt";\nimport {\n  dataChannelRecoveryDelayMs,\n  resetDataChannelRecoveryAttempt,\n  shouldScheduleDataChannelRecovery,\n  type RecoverableChannelKind,\n} from "./data_channel_recovery";\nimport { resolveControllerSessionUrl } from "./controller_session";\n',
    ),
    (
        '  const controlRef = useRef<RTCDataChannel | null>(null);\n  const pointerRef = useRef<RTCDataChannel | null>(null);\n  const sessionRef = useRef(crypto.randomUUID());\n',
        '  const controlRef = useRef<RTCDataChannel | null>(null);\n  const pointerRef = useRef<RTCDataChannel | null>(null);\n  const dataChannelRecoveryTimerRef = useRef<Record<RecoverableChannelKind, number | null>>({\n    control: null,\n    pointer: null,\n  });\n  const dataChannelRecoveryAttemptRef = useRef<Record<RecoverableChannelKind, number>>({\n    control: 0,\n    pointer: 0,\n  });\n  const sessionRef = useRef(crypto.randomUUID());\n',
    ),
    (
        '  const clearHostWaitRefreshTimer = () => {\n    if (hostWaitRefreshTimerRef.current !== null) {\n      window.clearTimeout(hostWaitRefreshTimerRef.current);\n    }\n    hostWaitRefreshTimerRef.current = null;\n  };\n\n  const scheduleHostWaitRefresh = (expiresInMs: unknown) => {\n',
        '  const clearHostWaitRefreshTimer = () => {\n    if (hostWaitRefreshTimerRef.current !== null) {\n      window.clearTimeout(hostWaitRefreshTimerRef.current);\n    }\n    hostWaitRefreshTimerRef.current = null;\n  };\n\n  const clearDataChannelRecoveryTimer = (kind: RecoverableChannelKind) => {\n    const timer = dataChannelRecoveryTimerRef.current[kind];\n    if (timer !== null) window.clearTimeout(timer);\n    dataChannelRecoveryTimerRef.current[kind] = null;\n  };\n\n  const resetDataChannelRecoveryState = () => {\n    clearDataChannelRecoveryTimer("control");\n    clearDataChannelRecoveryTimer("pointer");\n    dataChannelRecoveryAttemptRef.current = { control: 0, pointer: 0 };\n  };\n\n  const scheduleHostWaitRefresh = (expiresInMs: unknown) => {\n',
    ),
    (
        '      clearSignalReconnectTimer();\n      clearHostWaitRefreshTimer();\n      clearRuntimeControllerSession();\n',
        '      clearSignalReconnectTimer();\n      clearHostWaitRefreshTimer();\n      resetDataChannelRecoveryState();\n      clearRuntimeControllerSession();\n',
    ),
]

for old, new in replacements:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected exactly one main.tsx anchor, got {count}: {old[:120]!r}")
    text = text.replace(old, new, 1)

old_channels = """    const control = pc.createDataChannel(\"control\", { ordered: true });
    controlRef.current = control;
    control.onopen = () => {
      if (!isActiveSessionResource(controlRef.current, control, manualDisconnectRef.current) ||
          !isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current)) {
        return;
      }
      setStatus(wsRef.current?.readyState === WebSocket.OPEN
        ? \"control ready\"
        : \"control ready · signaling reconnecting\");
      startTelemetry(pc);
    };
    control.onmessage = (event) => {
      if (!isActiveSessionResource(controlRef.current, control, manualDisconnectRef.current) ||
          !isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current) ||
          typeof event.data !== \"string\") {
        return;
      }

      const decoded = decodeControlChannelText(event.data);
      if (decoded.kind === \"host-capabilities\") {
        setHostCapabilities(decoded.capabilities);
      } else if (decoded.kind === \"invalid\" && event.data.includes('\"t\":\"host-capabilities\"')) {
        console.debug(\"DeskLink rejected malformed host capabilities\");
      }
    };
    control.onclose = () => {
      if (controlRef.current === control) {
        stopTelemetry();
        setHostCapabilities(null);
      }
    };

    const pointer = pc.createDataChannel(\"pointer\", {
      ordered: false,
      maxRetransmits: 0,
    });
    pointer.bufferedAmountLowThreshold = POINTER_MOVE_BUFFER_BUDGET_BYTES;
    pointerRef.current = pointer;
    pointer.onopen = () => {
      if (!isActiveSessionResource(pointerRef.current, pointer, manualDisconnectRef.current) ||
          !isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current)) {
        return;
      }
      flushPendingPointerMove();
    };
    pointer.onbufferedamountlow = () => {
      if (!isActiveSessionResource(pointerRef.current, pointer, manualDisconnectRef.current) ||
          !isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current)) {
        return;
      }
      flushPendingPointerMove();
    };
    pointer.onclose = () => {
      if (pointerRef.current !== pointer) return;
      pendingMoveRef.current = null;
      if (pointerRafRef.current !== null) cancelAnimationFrame(pointerRafRef.current);
      pointerRafRef.current = null;
      pointerRef.current = null;
    };

    return pc;
"""
new_channels = """    createRecoverableDataChannel(pc, \"control\");
    createRecoverableDataChannel(pc, \"pointer\");

    return pc;
"""
if text.count(old_channels) != 1:
    raise SystemExit(f"expected one DataChannel creation block, got {text.count(old_channels)}")
text = text.replace(old_channels, new_channels, 1)

create_peer_anchor = '  const createPeer = (iceServers: RTCIceServer[]) => {\n'
helpers = """  const scheduleDataChannelRecovery = (
    pc: RTCPeerConnection,
    kind: RecoverableChannelKind,
    channel: RTCDataChannel,
  ) => {
    const currentChannel = kind === \"control\" ? controlRef.current : pointerRef.current;
    if (!shouldScheduleDataChannelRecovery({
      manualDisconnect: manualDisconnectRef.current,
      currentPeer: pcRef.current === pc,
      peerState: pc.connectionState,
      channelCurrent: currentChannel === channel,
      channelState: channel.readyState,
      replacementScheduled: dataChannelRecoveryTimerRef.current[kind] !== null,
    })) return;

    const attempt = dataChannelRecoveryAttemptRef.current[kind];
    dataChannelRecoveryTimerRef.current[kind] = window.setTimeout(() => {
      dataChannelRecoveryTimerRef.current[kind] = null;
      if (!isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current) ||
          pc.connectionState !== \"connected\") {
        return;
      }
      const activeChannel = kind === \"control\" ? controlRef.current : pointerRef.current;
      if (activeChannel !== channel || channel.readyState !== \"closed\") return;

      dataChannelRecoveryAttemptRef.current = {
        ...dataChannelRecoveryAttemptRef.current,
        [kind]: attempt + 1,
      };
      try {
        createRecoverableDataChannel(pc, kind);
      } catch (error) {
        console.debug(`DeskLink ${kind} DataChannel replacement failed`, error);
        scheduleDataChannelRecovery(pc, kind, channel);
      }
    }, dataChannelRecoveryDelayMs(attempt));
  };

  const attachControlChannel = (pc: RTCPeerConnection, control: RTCDataChannel) => {
    controlRef.current = control;
    control.onopen = () => {
      if (!isActiveSessionResource(controlRef.current, control, manualDisconnectRef.current) ||
          !isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current)) {
        return;
      }
      clearDataChannelRecoveryTimer(\"control\");
      dataChannelRecoveryAttemptRef.current = resetDataChannelRecoveryAttempt(
        \"control\",
        dataChannelRecoveryAttemptRef.current,
      );
      setStatus(wsRef.current?.readyState === WebSocket.OPEN
        ? \"control ready\"
        : \"control ready · signaling reconnecting\");
      startTelemetry(pc);
    };
    control.onmessage = (event) => {
      if (!isActiveSessionResource(controlRef.current, control, manualDisconnectRef.current) ||
          !isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current) ||
          typeof event.data !== \"string\") {
        return;
      }

      const decoded = decodeControlChannelText(event.data);
      if (decoded.kind === \"host-capabilities\") {
        setHostCapabilities(decoded.capabilities);
      } else if (decoded.kind === \"invalid\" && event.data.includes('\"t\":\"host-capabilities\"')) {
        console.debug(\"DeskLink rejected malformed host capabilities\");
      }
    };
    control.onclose = () => {
      if (controlRef.current !== control) return;
      stopTelemetry();
      setHostCapabilities(null);
      if (isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current) &&
          pc.connectionState === \"connected\") {
        setStatus(\"recovering control channel\");
      }
      scheduleDataChannelRecovery(pc, \"control\", control);
    };
  };

  const attachPointerChannel = (pc: RTCPeerConnection, pointer: RTCDataChannel) => {
    pointer.bufferedAmountLowThreshold = POINTER_MOVE_BUFFER_BUDGET_BYTES;
    pointerRef.current = pointer;
    pointer.onopen = () => {
      if (!isActiveSessionResource(pointerRef.current, pointer, manualDisconnectRef.current) ||
          !isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current)) {
        return;
      }
      clearDataChannelRecoveryTimer(\"pointer\");
      dataChannelRecoveryAttemptRef.current = resetDataChannelRecoveryAttempt(
        \"pointer\",
        dataChannelRecoveryAttemptRef.current,
      );
      flushPendingPointerMove();
    };
    pointer.onbufferedamountlow = () => {
      if (!isActiveSessionResource(pointerRef.current, pointer, manualDisconnectRef.current) ||
          !isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current)) {
        return;
      }
      flushPendingPointerMove();
    };
    pointer.onclose = () => {
      if (pointerRef.current !== pointer) return;
      if (pointerRafRef.current !== null) cancelAnimationFrame(pointerRafRef.current);
      pointerRafRef.current = null;
      scheduleDataChannelRecovery(pc, \"pointer\", pointer);
    };
  };

  const createRecoverableDataChannel = (
    pc: RTCPeerConnection,
    kind: RecoverableChannelKind,
  ) => {
    if (!isActiveSessionResource(pcRef.current, pc, manualDisconnectRef.current) ||
        pc.connectionState === \"closed\") {
      return;
    }

    if (kind === \"control\") {
      attachControlChannel(pc, pc.createDataChannel(\"control\", { ordered: true }));
      return;
    }

    attachPointerChannel(pc, pc.createDataChannel(\"pointer\", {
      ordered: false,
      maxRetransmits: 0,
    }));
  };

"""
if text.count(create_peer_anchor) != 1:
    raise SystemExit(f"expected one createPeer anchor, got {text.count(create_peer_anchor)}")
text = text.replace(create_peer_anchor, helpers + create_peer_anchor, 1)

old_connected = '        if (controlRef.current?.readyState === "open") startTelemetry(pc);\n      } else if (state === "disconnected") {\n'
new_connected = '        if (controlRef.current?.readyState === "open") startTelemetry(pc);\n        if (controlRef.current?.readyState === "closed") {\n          scheduleDataChannelRecovery(pc, "control", controlRef.current);\n        }\n        if (pointerRef.current?.readyState === "closed") {\n          scheduleDataChannelRecovery(pc, "pointer", pointerRef.current);\n        }\n      } else if (state === "disconnected") {\n'
if text.count(old_connected) != 1:
    raise SystemExit(f"expected one connected-state anchor, got {text.count(old_connected)}")
text = text.replace(old_connected, new_connected, 1)

old_disconnected = """      } else if (state === \"disconnected\") {
        setStatus(\"reconnecting network\");
        scheduleIceRestart(pc, false);
      } else if (state === \"failed\") {
        stopTelemetry();
        setStatus(\"reconnecting network\");
        scheduleIceRestart(pc, true);
      } else if (state === \"closed\") {
        stopTelemetry();
        clearIceRestartTimer();
        clearIceRestartWatchdog();
        iceRestartInFlightRef.current = false;
"""
new_disconnected = """      } else if (state === \"disconnected\") {
        clearDataChannelRecoveryTimer(\"control\");
        clearDataChannelRecoveryTimer(\"pointer\");
        setStatus(\"reconnecting network\");
        scheduleIceRestart(pc, false);
      } else if (state === \"failed\") {
        stopTelemetry();
        clearDataChannelRecoveryTimer(\"control\");
        clearDataChannelRecoveryTimer(\"pointer\");
        setStatus(\"reconnecting network\");
        scheduleIceRestart(pc, true);
      } else if (state === \"closed\") {
        stopTelemetry();
        clearIceRestartTimer();
        clearIceRestartWatchdog();
        clearDataChannelRecoveryTimer(\"control\");
        clearDataChannelRecoveryTimer(\"pointer\");
        iceRestartInFlightRef.current = false;
"""
if text.count(old_disconnected) != 1:
    raise SystemExit(f"expected one peer-state recovery anchor, got {text.count(old_disconnected)}")
text = text.replace(old_disconnected, new_disconnected, 1)

old_peer_reset = '    pendingLocalIceRef.current = [];\n    pendingRemoteIceRef.current = [];\n    setHostCapabilities(null);\n'
new_peer_reset = '    pendingLocalIceRef.current = [];\n    pendingRemoteIceRef.current = [];\n    resetDataChannelRecoveryState();\n    setHostCapabilities(null);\n'
if text.count(old_peer_reset) != 1:
    raise SystemExit(f"expected one peer reset anchor, got {text.count(old_peer_reset)}")
text = text.replace(old_peer_reset, new_peer_reset, 1)

old_disconnect = '    clearSignalReconnectTimer();\n    clearHostWaitRefreshTimer();\n    iceRestartInFlightRef.current = false;\n'
new_disconnect = '    clearSignalReconnectTimer();\n    clearHostWaitRefreshTimer();\n    resetDataChannelRecoveryState();\n    iceRestartInFlightRef.current = false;\n'
if text.count(old_disconnect) != 1:
    raise SystemExit(f"expected one disconnect cleanup anchor, got {text.count(old_disconnect)}")
text = text.replace(old_disconnect, new_disconnect, 1)

path.write_text(text, encoding="utf-8")

ui_path = Path("apps/web/src/home_ui.ts")
ui = ui_path.read_text(encoding="utf-8")
old_ui = '  if (normalized.includes("reconnecting network")) return "网络恢复中";\n'
new_ui = '  if (normalized.includes("recovering control channel")) return "控制通道恢复中";\n  if (normalized.includes("reconnecting network")) return "网络恢复中";\n'
if ui.count(old_ui) != 1:
    raise SystemExit(f"expected one home UI recovery status anchor, got {ui.count(old_ui)}")
ui_path.write_text(ui.replace(old_ui, new_ui, 1), encoding="utf-8")
