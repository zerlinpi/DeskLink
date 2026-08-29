const ACCESS_PROOF_ALGORITHM = "hmac-sha256-v1";
const NONCE_PATTERN = /^[0-9a-f]{64}$/;

function bytesToHex(bytes: Uint8Array) {
  let result = "";
  for (const value of bytes) result += value.toString(16).padStart(2, "0");
  return result;
}

export function accessProofAlgorithm() {
  return ACCESS_PROOF_ALGORITHM;
}

export async function createAccessProof(
  accessCode: string,
  controllerId: string,
  hostId: string,
  sessionId: string,
  nonce: string,
): Promise<string> {
  if (!accessCode || !controllerId || !hostId || !sessionId || !NONCE_PATTERN.test(nonce)) {
    throw new Error("invalid host access challenge");
  }
  if (!globalThis.crypto?.subtle) {
    throw new Error("Web Crypto is required for host access authentication");
  }

  const encoder = new TextEncoder();
  const key = await crypto.subtle.importKey(
    "raw",
    encoder.encode(accessCode),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  const message = [
    "DeskLink access proof v1",
    controllerId,
    hostId,
    sessionId,
    nonce,
  ].join("\n");
  const signature = await crypto.subtle.sign("HMAC", key, encoder.encode(message));
  return bytesToHex(new Uint8Array(signature));
}
