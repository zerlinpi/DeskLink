import { describe, expect, it } from "vitest";
import {
  POINTER_MOVE_BUFFER_BUDGET_BYTES,
  shouldDeferPointerMove,
} from "./pointer_transport";

describe("pointer move backpressure policy", () => {
  it("allows a small healthy sender queue", () => {
    expect(shouldDeferPointerMove(0)).toBe(false);
    expect(shouldDeferPointerMove(7)).toBe(false);
    expect(shouldDeferPointerMove(POINTER_MOVE_BUFFER_BUDGET_BYTES)).toBe(false);
  });

  it("defers move packets once the low-latency budget is exceeded", () => {
    expect(shouldDeferPointerMove(POINTER_MOVE_BUFFER_BUDGET_BYTES + 1)).toBe(true);
    expect(shouldDeferPointerMove(128)).toBe(true);
  });

  it("fails closed for invalid buffered amounts", () => {
    expect(shouldDeferPointerMove(Number.NaN)).toBe(true);
    expect(shouldDeferPointerMove(Number.POSITIVE_INFINITY)).toBe(true);
  });

  it("normalizes custom budgets without allowing negative thresholds", () => {
    expect(shouldDeferPointerMove(1, 0)).toBe(true);
    expect(shouldDeferPointerMove(0, -50)).toBe(false);
    expect(shouldDeferPointerMove(17, Number.NaN)).toBe(true);
  });
});
