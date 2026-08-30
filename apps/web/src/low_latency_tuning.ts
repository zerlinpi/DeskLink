type TunableReceiver = RTCRtpReceiver & {
  playoutDelayHint?: number | null;
  jitterBufferTarget?: number | null;
};

type AddTransceiver = RTCPeerConnection["addTransceiver"];
type DataChannelSend = RTCDataChannel["send"];

// Pointer motion is deliberately latest-wins. Even an unordered/unreliable SCTP
// DataChannel can accumulate a local send buffer faster than the network drains
// it. Once that happens, sending old mouse positions only increases perceived
// latency. Drop only move events under backpressure; wheel, button, keyboard and
// release-all events retain their existing delivery semantics.
const POINTER_MOVE_MAX_BUFFERED_BYTES = 8 * 1024;

function tuneReceiver(receiver: RTCRtpReceiver) {
  const tunable = receiver as TunableReceiver;

  try {
    if ("playoutDelayHint" in tunable) {
      tunable.playoutDelayHint = 0;
    }
  } catch (error) {
    console.debug("DeskLink playoutDelayHint unavailable", error);
  }

  try {
    if ("jitterBufferTarget" in tunable) {
      tunable.jitterBufferTarget = 0;
    }
  } catch (error) {
    console.debug("DeskLink jitterBufferTarget unavailable", error);
  }
}

function isStalePointerMove(channel: RTCDataChannel, data: unknown) {
  if (channel.label !== "pointer" || channel.bufferedAmount <= POINTER_MOVE_MAX_BUFFERED_BYTES) {
    return false;
  }
  if (typeof data !== "string") return false;
  return data.includes('"t":"pointer"') && data.includes('"kind":"move"');
}

// This module loads before browser_preflight.ts. Feature-detect the WebRTC
// globals instead of throwing during module evaluation, so unsupported browsers
// still reach DeskLink's user-facing compatibility explanation.
if (typeof RTCPeerConnection !== "undefined") {
  const peerPrototype = RTCPeerConnection.prototype;
  const originalAddTransceiver: AddTransceiver = peerPrototype.addTransceiver;
  peerPrototype.addTransceiver = function addDeskLinkTransceiver(
    this: RTCPeerConnection,
    trackOrKind: MediaStreamTrack | string,
    init?: RTCRtpTransceiverInit,
  ): RTCRtpTransceiver {
    const transceiver = originalAddTransceiver.call(this, trackOrKind, init);
    const kind = typeof trackOrKind === "string" ? trackOrKind : trackOrKind.kind;
    if (kind === "video") tuneReceiver(transceiver.receiver);
    return transceiver;
  } as AddTransceiver;
}

if (typeof RTCDataChannel !== "undefined") {
  const dataChannelPrototype = RTCDataChannel.prototype;
  const originalDataChannelSend: DataChannelSend = dataChannelPrototype.send;
  dataChannelPrototype.send = function sendDeskLinkData(
    this: RTCDataChannel,
    data: string | Blob | ArrayBuffer | ArrayBufferView,
  ): void {
    if (isStalePointerMove(this, data)) return;
    originalDataChannelSend.call(this, data as never);
  } as DataChannelSend;
}
