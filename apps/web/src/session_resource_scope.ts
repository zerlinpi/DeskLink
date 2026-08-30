export function isActiveSessionResource<T>(
  current: T | null,
  source: T,
  manualDisconnect: boolean,
): boolean {
  return !manualDisconnect && current === source;
}
