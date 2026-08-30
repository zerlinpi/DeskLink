import { describe, expect, it } from "vitest";
import { AsyncAttemptCoordinator } from "./async_attempt";

describe("AsyncAttemptCoordinator", () => {
  it("allows only one active attempt", () => {
    const attempts = new AsyncAttemptCoordinator();
    const first = attempts.begin("first");
    expect(first).not.toBeNull();
    expect(attempts.begin("second")).toBeNull();
    expect(first && attempts.isCurrent(first)).toBe(true);
  });

  it("lets an invalidated lifecycle start a new attempt", () => {
    const attempts = new AsyncAttemptCoordinator();
    const first = attempts.begin("old");
    expect(first).not.toBeNull();
    attempts.invalidate();
    const second = attempts.begin("new");
    expect(second).not.toBeNull();
    expect(first && attempts.isCurrent(first)).toBe(false);
    expect(second && attempts.isCurrent(second)).toBe(true);
  });

  it("does not let an old finally clear a newer attempt", () => {
    const attempts = new AsyncAttemptCoordinator();
    const oldAttempt = attempts.begin("old");
    if (!oldAttempt) throw new Error("old attempt was not created");
    attempts.invalidate();
    const newAttempt = attempts.begin("new");
    if (!newAttempt) throw new Error("new attempt was not created");

    attempts.finish(oldAttempt);
    expect(attempts.isCurrent(newAttempt)).toBe(true);
    attempts.finish(newAttempt);
    expect(attempts.begin("after-finish")).not.toBeNull();
  });
});
