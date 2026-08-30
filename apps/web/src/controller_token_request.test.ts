import { describe, expect, it, vi } from "vitest";
import {
  createControllerSessionTokenRequest,
  type ControllerSessionRequester,
} from "./controller_token_request";

describe("createControllerSessionTokenRequest", () => {
  it("forwards the coordinator AbortSignal to the session requester", async () => {
    const requester: ControllerSessionRequester = vi.fn(async () => ({
      token: "token-a",
      expiresAt: 10_000,
    }));
    const payload = {
      accountId: "admin",
      controllerId: "web-1",
      targetDeviceId: "host-a",
      accessKey: "secret",
    };
    const controller = new AbortController();
    const request = createControllerSessionTokenRequest(
      "https://signal.example/api/v1/controller-session",
      payload,
      requester,
    );

    await expect(request(controller.signal)).resolves.toEqual({
      token: "token-a",
      expiresAt: 10_000,
    });
    expect(requester).toHaveBeenCalledWith(
      "https://signal.example/api/v1/controller-session",
      payload,
      controller.signal,
    );
  });
});
