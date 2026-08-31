from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "apps/windows-agent/src/webrtc_session.h"
SOURCE = ROOT / "apps/windows-agent/src/webrtc_session.cpp"

header = HEADER.read_text(encoding="utf-8")
source = SOURCE.read_text(encoding="utf-8")

requirements = {
    "header uses callback-ordering event bridge": (
        header,
        "RustCoreShadowEventBridge rust_core_shadow_event_bridge_;",
    ),
    "input channel binding carries peer shadow scope": (
        header,
        "RustCoreShadowPeerScope rust_shadow_scope",
    ),
    "peer authority begins a shadow scope": (
        source,
        "rust_core_shadow_event_bridge_.BeginPeer(",
    ),
    "peer connected authority is mirrored": (
        source,
        "rust_core_shadow_event_bridge_.ComparePeerConnected(",
    ),
    "control open authority is mirrored": (
        source,
        "rust_core_shadow_event_bridge_.CompareControlOpened(",
    ),
    "control close authority is mirrored": (
        source,
        "rust_core_shadow_event_bridge_.CompareControlClosed(",
    ),
    "pointer open authority is mirrored": (
        source,
        "rust_core_shadow_event_bridge_.ComparePointerOpened(",
    ),
    "pointer close authority is mirrored": (
        source,
        "rust_core_shadow_event_bridge_.ComparePointerClosed(",
    ),
    "session authority loss ends the shadow session": (
        source,
        "rust_core_shadow_event_bridge_.EndSession()",
    ),
    "data channel receives captured peer scope": (
        source,
        "AttachControlChannel(channel, rust_shadow_scope);",
    ),
}

missing = [name for name, (text, marker) in requirements.items() if marker not in text]
if missing:
    for name in missing:
        print(f"MISSING: {name}")
    raise SystemExit(1)

print("DeskLink Rust shadow production-integration source contract passed.")
