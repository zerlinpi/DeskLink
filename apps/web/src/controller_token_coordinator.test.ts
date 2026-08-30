import { describe, expect, it } from "vitest";
import { ControllerTokenCoordinator } from "./controller_token_coordinator";

type Deferred<T> = {
  promise: Promise<T>;
  resolve: (value: T) => void;
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
    const request = () => {
      requests += 1;
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
    pending.resolve({ token: "token-a", expiresAt: nowSeconds + 600 });
    await expect(first).resolves.toBe("token-a");
    await expect(second).resolves.toBe("token-a");
  });

  it("does not reuse an in-flight token request for a different target", async () => {
    const coordinator = new ControllerTokenCoordinator();
    const oldTarget = deferred<{ token: string; expiresAt: number }>();
    const newTarget = deferred<{ token: string; expiresAt: number }>();
    let oldRequests = 0;
    let newRequests = 0;

    const oldPromise = coordinator.getToken("host-a", {
      nowSeconds,
      refreshMarginSeconds,
      request: () => {
        oldRequests += 1;
        return oldTarget.promise;
      },
    });
    const newPromise = coordinator.getToken("host-b", {
      nowSeconds,
      refreshMarginSeconds,
      request: () => {
        newRequests += 1;
        return newTarget.promise;
      },
    });

    expect(oldRequests).toBe(1);
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

  it("prevents a cleared stale request from repopulating the cache", async () => {
    const coordinator = new ControllerTokenCoordinator();
    const stale = deferred<{ token: string; expiresAt: number }>();

    const stalePromise = coordinator.getToken("host-a", {
      nowSeconds,
      refreshMarginSeconds,
      request: () => stale.promise,
    });
    coordinator.clear();

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
