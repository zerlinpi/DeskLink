import { describe, expect, it } from "vitest";
import {
  isActiveAsyncSessionResource,
  isActiveSessionResource,
} from "./session_resource_scope";

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


describe("isActiveAsyncSessionResource", () => {
  it("commits an async completion only while callback and resource scopes are current", () => {
    const resource = {};
    expect(isActiveAsyncSessionResource(resource, resource, false, true)).toBe(true);
  });

  it("rejects a late completion after socket/session scope changed", () => {
    const resource = {};
    expect(isActiveAsyncSessionResource(resource, resource, false, false)).toBe(false);
  });

  it("rejects a late completion from a replaced peer", () => {
    expect(isActiveAsyncSessionResource({}, {}, false, true)).toBe(false);
  });

  it("rejects a late completion during manual disconnect", () => {
    const resource = {};
    expect(isActiveAsyncSessionResource(resource, resource, true, true)).toBe(false);
  });
});
