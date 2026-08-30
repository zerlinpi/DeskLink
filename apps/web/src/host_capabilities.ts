export type CapabilityMetadata = Record<string, unknown>;

export type HostCapability<TMetadata extends CapabilityMetadata = CapabilityMetadata> = {
  available: boolean;
  reason?: string;
  metadata?: TMetadata;
};

export type ResolutionLimit = {
  width: number;
  height: number;
};

export type CodecCapability = {
  codec: string;
  profiles?: string[];
  maximumFps?: number;
  maximumResolution?: ResolutionLimit;
};

export type HostCapabilitiesV1 = {
  version: 1;
  secureAttention: HostCapability<{ policy?: string }>;
  clipboard: HostCapability;
  fileTransfer: HostCapability;
  systemAudio: HostCapability<{ backend?: string }>;
  microphone: HostCapability<{ backend?: string }>;
  protectedDesktop: HostCapability;
  multiMonitor: HostCapability;
  highRefresh: HostCapability;
  virtualDisplay: HostCapability;
  privacyMode: HostCapability;
  virtualHid: HostCapability;
  gamepad: HostCapability;
  codecs: CodecCapability[];
  maximumFps: number;
  maximumResolution: ResolutionLimit;
};

export type HostCapabilitiesMessageV1 = {
  t: "host-capabilities";
  version: 1;
  capabilities: HostCapabilitiesV1;
};

const MAX_REASON_LENGTH = 128;
const MAX_CODEC_NAME_LENGTH = 32;
const MAX_CODEC_PROFILES = 16;
const MAX_DIMENSION = 16384;
const MAX_FPS = 1000;

function asRecord(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function finiteInteger(value: unknown, minimum: number, maximum: number, fallback = 0) {
  return typeof value === "number" && Number.isInteger(value) && value >= minimum && value <= maximum
    ? value
    : fallback;
}

function boundedText(value: unknown, maximum: number) {
  return typeof value === "string" && value.length <= maximum ? value : "";
}

function parseCapability<TMetadata extends CapabilityMetadata = CapabilityMetadata>(
  value: unknown,
): HostCapability<TMetadata> {
  const object = asRecord(value);
  if (!object || typeof object.available !== "boolean") return { available: false };

  const reason = boundedText(object.reason, MAX_REASON_LENGTH);
  const metadata = asRecord(object.metadata) as TMetadata | null;
  return {
    available: object.available,
    ...(reason ? { reason } : {}),
    ...(metadata ? { metadata } : {}),
  };
}

function parseResolution(value: unknown): ResolutionLimit {
  const object = asRecord(value);
  if (!object) return { width: 0, height: 0 };
  return {
    width: finiteInteger(object.width, 1, MAX_DIMENSION),
    height: finiteInteger(object.height, 1, MAX_DIMENSION),
  };
}

function parseCodecs(value: unknown): CodecCapability[] {
  if (!Array.isArray(value)) return [];
  const result: CodecCapability[] = [];
  for (const candidate of value.slice(0, 16)) {
    const object = asRecord(candidate);
    if (!object) continue;
    const codec = boundedText(object.codec, MAX_CODEC_NAME_LENGTH).toLowerCase();
    if (!codec) continue;
    const profiles = Array.isArray(object.profiles)
      ? object.profiles
        .filter((profile): profile is string => typeof profile === "string" && profile.length > 0 && profile.length <= 64)
        .slice(0, MAX_CODEC_PROFILES)
      : undefined;
    const maximumFps = finiteInteger(object.maximumFps, 1, MAX_FPS);
    const maximumResolution = parseResolution(object.maximumResolution);
    result.push({
      codec,
      ...(profiles?.length ? { profiles } : {}),
      ...(maximumFps ? { maximumFps } : {}),
      ...(maximumResolution.width && maximumResolution.height ? { maximumResolution } : {}),
    });
  }
  return result;
}

function parseNestedCapabilities(message: Record<string, unknown>): HostCapabilitiesV1 | null {
  const nested = asRecord(message.capabilities);
  if (!nested || nested.version !== 1) return null;

  return {
    version: 1,
    secureAttention: parseCapability<{ policy?: string }>(nested.secureAttention),
    clipboard: parseCapability(nested.clipboard),
    fileTransfer: parseCapability(nested.fileTransfer),
    systemAudio: parseCapability<{ backend?: string }>(nested.systemAudio),
    microphone: parseCapability<{ backend?: string }>(nested.microphone),
    protectedDesktop: parseCapability(nested.protectedDesktop),
    multiMonitor: parseCapability(nested.multiMonitor),
    highRefresh: parseCapability(nested.highRefresh),
    virtualDisplay: parseCapability(nested.virtualDisplay),
    privacyMode: parseCapability(nested.privacyMode),
    virtualHid: parseCapability(nested.virtualHid),
    gamepad: parseCapability(nested.gamepad),
    codecs: parseCodecs(nested.codecs),
    maximumFps: finiteInteger(nested.maximumFps, 1, MAX_FPS),
    maximumResolution: parseResolution(nested.maximumResolution),
  };
}

function parseLegacyCapabilities(message: Record<string, unknown>): HostCapabilitiesV1 | null {
  if (message.version !== 1) return null;

  const secureAttentionReason = boundedText(message.secureAttentionReason, MAX_REASON_LENGTH);
  const secureAttentionPolicy = boundedText(message.secureAttentionPolicy, MAX_REASON_LENGTH);
  return {
    version: 1,
    secureAttention: {
      available: message.secureAttentionAvailable === true,
      ...(secureAttentionReason ? { reason: secureAttentionReason } : {}),
      ...(secureAttentionPolicy ? { metadata: { policy: secureAttentionPolicy } } : {}),
    },
    clipboard: { available: message.clipboardAvailable === true },
    fileTransfer: { available: message.fileTransferAvailable === true },
    systemAudio: { available: message.audioAvailable === true },
    microphone: { available: false },
    protectedDesktop: { available: message.protectedDesktopAvailable === true },
    multiMonitor: { available: false, reason: "legacy-capability-not-advertised" },
    highRefresh: { available: false, reason: "legacy-capability-not-advertised" },
    virtualDisplay: { available: false },
    privacyMode: { available: false },
    virtualHid: { available: false },
    gamepad: { available: false },
    codecs: [],
    maximumFps: 0,
    maximumResolution: { width: 0, height: 0 },
  };
}

export function parseHostCapabilitiesMessage(value: unknown): HostCapabilitiesV1 | null {
  const message = asRecord(value);
  if (!message || message.t !== "host-capabilities" || message.version !== 1) return null;
  return parseNestedCapabilities(message) ?? parseLegacyCapabilities(message);
}

export function capabilityReason(capability: HostCapability | null | undefined) {
  return capability?.available ? "" : capability?.reason ?? "unavailable";
}
