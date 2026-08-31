import { describe, expect, it } from "vitest";
import {
  isConnectAction,
  shouldBlockConnectionInteraction,
  shouldBlockEnterConnect,
} from "./browser_preflight_policy";

describe("shouldBlockConnectionInteraction", () => {
  it("blocks connection interactions only when a required capability is missing", () => {
    expect(shouldBlockConnectionInteraction({
      blockingCount: 1,
      insideConnectCard: true,
      insideRecentDevices: false,
    })).toBe(true);

    expect(shouldBlockConnectionInteraction({
      blockingCount: 0,
      insideConnectCard: true,
      insideRecentDevices: false,
    })).toBe(false);
  });

  it("does not block unrelated UI or recent-device shortcuts", () => {
    expect(shouldBlockConnectionInteraction({
      blockingCount: 2,
      insideConnectCard: false,
      insideRecentDevices: false,
    })).toBe(false);

    expect(shouldBlockConnectionInteraction({
      blockingCount: 2,
      insideConnectCard: true,
      insideRecentDevices: true,
    })).toBe(false);
  });
});

describe("isConnectAction", () => {
  it("prefers the explicit connection action marker", () => {
    expect(isConnectAction("connect", "anything")).toBe(true);
    expect(isConnectAction("disconnect", "连接设备")).toBe(false);
  });

  it("supports the current English and Chinese button labels as fallback", () => {
    expect(isConnectAction(undefined, "Connect")).toBe(true);
    expect(isConnectAction(undefined, " 连接设备 ")).toBe(true);
    expect(isConnectAction(undefined, "断开连接")).toBe(false);
  });
});

describe("shouldBlockEnterConnect", () => {
  const blockedContext = {
    blockingCount: 1,
    insideConnectCard: true,
    insideRecentDevices: false,
  } as const;

  it("blocks Enter only when an actual primary connect action exists", () => {
    expect(shouldBlockEnterConnect(blockedContext, true)).toBe(true);
    expect(shouldBlockEnterConnect(blockedContext, false)).toBe(false);
  });

  it("does not block Enter when the preflight itself is non-blocking", () => {
    expect(shouldBlockEnterConnect({
      ...blockedContext,
      blockingCount: 0,
    }, true)).toBe(false);
  });
});
