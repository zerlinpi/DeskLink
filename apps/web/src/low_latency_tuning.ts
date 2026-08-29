type TunableReceiver = RTCRtpReceiver & {
  playoutDelayHint?: number | null;
  jitterBufferTarget?: number | null;
};

type AddTransceiver = RTCPeerConnection["addTransceiver"];

const prototype = RTCPeerConnection.prototype;
const originalAddTransceiver: AddTransceiver = prototype.addTransceiver;

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

prototype.addTransceiver = function addDeskLinkTransceiver(
  trackOrKind: MediaStreamTrack | string,
  init?: RTCRtpTransceiverInit,
): RTCRtpTransceiver {
  const transceiver = originalAddTransceiver.call(this, trackOrKind, init);
  const kind = typeof trackOrKind === "string" ? trackOrKind : trackOrKind.kind;
  if (kind === "video") tuneReceiver(transceiver.receiver);
  return transceiver;
} as AddTransceiver;
