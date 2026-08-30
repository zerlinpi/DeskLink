export type ControllerSession = {
  token: string;
  expiresAt: number;
};

export type ControllerSessionRequest = {
  accountId: string;
  controllerId: string;
  targetDeviceId: string;
  accessKey: string;
};

export function resolveControllerSessionUrl(signalUrl: string, configuredUrl: string): string {
  if (configuredUrl) return new URL(configuredUrl, window.location.href).toString();

  const signal = new URL(signalUrl, window.location.href);
  if (signal.protocol === "ws:") signal.protocol = "http:";
  if (signal.protocol === "wss:") signal.protocol = "https:";
  signal.pathname = "/api/v1/controller-session";
  signal.search = "";
  signal.hash = "";
  return signal.toString();
}

export async function requestControllerSession(
  endpoint: string,
  request: ControllerSessionRequest,
  signal?: AbortSignal,
): Promise<ControllerSession> {
  const response = await fetch(endpoint, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify(request),
    cache: "no-store",
    credentials: "omit",
    signal,
  });
  if (!response.ok) {
    if (response.status === 401) throw new Error("controller key rejected");
    if (response.status === 403) throw new Error("controller is not allowed to access this device");
    if (response.status === 429) throw new Error("too many controller authentication attempts");
    throw new Error(`controller session HTTP ${response.status}`);
  }

  const session = await response.json() as ControllerSession;
  if (!session.token || !Number.isFinite(session.expiresAt)) {
    throw new Error("controller session response is incomplete");
  }
  const now = Math.floor(Date.now() / 1000);
  if (session.expiresAt <= now + 30) {
    throw new Error("controller session expires too soon");
  }
  return session;
}
