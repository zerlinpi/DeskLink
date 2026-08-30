import { afterEach, describe, expect, it, vi } from "vitest";

import {
  requestControllerSession,
  resolveControllerSessionUrl,
} from "./controller_session";

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

function response(status: number, body: unknown = {}) {
  return {
    ok: status >= 200 && status < 300,
    status,
    json: async () => body,
  } as Response;
}

describe("controller session authentication", () => {
  it("derives HTTPS controller-session endpoint from WSS signaling", () => {
    vi.stubGlobal("window", { location: { href: "https://desklink.example/app/" } });
    expect(resolveControllerSessionUrl("wss://signal.example/ws?old=1#fragment", ""))
      .toBe("https://signal.example/api/v1/controller-session");
  });

  it("uses explicitly configured same-origin endpoints", () => {
    vi.stubGlobal("window", { location: { href: "https://desklink.example/app/" } });
    expect(resolveControllerSessionUrl("wss://ignored.example/ws", "/auth/controller"))
      .toBe("https://desklink.example/auth/controller");
  });

  it("sends credentials only in the POST body and accepts a healthy short-lived token", async () => {
    const fetchMock = vi.fn().mockResolvedValue(response(200, {
      token: "ct1.short-lived-token",
      expiresAt: 10_000,
    }));
    vi.stubGlobal("fetch", fetchMock);
    vi.spyOn(Date, "now").mockReturnValue(1_000_000);

    const session = await requestControllerSession("https://signal.example/api/v1/controller-session", {
      accountId: "controller-account",
      controllerId: "web-controller",
      targetDeviceId: "office-pc",
      accessKey: "secret-controller-key",
    });

    expect(session).toEqual({ token: "ct1.short-lived-token", expiresAt: 10_000 });
    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(url).toBe("https://signal.example/api/v1/controller-session");
    expect(init.method).toBe("POST");
    expect(init.cache).toBe("no-store");
    expect(init.credentials).toBe("omit");
    expect(String(init.body)).toContain("secret-controller-key");
    expect(url).not.toContain("secret-controller-key");
  });

  it.each([
    [401, "controller key rejected"],
    [403, "controller is not allowed to access this device"],
    [429, "too many controller authentication attempts"],
    [503, "controller session HTTP 503"],
  ])("maps HTTP %i to a stable authentication failure", async (status, message) => {
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(response(status)));
    await expect(requestControllerSession("https://signal.example/session", {
      accountId: "account",
      controllerId: "controller",
      targetDeviceId: "host",
      accessKey: "key",
    })).rejects.toThrow(message);
  });

  it("rejects incomplete or nearly-expired session tokens", async () => {
    vi.spyOn(Date, "now").mockReturnValue(1_000_000);
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response(200, { token: "", expiresAt: 5000 }))
      .mockResolvedValueOnce(response(200, { token: "ct1.expiring", expiresAt: 1030 }));
    vi.stubGlobal("fetch", fetchMock);

    const request = {
      accountId: "account",
      controllerId: "controller",
      targetDeviceId: "host",
      accessKey: "key",
    };
    await expect(requestControllerSession("https://signal.example/session", request))
      .rejects.toThrow("controller session response is incomplete");
    await expect(requestControllerSession("https://signal.example/session", request))
      .rejects.toThrow("controller session expires too soon");
  });
});
