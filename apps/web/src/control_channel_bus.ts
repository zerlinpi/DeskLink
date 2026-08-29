type ControlChannelDetail = {
  channel: RTCDataChannel;
  peer: RTCPeerConnection;
};

type ControlMessageDetail = {
  channel: RTCDataChannel;
  message: Record<string, unknown>;
};

const MAX_CONTROL_MESSAGE_CHARS = 512 * 1024;
const peerPrototype = RTCPeerConnection.prototype;
const originalCreateDataChannel = peerPrototype.createDataChannel;

function dispatchChannel(name: string, channel: RTCDataChannel, peer: RTCPeerConnection) {
  window.dispatchEvent(new CustomEvent<ControlChannelDetail>(name, {
    detail: { channel, peer },
  }));
}

peerPrototype.createDataChannel = function createDeskLinkDataChannel(
  this: RTCPeerConnection,
  label: string,
  dataChannelDict?: RTCDataChannelInit,
): RTCDataChannel {
  const channel = originalCreateDataChannel.call(this, label, dataChannelDict);
  if (label !== "control") return channel;

  dispatchChannel("desklink:control-channel", channel, this);

  channel.addEventListener("message", (event) => {
    if (typeof event.data !== "string" ||
        event.data.length === 0 ||
        event.data.length > MAX_CONTROL_MESSAGE_CHARS) {
      return;
    }
    try {
      const parsed = JSON.parse(event.data) as unknown;
      if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return;
      window.dispatchEvent(new CustomEvent<ControlMessageDetail>("desklink:control-message", {
        detail: {
          channel,
          message: parsed as Record<string, unknown>,
        },
      }));
    } catch {
      // Non-JSON application messages are not part of DeskLink control v0.
    }
  });

  channel.addEventListener("close", () => {
    dispatchChannel("desklink:control-channel-closed", channel, this);
  });

  return channel;
} as typeof peerPrototype.createDataChannel;

export {};
