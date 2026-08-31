#include "file_transfer_chunk_policy.h"

#include <iostream>

int main() {
  if (desklink::IsValidUploadChunkProgress(0, 0, 1024)) {
    std::cerr << "Zero-length chunk must not be accepted for an incomplete upload.\n";
    return 1;
  }
  if (desklink::IsValidUploadChunkProgress(0, 0, 0)) {
    std::cerr << "Zero-length binary chunk must not be used to finalize an empty upload.\n";
    return 1;
  }
  if (!desklink::IsValidUploadChunkProgress(512, 0, 1024)) {
    std::cerr << "A normal in-range upload chunk was rejected.\n";
    return 1;
  }
  if (!desklink::IsValidUploadChunkProgress(512, 512, 1024)) {
    std::cerr << "A chunk exactly matching the remaining upload size was rejected.\n";
    return 1;
  }
  if (desklink::IsValidUploadChunkProgress(513, 512, 1024)) {
    std::cerr << "An upload chunk larger than the remaining file size was accepted.\n";
    return 1;
  }
  if (desklink::IsValidUploadChunkProgress(1, 1024, 1024)) {
    std::cerr << "A chunk was accepted after the upload was already complete.\n";
    return 1;
  }

  std::cout << "DeskLink file transfer chunk policy smoke passed.\n";
  return 0;
}
