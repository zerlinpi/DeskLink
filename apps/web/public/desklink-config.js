// DeskLink browser runtime configuration.
//
// This file is copied to dist/ without hashing so a deployment can change
// public endpoint settings without rebuilding the React/Vite bundle.
// `null` means "use the build-time value (if any), then the application default".
//
// SECURITY: keep credentials and secrets out of this file. In particular, do
// not add Access Codes, device credentials, controller secrets, Signal auth
// tokens, TURN REST shared secrets, or long-lived TURN usernames/passwords.
(() => {
  const config = {
    version: 1,
    signalUrl: null,
    controllerSessionUrl: null,
    stunUrl: null,
    turnUrl: null,
    turnTlsUrl: null,
    turnCredentialsUrl: null,
    features: {
      controllerAuthRequired: null,
      turnRuntimeRequired: null,
      forceRelay: null,
      lanFirstIce: null,
    },
  };

  globalThis.DESKLINK_CONFIG = Object.freeze({
    ...config,
    features: Object.freeze({ ...config.features }),
  });
})();
