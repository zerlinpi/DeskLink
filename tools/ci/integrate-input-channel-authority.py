from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, got {count}")
    return text.replace(old, new, 1)


def replace_in_section(
    text: str,
    start_anchor: str,
    end_anchor: str,
    old: str,
    new: str,
    label: str,
) -> str:
    start = text.index(start_anchor)
    end = text.index(end_anchor, start)
    section = text[start:end]
    section = replace_once(section, old, new, label)
    return text[:start] + section + text[end:]


cmake_path = Path("apps/windows-agent/CMakeLists.txt")
cmake = cmake_path.read_text(encoding="utf-8")
cmake = replace_once(
    cmake,
    "  src/input_injector.cpp\n  src/input_injector.h\n",
    "  src/input_channel_authority.h\n  src/input_injector.cpp\n  src/input_injector.h\n",
    "agent authority header",
)
cmake = replace_once(
    cmake,
    "add_executable(desklink-pointer-wire-smoke\n"
    "  src/pointer_wire_smoke.cpp\n"
    "  src/pointer_wire.cpp\n"
    "  src/pointer_wire.h\n"
    ")\n"
    "target_compile_definitions(desklink-pointer-wire-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)\n"
    "target_compile_options(desklink-pointer-wire-smoke PRIVATE /W4 /permissive-)\n"
    "target_link_libraries(desklink-pointer-wire-smoke PRIVATE nlohmann_json::nlohmann_json)\n\n",
    "add_executable(desklink-pointer-wire-smoke\n"
    "  src/pointer_wire_smoke.cpp\n"
    "  src/pointer_wire.cpp\n"
    "  src/pointer_wire.h\n"
    ")\n"
    "target_compile_definitions(desklink-pointer-wire-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)\n"
    "target_compile_options(desklink-pointer-wire-smoke PRIVATE /W4 /permissive-)\n"
    "target_link_libraries(desklink-pointer-wire-smoke PRIVATE nlohmann_json::nlohmann_json)\n\n"
    "add_executable(desklink-input-channel-authority-smoke\n"
    "  src/input_channel_authority_smoke.cpp\n"
    "  src/input_channel_authority.h\n"
    ")\n"
    "target_compile_definitions(desklink-input-channel-authority-smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)\n"
    "target_compile_options(desklink-input-channel-authority-smoke PRIVATE /W4 /permissive-)\n\n",
    "authority smoke target",
)
cmake_path.write_text(cmake, encoding="utf-8")

cpp_path = Path("apps/windows-agent/src/webrtc_session.cpp")
cpp = cpp_path.read_text(encoding="utf-8")

session_reset = (
    "    control_.reset();\n"
    "    file_transfer_receiver_.reset();\n"
    "    video_track_.reset();\n"
)
session_reset_with_authority = (
    "    control_.reset();\n"
    "    input_channel_authority_.InvalidateAll();\n"
    "    file_transfer_receiver_.reset();\n"
    "    video_track_.reset();\n"
)
cpp = replace_in_section(
    cpp,
    "void WebRtcSession::Stop()",
    "bool WebRtcSession::connected() const",
    session_reset,
    session_reset_with_authority,
    "Stop authority invalidation",
)

device_reset = session_reset.replace("    ", "      ")
device_reset_with_authority = session_reset_with_authority.replace("    ", "      ")
cpp = replace_in_section(
    cpp,
    '  if (type == "device-revoked") {',
    '  const std::string from = message.value("from", "");',
    device_reset,
    device_reset_with_authority,
    "device-revoked authority invalidation",
)

cpp = replace_once(
    cpp,
    "    peer_ = peer;\n"
    "    control_.reset();\n"
    "    file_transfer_receiver_.reset();\n",
    "    peer_ = peer;\n"
    "    control_.reset();\n"
    "    input_channel_authority_.InvalidateAll();\n"
    "    file_transfer_receiver_.reset();\n",
    "peer replacement authority invalidation",
)

start = cpp.index("void WebRtcSession::AttachControlChannel(")
end = cpp.index("\nvoid WebRtcSession::HandleControl(", start)
replacement = """void WebRtcSession::AttachControlChannel(const std::shared_ptr<rtc::DataChannel>& channel) {
  if (!channel) return;

  const std::string label = channel->label();
  InputChannelKind kind;
  if (label == \"control\") {
    kind = InputChannelKind::Control;
  } else if (label == \"pointer\") {
    kind = InputChannelKind::Pointer;
  } else {
    return;
  }

  InputChannelGeneration generation = 0;
  {
    std::scoped_lock lock(mutex_);
    generation = input_channel_authority_.Activate(kind);
    if (kind == InputChannelKind::Control) control_ = channel;
  }

  const std::weak_ptr<rtc::DataChannel> weak_channel = channel;
  channel->onOpen([this, label, kind, generation, weak_channel]() {
    auto opened = weak_channel.lock();
    if (!opened) return;
    {
      std::scoped_lock lock(mutex_);
      if (!input_channel_authority_.IsCurrent(kind, generation)) return;
      if (kind == InputChannelKind::Control && control_ != opened) return;
    }

    std::cout << label << \" DataChannel open\\n\";
    if (kind == InputChannelKind::Control && config_.on_monitor_state_requested) {
      config_.on_monitor_state_requested();
    }
  });

  channel->onClosed([this, label, kind, generation, weak_channel]() {
    auto closed = weak_channel.lock();
    bool was_current = false;
    {
      std::scoped_lock lock(mutex_);
      was_current = input_channel_authority_.RevokeIfCurrent(kind, generation);
      if (!was_current) return;
      if (kind == InputChannelKind::Control && (!closed || control_ == closed)) {
        control_.reset();
      }
    }

    // Only the currently authoritative input channel is an authority-loss
    // boundary. A predecessor that closes after its replacement is active must
    // not release keys/buttons belonging to the replacement.
    input_.ReleaseAll();
    std::cout << label << \" DataChannel closed\\n\";
  });

  channel->onMessage([this, kind, generation, weak_channel](rtc::message_variant data) {
    auto source = weak_channel.lock();
    if (!source) return;
    {
      std::scoped_lock lock(mutex_);
      if (!input_channel_authority_.IsCurrent(kind, generation)) return;
      if (kind == InputChannelKind::Control && control_ != source) return;
    }

    if (kind == InputChannelKind::Pointer) {
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

      // Parsing can be non-trivial. Recheck immediately before injection so a
      // concurrent replacement cannot keep a stale pointer callback authorized.
      {
        std::scoped_lock lock(mutex_);
        if (!input_channel_authority_.IsCurrent(kind, generation)) return;
      }
      if (event.kind == PointerWireKind::Move) {
        input_.PointerMove(event.x, event.y);
      } else if (event.kind == PointerWireKind::Wheel) {
        input_.PointerWheel(event.wheel_delta);
      }
      return;
    }

    if (const auto* text = std::get_if<std::string>(&data)) {
      {
        std::scoped_lock lock(mutex_);
        if (!input_channel_authority_.IsCurrent(kind, generation) || control_ != source) return;
      }
      HandleControl(*text);
    }
  });
}
"""
cpp = cpp[:start] + replacement + cpp[end:]
cpp_path.write_text(cpp, encoding="utf-8")
