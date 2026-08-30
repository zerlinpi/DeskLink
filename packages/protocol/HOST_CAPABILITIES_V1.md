# HostCapabilitiesV1

`host-capabilities` is an authenticated control-channel message. It advertises what the current host/session can actually provide so controllers can hide or gate unsupported operations instead of probing privileged actions blindly.

The machine-readable schema is `host-capabilities-v1.schema.json`.

## Envelope

```json
{
  "t": "host-capabilities",
  "version": 1,
  "capabilities": {
    "version": 1,
    "secureAttention": {
      "available": false,
      "reason": "policy-not-allowed",
      "metadata": { "policy": "disabled" }
    },
    "clipboard": { "available": true },
    "fileTransfer": { "available": true },
    "systemAudio": { "available": false, "reason": "not-implemented" },
    "microphone": { "available": false, "reason": "not-implemented" },
    "protectedDesktop": { "available": false, "reason": "not-implemented" },
    "multiMonitor": { "available": true },
    "highRefresh": { "available": true },
    "virtualDisplay": { "available": false, "reason": "not-implemented" },
    "privacyMode": { "available": false, "reason": "not-implemented" },
    "virtualHid": { "available": false, "reason": "not-implemented" },
    "gamepad": { "available": false, "reason": "not-implemented" },
    "codecs": [
      {
        "codec": "h264",
        "maximumFps": 144,
        "maximumResolution": { "width": 3840, "height": 2160 }
      }
    ],
    "maximumFps": 144,
    "maximumResolution": { "width": 3840, "height": 2160 }
  }
}
```

## Capability semantics

Every feature capability uses the same base shape:

```json
{
  "available": false,
  "reason": "stable-machine-readable-reason",
  "metadata": {}
}
```

`available` is authoritative for the current host/session. Controllers must not infer support from product version, operating system version, GPU vendor, or the presence of a UI button.

`reason` is optional and should be a short stable machine-readable value. Human UI text is controller-localized and must not depend on parsing arbitrary Service error strings.

`metadata` is optional bounded feature-specific information. It must never contain Access Codes, device credentials, controller keys, TURN credentials, long-lived tokens, filesystem paths, shell commands, registry commands, or other secrets/privileged command material.

## Required V1 capabilities

V1 has explicit entries for:

- `secureAttention`
- `clipboard`
- `fileTransfer`
- `systemAudio`
- `microphone`
- `protectedDesktop`
- `multiMonitor`
- `highRefresh`
- `virtualDisplay`
- `privacyMode`
- `virtualHid`
- `gamepad`
- `codecs`
- `maximumFps`
- `maximumResolution`

A host must advertise unsupported/planned features as `available:false`; it must not advertise a planned capability as available merely because the protocol field exists.

## Compatibility

The envelope version and nested capability version are both `1`. A V1 controller must reject unsupported future envelope versions rather than treating unknown content as trusted capability state.

During migration, the Web controller may accept the existing flat V1 message (`secureAttentionAvailable`, `clipboardAvailable`, etc.) and conservatively map it into `HostCapabilitiesV1`. Features absent from the legacy message remain unavailable instead of being guessed.

New senders should emit only the nested `capabilities` representation. Once supported deployed clients no longer require the legacy form, the flat representation can be removed in a later protocol version/migration.

Unknown metadata fields inside a known capability are advisory only. They cannot grant privilege; `available` plus the host-side authorization/policy checks remain the actual enforcement boundary.

## Publication lifecycle

Host capability publication is independent from monitor enumeration. The target architecture uses a dedicated `SendHostCapabilities()` path and publishes on:

1. authenticated reliable control channel open;
2. capability changes (for example audio device/backend or policy changes);
3. device/capture capability changes;
4. remote session changes/reconnect.

A controller must clear cached capability state when the control channel/session closes and wait for a fresh advertisement after reconnect. Stale capabilities from a previous session must not authorize a system operation.

## Secure Attention

Typical stable reasons include:

- `service-broker-unavailable`
- `api-unavailable`
- `policy-read-error`
- `policy-not-allowed`
- `rate-limited` (operation result, not normally a persistent capability reason)

Capability advertisement does not execute SAS. The actual `system-operation` request still goes through the PID/SID-bound LocalSystem broker, policy preflight, operation whitelist, request ID checks and rate limiting.
