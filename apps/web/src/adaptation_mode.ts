export type AdaptationMode = "desktop" | "game";

export const DEFAULT_ADAPTATION_MODE: AdaptationMode = "desktop";

export function isAdaptationMode(value: unknown): value is AdaptationMode {
  return value === "desktop" || value === "game";
}

export function adaptationModeMessage(mode: AdaptationMode) {
  return { t: "adaptation-mode" as const, mode };
}
