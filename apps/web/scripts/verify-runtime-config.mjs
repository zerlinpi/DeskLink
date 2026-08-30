import { readFile, readdir } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import path from "node:path";

const webRoot = fileURLToPath(new URL("../", import.meta.url));
const distRoot = path.join(webRoot, "dist");
const htmlPath = path.join(distRoot, "index.html");
const configPath = path.join(distRoot, "desklink-config.js");

function fail(message) {
  console.error(`runtime config verification failed: ${message}`);
  process.exitCode = 1;
}

const [html, configSource] = await Promise.all([
  readFile(htmlPath, "utf8"),
  readFile(configPath, "utf8"),
]);

const configIndex = html.indexOf("desklink-config.js");
const firstModuleIndex = html.indexOf('type="module"');
if (configIndex < 0) {
  fail("dist/index.html does not load desklink-config.js");
} else if (firstModuleIndex >= 0 && configIndex > firstModuleIndex) {
  fail("desklink-config.js must load before the first module script");
}

const forbiddenRuntimeKeys = [
  "accessCode",
  "deviceCredential",
  "controllerSecret",
  "signalAuthToken",
  "turnRestSecret",
  "turnSharedSecret",
  "turnUsername",
  "turnPassword",
];
for (const key of forbiddenRuntimeKeys) {
  if (new RegExp(`\\b${key}\\b`, "i").test(configSource)) {
    fail(`public runtime config exposes forbidden secret field ${key}`);
  }
}

for (const required of [
  "signalUrl",
  "controllerSessionUrl",
  "stunUrl",
  "turnUrl",
  "turnTlsUrl",
  "turnCredentialsUrl",
  "controllerAuthRequired",
  "turnRuntimeRequired",
  "forceRelay",
]) {
  if (!configSource.includes(required)) {
    fail(`desklink-config.js is missing public field ${required}`);
  }
}

const assetsDir = path.join(distRoot, "assets");
const assetNames = await readdir(assetsDir);
const javascriptAssets = assetNames.filter((name) => name.endsWith(".js"));
const bundleSource = (
  await Promise.all(javascriptAssets.map((name) => readFile(path.join(assetsDir, name), "utf8")))
).join("\n");

if (!bundleSource.includes("DESKLINK_CONFIG")) {
  fail("production bundle no longer references the runtime configuration object");
}
for (const required of ["signalUrl", "controllerSessionUrl", "turnCredentialsUrl", "forceRelay"]) {
  if (!bundleSource.includes(required)) {
    fail(`production bundle is not wired to runtime field ${required}`);
  }
}

if (!process.exitCode) {
  console.log("DeskLink runtime config dist contract OK");
}
