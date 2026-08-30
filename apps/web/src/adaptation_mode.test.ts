import { describe, expect, it } from "vitest";
import {
  DEFAULT_ADAPTATION_MODE,
  adaptationModeMessage,
  isAdaptationMode,
} from "./adaptation_mode";

describe("adaptation mode control protocol", () => {
  it("defaults to desktop mode for compatibility", () => {
    expect(DEFAULT_ADAPTATION_MODE).toBe("desktop");
  });

  it("accepts only supported mode identifiers", () => {
    expect(isAdaptationMode("desktop")).toBe(true);
    expect(isAdaptationMode("game")).toBe(true);
    expect(isAdaptationMode("auto")).toBe(false);
    expect(isAdaptationMode(1)).toBe(false);
  });

  it("encodes the reliable control message expected by the Windows host", () => {
    expect(adaptationModeMessage("game")).toEqual({
      t: "adaptation-mode",
      mode: "game",
    });
  });
});
