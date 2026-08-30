import type { HostCapabilitiesV1 } from "./host_capabilities";

export const SECURE_ATTENTION_OPERATION = "secure-attention-sequence";
export const SECURE_ATTENTION_TIMEOUT_MS = 6000;

export type SecureAttentionPhase =
  | "waiting-capabilities"
  | "unavailable"
  | "ready"
  | "pending"
  | "success"
  | "error";

export type SecureAttentionView = {
  phase: SecureAttentionPhase;
  enabled: boolean;
  label: string;
  reason: string;
  requestId: string;
};

export type SystemOperationRequest = {
  t: "system-operation";
  operation: typeof SECURE_ATTENTION_OPERATION;
  requestId: string;
};

export type SecureAttentionContext = {
  sessionActive: boolean;
  controlChannelOpen: boolean;
  capabilities: HostCapabilitiesV1 | null;
};

type PendingRequest = {
  requestId: string;
  startedAtMs: number;
};

function unavailableLabel(reason: string) {
  if (reason === "service-broker-unavailable") return "Service 不支持";
  if (reason === "policy-not-allowed") return "策略未允许";
  if (reason === "policy-read-error") return "策略不可读";
  if (reason === "api-unavailable") return "系统不支持";
  return "Ctrl+Alt+Del";
}

function resultLabel(errorCode: string, errorText: string) {
  const normalized = errorText.toLowerCase();
  if (errorCode === "service-broker-unavailable" || normalized.includes("broker is not enabled")) {
    return { label: "Service 不支持", reason: "service-broker-unavailable" };
  }
  if (errorCode === "policy-not-allowed" || normalized.includes("policy does not allow services")) {
    return { label: "策略未允许", reason: "policy-not-allowed" };
  }
  if (errorCode === "policy-read-error") {
    return { label: "策略不可读", reason: "policy-read-error" };
  }
  if (errorCode === "api-unavailable" || normalized.includes("api is unavailable")) {
    return { label: "系统不支持", reason: "api-unavailable" };
  }
  if (errorCode === "rate-limited" || normalized.includes("rate limited")) {
    return { label: "操作过快", reason: "rate-limited" };
  }
  return { label: "当前不可用", reason: errorCode || "operation-failed" };
}

function validRequestId(value: string) {
  return value.length >= 8 && value.length <= 128 && /^[A-Za-z0-9._:-]+$/.test(value);
}

export class SecureAttentionStateMachine {
  private context: SecureAttentionContext = {
    sessionActive: false,
    controlChannelOpen: false,
    capabilities: null,
  };
  private pending: PendingRequest | null = null;
  private feedback: { phase: "success" | "error"; label: string; reason: string } | null = null;

  setContext(context: Partial<SecureAttentionContext>) {
    this.context = { ...this.context, ...context };
    if (!this.context.sessionActive || !this.context.controlChannelOpen) {
      this.pending = null;
      this.feedback = null;
    }
  }

  reset(clearCapabilities = true) {
    this.pending = null;
    this.feedback = null;
    this.context.sessionActive = false;
    this.context.controlChannelOpen = false;
    if (clearCapabilities) this.context.capabilities = null;
  }

  request(requestId: string, nowMs: number): SystemOperationRequest | null {
    if (!validRequestId(requestId) || !Number.isFinite(nowMs)) return null;
    if (!this.canRequest() || this.pending) return null;
    this.feedback = null;
    this.pending = { requestId, startedAtMs: nowMs };
    return {
      t: "system-operation",
      operation: SECURE_ATTENTION_OPERATION,
      requestId,
    };
  }

  handleResult(value: unknown) {
    if (!this.pending || value === null || typeof value !== "object" || Array.isArray(value)) return false;
    const message = value as Record<string, unknown>;
    if (message.t !== "system-operation-result" ||
        message.operation !== SECURE_ATTENTION_OPERATION ||
        message.requestId !== this.pending.requestId) {
      return false;
    }

    this.pending = null;
    if (message.ok === true) {
      this.feedback = { phase: "success", label: "已发送", reason: "" };
      return true;
    }

    const errorCode = typeof message.errorCode === "string" && message.errorCode.length <= 128
      ? message.errorCode
      : "";
    const errorText = typeof message.error === "string" && message.error.length <= 512
      ? message.error
      : "";
    const mapped = resultLabel(errorCode, errorText);
    this.feedback = { phase: "error", ...mapped };
    return true;
  }

  expire(nowMs: number) {
    if (!this.pending || !Number.isFinite(nowMs)) return false;
    if (nowMs - this.pending.startedAtMs < SECURE_ATTENTION_TIMEOUT_MS) return false;
    this.pending = null;
    this.feedback = { phase: "error", label: "请求超时", reason: "timeout" };
    return true;
  }

  clearFeedback() {
    this.feedback = null;
  }

  view(): SecureAttentionView {
    if (this.pending) {
      return {
        phase: "pending",
        enabled: false,
        label: "请求中…",
        reason: "",
        requestId: this.pending.requestId,
      };
    }
    if (this.feedback) {
      return {
        phase: this.feedback.phase,
        enabled: false,
        label: this.feedback.label,
        reason: this.feedback.reason,
        requestId: "",
      };
    }

    const capability = this.context.capabilities?.secureAttention;
    if (!capability) {
      return {
        phase: "waiting-capabilities",
        enabled: false,
        label: "Ctrl+Alt+Del",
        reason: "capability-not-received",
        requestId: "",
      };
    }
    if (!capability.available) {
      const reason = capability.reason ?? "unavailable";
      return {
        phase: "unavailable",
        enabled: false,
        label: unavailableLabel(reason),
        reason,
        requestId: "",
      };
    }

    const enabled = this.context.sessionActive && this.context.controlChannelOpen;
    return {
      phase: "ready",
      enabled,
      label: "Ctrl+Alt+Del",
      reason: enabled ? "" : "session-or-channel-unavailable",
      requestId: "",
    };
  }

  private canRequest() {
    return this.context.sessionActive &&
      this.context.controlChannelOpen &&
      this.context.capabilities?.secureAttention.available === true;
  }
}
