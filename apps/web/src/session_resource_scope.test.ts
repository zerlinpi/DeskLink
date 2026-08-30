import { describe, expect, it } from "vitest";
import { isActiveSessionResource } from "./session_resource_scope";

describe("isActiveSessionResource", () => {
  it("accepts the current resource while the session is active", () => {
    const resource = {};
    expect(isActiveSessionResource(resource, resource, false)).toBe(true);
  });

  it("rejects a stale resource replaced by a newer one", () => {
    const current = {};
    const stale = {};
    expect(isActiveSessionResource(current, stale, false)).toBe(false);
  });

  it("rejects callbacks after the current resource has been cleared", () => {
    expect(isActiveSessionResource(null, {}, false)).toBe(false);
  });

  it("rejects callbacks during a manual disconnect even for the current resource", () => {
    const resource = {};
    expect(isActiveSessionResource(resource, resource, true)).toBe(false);
  });
});
