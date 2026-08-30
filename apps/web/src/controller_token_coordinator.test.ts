import { describe, expect, it } from "vitest";
import { ControllerTokenCoordinator } from "./controller_token_coordinator";

type Deferred<T> = {
  promise: Promise<T>;
  resolve: (value: T) => void;
};

type SignalCapture = {
  value?: AbortSignal;
};

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((next) => {
    resolve = next;
  });
  return { promise, resolve };
}

const nowSeconds = 1_000;
const refreshMarginSeconds = 90;

describe("ControllerTokenCoordinator", () => {
  it("reuses a fresh cached token for the same target", async () => {
    const coordinator = new ControllerTokenCoordinator();
    let requests = 0;
    const request = async () => {
      requests += 1;
      return { token: "token-a", expiresAt: nowSeconds + 600 };
    };

    await expect(coordinator.getToken("host-a", {
      nowSeconds,
      refreshMarginSeconds,
      request,
    })).resolves.toBe("token-a");
    await expect(coordinator.getToken("host-a", {
      nowSeconds: nowSeconds + 10,
      refreshMarginSeconds,
      request,
    })).resolves.toBe("token-a");
    expect(requests).toBe(1);
  });

  it("single-flights concurrent requests for the same target", async () => {
    const coordinator = new ControllerTokenCoordinator();
    const pending = deferred<{ token: string; expiresAt: number }>();
    let requests = 0;
    const firstSignal: SignalCapture = {};
    const request = (signal: AbortSignal) => {
      requests += 1;
      firstSignal.value = signal;
      return pending.promise;
    };

    const first = coordinator.getToken("host-a", {
      nowSeconds,
      refreshMarginSeconds,
      request,
    });
    const second = coordinator.getToken("host-a", {
      nowSeconds,
      refreshMarginSeconds,
      request,
    });

    expect(requests).toBe(1);
    expect(firstSignal.value?.aborted).toBe(false);
    pending.resolve({ token: "token-a", expiresAt: nowSeconds + 600 });
    await expect(first).resolves.toBe("token-a");
    await expect(second).resolves.toBe("token-a");
  });

  it("cancels an in-flight token request when the target changes", async () => {
    const coordinator = new ControllerTokenCoordinator();
    const oldTarget = deferred<{ token: string; expiresAt: number }>();
    const newTarget = deferred<{ token: string; expiresAt: number }>();
    const oldSignal: SignalCapture = {};
    const newSignal: SignalCapture = {};
    let newRequests = 0;

    const oldPromise = coordinator.getToken("host-a", {
      nowSeconds,
      refreshMarginSeconds,
      request: (signal) => {
        oldSignal.value = signal;
        return oldTarget.promise;
      },
    });
    const newPromise = coordinator.getToken("host-b", {
      nowSeconds,
      refreshMarginSeconds,
      request: (signal) => {
        newSignal.value = signal;
        newRequests += 1;
        return newTarget.promise;
      },
    });

    expect(oldSignal.value?.aborted).toBe(true);
    expect(newSignal.value?.aborted).toBe(false);
    expect(newRequests).toBe(1);

    newTarget.resolve({ token: "token-b", expiresAt: nowSeconds + 600 });
    await expect(newPromise).resolves.toBe("token-b");
    oldTarget.resolve({ token: "token-a", expiresAt: nowSeconds + 600 });
    await expect(oldPromise).resolves.toBe("token-a");

    await expect(coordinator.getToken("host-b", {
      nowSeconds: nowSeconds + 10,
      refreshMarginSeconds,
      request: async () => {
        newRequests += 1;
        return { token: "unexpected", expiresAt: nowSeconds + 600 };
      },
    })).resolves.toBe("token-b");
    expect(newRequests).toBe(1);
  });

  it("cancels and invalidates a request when the controller session is cleared", async () => {
    const coordinator = new ControllerTokenCoordinator();
    const stale = deferred<{ token: string; expiresAt: number }>();
    const staleSignal: SignalCapture = {};

    const stalePromise = coordinator.getToken("host-a", {
      nowSeconds,
      refreshMarginSeconds,
      request: (signal) => {
        staleSignal.value = signal;
        return stale.promise;
      },
    });
    coordinator.clear();
    expect(staleSignal.value?.aborted).toBe(true);

    stale.resolve({ token: "stale-token", expiresAt: nowSeconds + 600 });
    await expect(stalePromise).resolves.toBe("stale-token");

    let freshRequests = 0;
    await expect(coordinator.getToken("host-a", {
      nowSeconds: nowSeconds + 1,
      refreshMarginSeconds,
      request: async () => {
        freshRequests += 1;
        return { token: "fresh-token", expiresAt: nowSeconds + 600 };
      },
    })).resolves.toBe("fresh-token");
    expect(freshRequests).toBe(1);
  });

  it("clears failed requests so the same target can retry", async () => {
    const coordinator = new ControllerTokenCoordinator();
    await expect(coordinator.getToken("host-a", {
      nowSeconds,
      refreshMarginSeconds,
      request: async () => {
        throw new Error("temporary auth outage");
      },
    })).rejects.toThrow("temporary auth outage");

    await expect(coordinator.getToken("host-a", {
      nowSeconds: nowSeconds + 1,
      refreshMarginSeconds,
      request: async () => ({ token: "recovered", expiresAt: nowSeconds + 600 }),
    })).resolves.toBe("recovered");
  });

  it("refreshes expired or explicitly refreshed tokens", async () => {
    const coordinator = new ControllerTokenCoordinator();
    let requests = 0;
    const request = async () => {
      requests += 1;
      return { token: `token-${requests}`, expiresAt: nowSeconds + 600 };
    };

    await expect(coordinator.getToken("host-a", {
      nowSeconds,
      refreshMarginSeconds,
      request,
    })).resolves.toBe("token-1");
    await expect(coordinator.getToken("host-a", {
      nowSeconds: nowSeconds + 550,
      refreshMarginSeconds,
      request,
    })).resolves.toBe("token-2");
    await expect(coordinator.getToken("host-a", {
      forceRefresh: true,
      nowSeconds: nowSeconds + 551,
      refreshMarginSeconds,
      request,
    })).resolves.toBe("token-3");
  });
});
