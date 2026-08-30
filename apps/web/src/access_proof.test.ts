import { describe, expect, it } from "vitest";

import { accessProofAlgorithm, createAccessProof } from "./access_proof";

describe("Access Code proof", () => {
  it("matches the cross-language BCrypt/Web Crypto compatibility vector", async () => {
    expect(accessProofAlgorithm()).toBe("hmac-sha256-v1");
    await expect(createAccessProof(
      "DeskLink-Test-Access-Code-123!",
      "web-test-01",
      "win-test-01",
      "session-123",
      "0".repeat(64),
    )).resolves.toBe(
      "4bda9e0359353d0b47b5e7c22c24664318cbc42dfdcc16a09170e2fd1ea95214",
    );
  });

  it.each([
    ["", "controller", "host", "session", "0".repeat(64)],
    ["code", "", "host", "session", "0".repeat(64)],
    ["code", "controller", "", "session", "0".repeat(64)],
    ["code", "controller", "host", "", "0".repeat(64)],
    ["code", "controller", "host", "session", "not-a-nonce"],
    ["code", "controller", "host", "session", "A".repeat(64)],
  ])("rejects malformed host access challenges", async (code, controller, host, session, nonce) => {
    await expect(createAccessProof(code, controller, host, session, nonce))
      .rejects.toThrow("invalid host access challenge");
  });
});
