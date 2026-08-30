#include "host_capabilities.h"

#include <array>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

#ifndef DESKLINK_HOST_CAPABILITIES_FIXTURE_PATH
#error DESKLINK_HOST_CAPABILITIES_FIXTURE_PATH must be defined
#endif

int main() {
  desklink::HostCapabilitiesV1Input input;
  input.secure_attention_available = true;
  input.secure_attention_policy = "allow-services";
  input.clipboard_available = true;
  input.file_transfer_available = true;
  input.system_audio_available = false;
  input.microphone_available = false;
  input.protected_desktop_available = false;
  input.monitor_count = 2;
  input.maximum_fps = 144;
  input.maximum_width = 1920;
  input.maximum_height = 1080;
  input.h264_available = true;

  std::ifstream fixture_stream(DESKLINK_HOST_CAPABILITIES_FIXTURE_PATH, std::ios::binary);
  if (!fixture_stream) {
    std::cerr << "Unable to open HostCapabilitiesV1 fixture: "
              << DESKLINK_HOST_CAPABILITIES_FIXTURE_PATH << "\n";
    return 1;
  }

  nlohmann::json fixture;
  try {
    fixture_stream >> fixture;
  } catch (const std::exception& error) {
    std::cerr << "Invalid HostCapabilitiesV1 fixture: " << error.what() << "\n";
    return 1;
  }

  const nlohmann::json produced = desklink::BuildHostCapabilitiesV1(input);
  if (produced != fixture) {
    std::cerr << "Windows HostCapabilitiesV1 producer drifted from the canonical fixture.\n"
              << "Produced:\n" << produced.dump(2) << "\n"
              << "Fixture:\n" << fixture.dump(2) << "\n";
    return 1;
  }

  if (produced.value("t", "") != "host-capabilities" ||
      produced.value("version", 0) != 1 ||
      !produced.contains("capabilities") ||
      !produced["capabilities"].is_object()) {
    std::cerr << "HostCapabilitiesV1 must use the canonical nested capabilities envelope.\n";
    return 1;
  }

  static constexpr std::array<const char*, 7> kLegacyFlatFields = {
      "secureAttentionAvailable",
      "secureAttentionReason",
      "secureAttentionPolicy",
      "clipboardAvailable",
      "fileTransferAvailable",
      "audioAvailable",
      "protectedDesktopAvailable",
  };
  for (const char* field : kLegacyFlatFields) {
    if (produced.contains(field)) {
      std::cerr << "HostCapabilitiesV1 unexpectedly emitted legacy flat field: "
                << field << "\n";
      return 1;
    }
  }

  std::cout << "DeskLink HostCapabilitiesV1 producer smoke passed.\n";
  return 0;
}
