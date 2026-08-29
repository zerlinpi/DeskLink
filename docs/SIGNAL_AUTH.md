# Signaling device registration authentication

DeskLink's signaling server supports an optional HMAC registration gate controlled by `DESKLINK_SIGNAL_AUTH_SECRET`.

## Why it exists

Without a registration credential, anyone who knows or guesses a `deviceId` can attempt to register that ID. Because the hub intentionally keeps one live connection per device ID, an unauthenticated duplicate connection can otherwise displace the existing registration.

Development remains backward-compatible: when `DESKLINK_SIGNAL_AUTH_SECRET` is empty/unset, `/ws?deviceId=...` behaves exactly as before.

## Token format

When the secret is enabled, the WebSocket URL must also contain an `auth` query parameter:

```text
wss://control.example.com/ws?deviceId=office-pc&auth=<token>
```

The token format is:

```text
<unix-expiry>.<base64url-hmac-sha256>
```

The signature input is:

```text
deviceId + "\n" + unix-expiry
```

The HMAC key is `DESKLINK_SIGNAL_AUTH_SECRET`. Tokens must be unexpired and may not be valid for more than 24 hours from the signaling server's current clock.

## Provisioning model

Do **not** put `DESKLINK_SIGNAL_AUTH_SECRET` into a browser bundle or native client. The secret belongs only on trusted backend infrastructure.

The intended production flow is:

1. user/device authenticates to the product account/device service;
2. that trusted service mints a short-lived registration token for the exact `deviceId`;
3. the browser/native client connects to signaling with `deviceId` + token;
4. signaling validates the HMAC before upgrading the connection to WebSocket;
5. the raw shared secret never leaves trusted backend infrastructure.

The current repository contains token mint/validate primitives and server-side enforcement. The browser and Windows clients still need an authenticated provisioning API before production deployments should enable `DESKLINK_SIGNAL_AUTH_SECRET`.

## TURN credentials

`apps/signal/turn_credentials.go` separately implements coturn TURN REST temporary credential generation. It intentionally has no anonymous HTTP endpoint yet. TURN credentials should be issued only after the same account/device authentication boundary exists; otherwise a public credential endpoint would allow third parties to consume relay bandwidth.

## Deployment guidance

When client provisioning is implemented:

- generate a long random `DESKLINK_SIGNAL_AUTH_SECRET` and store it in a secret manager;
- rotate it deliberately; rotation invalidates outstanding registration tokens;
- keep token TTL short (minutes rather than the 24-hour maximum where practical);
- use HTTPS/WSS so query-string tokens are encrypted in transit;
- configure reverse-proxy access logs to avoid retaining full query strings containing `auth` tokens;
- keep signaling auth and host remote-control access codes as separate credentials with separate responsibilities.
