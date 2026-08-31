export function isActiveSessionResource<T>(
  current: T | null,
  source: T,
  manualDisconnect: boolean,
): boolean {
  return !manualDisconnect && current === source;
}

export function isActiveAsyncSessionResource<T>(
  current: T | null,
  source: T,
  manualDisconnect: boolean,
  callbackScopeCurrent: boolean,
): boolean {
  return callbackScopeCurrent &&
    isActiveSessionResource(current, source, manualDisconnect);
}
