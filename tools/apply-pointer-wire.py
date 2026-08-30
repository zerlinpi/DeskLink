from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one anchor, got {count}: {old[:100]!r}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "apps/web/src/main.tsx",
    'import { isActiveSessionResource } from "./session_resource_scope";\n',
    'import { isActiveSessionResource } from "./session_resource_scope";\n'
    'import { encodePointerMove, encodePointerWheel } from "./pointer_wire";\n',
)
replace_once(
    "apps/web/src/main.tsx",
    '  const sendPointerFast = (payload: object) => {\n'
    '    const channel = pointerRef.current;\n'
    '    if (channel?.readyState === "open") channel.send(JSON.stringify(payload));\n'
    '  };',
    '  const sendPointerFast = (payload: ArrayBuffer) => {\n'
    '    const channel = pointerRef.current;\n'
    '    if (channel?.readyState === "open") channel.send(payload);\n'
    '  };',
)
replace_once(
    "apps/web/src/main.tsx",
    "      if (latest) sendPointerFast(latest);",
    "      if (latest) sendPointerFast(encodePointerMove(latest.x, latest.y));",
)
replace_once(
    "apps/web/src/main.tsx",
    '            sendPointerFast({ t: "wheel", delta: (e.deltaY < 0 ? 120 : -120) * magnitude });',
    '            sendPointerFast(encodePointerWheel((e.deltaY < 0 ? 120 : -120) * magnitude));',
)

replace_once(
    "apps/windows-agent/src/webrtc_session.cpp",
    '#include "file_transfer_receiver.h"\n',
    '#include "file_transfer_receiver.h"\n#include "pointer_wire.h"\n',
)
replace_once(
    "apps/windows-agent/src/webrtc_session.cpp",
    '''  channel->onMessage([this](rtc::message_variant data) {
    if (const auto* text = std::get_if<std::string>(&data)) {
      HandleControl(*text);
    }
  });''',
    '''  channel->onMessage([this, label](rtc::message_variant data) {
    if (label == "pointer") {
      PointerWireEvent event;
      bool parsed = false;
      if (const auto* binary = std::get_if<rtc::binary>(&data)) {
        parsed = ParsePointerWire(
            std::span<const std::byte>(binary->data(), binary->size()),
            &event);
      } else if (const auto* text = std::get_if<std::string>(&data)) {
        parsed = ParseLegacyPointerJson(*text, &event);
      }
      if (!parsed) return;

      if (event.kind == PointerWireKind::Move) {
        input_.PointerMove(event.x, event.y);
      } else if (event.kind == PointerWireKind::Wheel) {
        input_.PointerWheel(event.wheel_delta);
      }
      return;
    }

    if (const auto* text = std::get_if<std::string>(&data)) {
      HandleControl(*text);
    }
  });''',
)

replace_once(
    "apps/windows-agent/CMakeLists.txt",
    "  src/input_injector.cpp\n  src/input_injector.h\n  src/performance_tuning.cpp\n",
    "  src/input_injector.cpp\n  src/input_injector.h\n  src/pointer_wire.cpp\n  src/pointer_wire.h\n  src/performance_tuning.cpp\n",
)
anchor = '''add_executable(desklink-video-policy-smoke
  src/video_policy_smoke.cpp
  src/video_policy.cpp
  src/video_policy.h
)
target_compile_definitions(desklink-video-policy-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-video-policy-smoke PRIVATE /W4 /permissive-)
'''
replace_once(
    "apps/windows-agent/CMakeLists.txt",
    anchor,
    anchor
    + '''
add_executable(desklink-pointer-wire-smoke
  src/pointer_wire_smoke.cpp
  src/pointer_wire.cpp
  src/pointer_wire.h
)
target_compile_definitions(desklink-pointer-wire-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(desklink-pointer-wire-smoke PRIVATE /W4 /permissive-)
target_link_libraries(desklink-pointer-wire-smoke PRIVATE nlohmann_json::nlohmann_json)
''',
)
