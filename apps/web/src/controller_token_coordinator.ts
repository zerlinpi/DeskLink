import type { ControllerSession } from "./controller_session";

type CachedControllerToken = ControllerSession & {
  target: string;
};

type InFlightControllerToken = {
  id: symbol;
  target: string;
  promise: Promise<string>;
};

export type ControllerTokenRequestOptions = {
  forceRefresh?: boolean;
  nowSeconds: number;
  refreshMarginSeconds: number;
  request: () => Promise<ControllerSession>;
};

export class ControllerTokenCoordinator {
  private cached: CachedControllerToken | null = null;
  private inFlight: InFlightControllerToken | null = null;

  clear(): void {
    this.cached = null;
    this.inFlight = null;
  }

  async getToken(target: string, options: ControllerTokenRequestOptions): Promise<string> {
    const normalizedTarget = target.trim();
    if (!normalizedTarget) throw new Error("controller token target is required");

    if (!options.forceRefresh &&
        this.cached?.target === normalizedTarget &&
        this.cached.expiresAt > options.nowSeconds + options.refreshMarginSeconds) {
      return this.cached.token;
    }

    if (this.inFlight?.target === normalizedTarget) return this.inFlight.promise;

    const requestId = Symbol(normalizedTarget);
    const promise = options.request().then((session) => {
      if (this.inFlight?.id === requestId) {
        this.cached = {
          target: normalizedTarget,
          token: session.token,
          expiresAt: session.expiresAt,
        };
      }
      return session.token;
    }).finally(() => {
      if (this.inFlight?.id === requestId) this.inFlight = null;
    });

    this.inFlight = {
      id: requestId,
      target: normalizedTarget,
      promise,
    };
    return promise;
  }
}
