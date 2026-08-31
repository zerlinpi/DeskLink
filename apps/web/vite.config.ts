import { defineConfig, loadEnv } from "vite";
import react from "@vitejs/plugin-react";

function literal(value: string | undefined) {
  return value === undefined ? "undefined" : JSON.stringify(value);
}

function runtimeString(property: string, buildValue: string | undefined) {
  const runtime = `globalThis.DESKLINK_CONFIG?.${property}`;
  return `(typeof ${runtime} === "string" ? ${runtime} : ${literal(buildValue)})`;
}

function runtimeBooleanFlag(
  property: string,
  buildValue: string | undefined,
  trueValue: string,
  falseValue: string,
) {
  const runtime = `globalThis.DESKLINK_CONFIG?.features?.${property}`;
  return `(typeof ${runtime} === "boolean" ? (${runtime} ? ${JSON.stringify(trueValue)} : ${JSON.stringify(falseValue)}) : ${literal(buildValue)})`;
}

function runtimeConfigBridge(env: Record<string, string>) {
  const replacements = new Map<string, string>([
    ["import.meta.env.VITE_SIGNAL_URL", runtimeString("signalUrl", env.VITE_SIGNAL_URL)],
    [
      "import.meta.env.VITE_CONTROLLER_SESSION_URL",
      runtimeString("controllerSessionUrl", env.VITE_CONTROLLER_SESSION_URL),
    ],
    ["import.meta.env.VITE_STUN_URL", runtimeString("stunUrl", env.VITE_STUN_URL)],
    ["import.meta.env.VITE_TURN_URL", runtimeString("turnUrl", env.VITE_TURN_URL)],
    ["import.meta.env.VITE_TURN_TLS_URL", runtimeString("turnTlsUrl", env.VITE_TURN_TLS_URL)],
    [
      "import.meta.env.VITE_TURN_CREDENTIALS_URL",
      runtimeString("turnCredentialsUrl", env.VITE_TURN_CREDENTIALS_URL),
    ],
    [
      "import.meta.env.VITE_CONTROLLER_AUTH_REQUIRED",
      runtimeBooleanFlag("controllerAuthRequired", env.VITE_CONTROLLER_AUTH_REQUIRED, "1", "0"),
    ],
    [
      "import.meta.env.VITE_TURN_RUNTIME_REQUIRED",
      runtimeBooleanFlag("turnRuntimeRequired", env.VITE_TURN_RUNTIME_REQUIRED, "1", "0"),
    ],
    [
      "import.meta.env.VITE_ICE_TRANSPORT_POLICY",
      runtimeBooleanFlag("forceRelay", env.VITE_ICE_TRANSPORT_POLICY, "relay", "all"),
    ],
    [
      "import.meta.env.VITE_LAN_FIRST_ICE",
      runtimeBooleanFlag("lanFirstIce", env.VITE_LAN_FIRST_ICE, "1", "0"),
    ],
  ]);

  return {
    name: "desklink-runtime-config",
    enforce: "pre" as const,
    transform(code: string, id: string) {
      if (!id.includes("/src/")) return null;

      let transformed = code;
      for (const [source, replacement] of replacements) {
        transformed = transformed.split(source).join(replacement);
      }
      if (transformed === code) return null;
      return { code: transformed, map: null };
    },
  };
}

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), "VITE_");

  return {
    plugins: [runtimeConfigBridge(env), react()],
  };
});
