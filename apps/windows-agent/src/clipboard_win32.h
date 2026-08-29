#pragma once

#include <string>

namespace desklink {

// Clipboard transfer is intentionally text-only and user-initiated by the
// controller. Both helpers cap UTF-8 payloads to 1 MiB.
bool ReadClipboardTextUtf8(std::string* text, std::string* error);
bool WriteClipboardTextUtf8(const std::string& text, std::string* error);

}  // namespace desklink
