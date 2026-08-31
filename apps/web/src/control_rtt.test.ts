import { describe, expect, it } from "vitest";
import {
  CONTROL_RTT_PROBE_TIMEOUT_MS,
  isValidControlRttRequestId,
  resolveControlRttAck,
  shouldIssueControlRttProbe,
} from "./control_rtt";

describe("control RTT probe policy", () => {
  it("allows a probe when none is pending and suppresses duplicates", () => {
    expect(shouldIssueControlRttProbe(null, 100)).toBe(true);
    expect(shouldIssueControlRttProbe({ requestId: "probe-1", sentAtMs: 100 }, 101)).toBe(false);
  });

  it("allows a replacement probe after the timeout", () => {
    expect(shouldIssueControlRttProbe(
      { requestId: "probe-1", sentAtMs: 100 },
      100 + CONTROL_RTT_PROBE_TIMEOUT_MS,
    )).toBe(true);
  });

  it("measures only the matching non-stale acknowledgement", () => {
    const pending = { requestId: "probe-1", sentAtMs: 100 };
    expect(resolveControlRttAck(pending, "probe-1", 142.5)).toBe(42.5);
    expect(resolveControlRttAck(pending, "probe-2", 142.5)).toBeNull();
    expect(resolveControlRttAck(pending, "probe-1", 99)).toBeNull();
    expect(resolveControlRttAck(
      pending,
      "probe-1",
      100 + CONTROL_RTT_PROBE_TIMEOUT_MS + 1,
    )).toBeNull();
  });

  it("bounds request IDs used by the echo protocol", () => {
    expect(isValidControlRttRequestId("abc")).toBe(true);
    expect(isValidControlRttRequestId("")).toBe(false);
    expect(isValidControlRttRequestId("x".repeat(129))).toBe(false);
    expect(isValidControlRttRequestId(123)).toBe(false);
  });
});
