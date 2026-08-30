import {
  requestControllerSession,
  type ControllerSession,
  type ControllerSessionRequest,
} from "./controller_session";

export type ControllerSessionRequester = (
  endpoint: string,
  request: ControllerSessionRequest,
  signal?: AbortSignal,
) => Promise<ControllerSession>;

export function createControllerSessionTokenRequest(
  endpoint: string,
  request: ControllerSessionRequest,
  requester: ControllerSessionRequester = requestControllerSession,
): (signal: AbortSignal) => Promise<ControllerSession> {
  return (signal) => requester(endpoint, request, signal);
}
