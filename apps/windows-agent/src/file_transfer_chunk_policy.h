#pragma once

#include <cstddef>
#include <cstdint>

namespace desklink {

constexpr bool IsValidUploadChunkProgress(
    size_t payload_size,
    uint64_t received,
    uint64_t expected_size) noexcept {
  if (payload_size == 0 || received >= expected_size) return false;
  return static_cast<uint64_t>(payload_size) <= expected_size - received;
}

static_assert(!IsValidUploadChunkProgress(0, 0, 1024));
static_assert(!IsValidUploadChunkProgress(0, 0, 0));
static_assert(IsValidUploadChunkProgress(512, 0, 1024));
static_assert(IsValidUploadChunkProgress(512, 512, 1024));
static_assert(!IsValidUploadChunkProgress(513, 512, 1024));
static_assert(!IsValidUploadChunkProgress(1, 1024, 1024));

}  // namespace desklink
