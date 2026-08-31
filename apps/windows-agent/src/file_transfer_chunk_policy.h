#pragma once

#include <cstddef>
#include <cstdint>

namespace desklink {

inline bool IsValidUploadChunkProgress(
    size_t payload_size,
    uint64_t received,
    uint64_t expected_size) noexcept {
  if (payload_size == 0 || received >= expected_size) return false;
  return static_cast<uint64_t>(payload_size) <= expected_size - received;
}

}  // namespace desklink
