import { webcrypto } from "node:crypto";

const cryptoApi = globalThis.crypto ?? webcrypto;
const encoder = new TextEncoder();
const accessCode = "DeskLink-Test-Access-Code-123!";
const nonce = "0".repeat(64);
const message = [
  "DeskLink access proof v1",
  "web-test-01",
  "win-test-01",
  "session-123",
  nonce,
].join("\n");
const expected = "4bda9e0359353d0b47b5e7c22c24664318cbc42dfdcc16a09170e2fd1ea95214";

const key = await cryptoApi.subtle.importKey(
  "raw",
  encoder.encode(accessCode),
  { name: "HMAC", hash: "SHA-256" },
  false,
  ["sign"],
);
const signature = new Uint8Array(
  await cryptoApi.subtle.sign("HMAC", key, encoder.encode(message)),
);
const actual = Array.from(signature, (value) => value.toString(16).padStart(2, "0")).join("");

if (actual !== expected) {
  console.error("Access proof Web Crypto vector mismatch");
  console.error("expected:", expected);
  console.error("actual:  ", actual);
  process.exit(1);
}

console.log("DeskLink access proof Web Crypto vector passed.");
