#include "h264_annexb.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool Equals(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right) {
  return left == right;
}

int Fail(const char* message) {
  std::cerr << "H264 Annex-B smoke failed: " << message << "\n";
  return 1;
}

}  // namespace

int main() {
  const std::vector<uint8_t> annexb_header = {
      0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f,
      0, 0, 0, 1, 0x68, 0xce, 0x06,
  };
  std::vector<uint8_t> normalized;
  uint8_t length_size = 4;
  bool length_prefixed = true;
  if (!desklink::NormalizeH264SequenceHeader(
          annexb_header.data(),
          annexb_header.size(),
          &normalized,
          &length_size,
          &length_prefixed)) {
    return Fail("Annex-B sequence header was rejected");
  }
  if (length_prefixed || !Equals(normalized, annexb_header)) {
    return Fail("Annex-B sequence header was modified or misclassified");
  }
  if (!desklink::H264AnnexBHasParameterSet(normalized)) {
    return Fail("Annex-B parameter set was not detected");
  }

  // AVCDecoderConfigurationRecord with four-byte NAL lengths, one SPS and one PPS.
  const std::vector<uint8_t> avcc_header = {
      0x01, 0x42, 0x00, 0x1f, 0xff, 0xe1,
      0x00, 0x04, 0x67, 0x42, 0x00, 0x1f,
      0x01,
      0x00, 0x03, 0x68, 0xce, 0x06,
  };
  const std::vector<uint8_t> expected_header = annexb_header;
  normalized.clear();
  length_size = 0;
  length_prefixed = false;
  if (!desklink::NormalizeH264SequenceHeader(
          avcc_header.data(),
          avcc_header.size(),
          &normalized,
          &length_size,
          &length_prefixed)) {
    return Fail("avcC sequence header was rejected");
  }
  if (!length_prefixed || length_size != 4 || !Equals(normalized, expected_header)) {
    return Fail("avcC sequence header was not normalized correctly");
  }

  const std::vector<uint8_t> length_prefixed_au = {
      0x00, 0x00, 0x00, 0x04, 0x65, 0xaa, 0xbb, 0xcc,
      0x00, 0x00, 0x00, 0x03, 0x41, 0x11, 0x22,
  };
  const std::vector<uint8_t> expected_au = {
      0x00, 0x00, 0x00, 0x01, 0x65, 0xaa, 0xbb, 0xcc,
      0x00, 0x00, 0x00, 0x01, 0x41, 0x11, 0x22,
  };
  normalized.clear();
  if (!desklink::NormalizeH264AccessUnit(
          length_prefixed_au.data(),
          length_prefixed_au.size(),
          4,
          true,
          &normalized) ||
      !Equals(normalized, expected_au)) {
    return Fail("length-prefixed access unit was not converted to Annex-B");
  }

  // This prefix can look like a 3-byte Annex-B start code (00 00 01), but the
  // known avcC framing mode must win. The encoded NAL length is 0x00000101.
  std::vector<uint8_t> ambiguous_length_prefixed = {0x00, 0x00, 0x01, 0x01};
  ambiguous_length_prefixed.push_back(0x65);
  ambiguous_length_prefixed.resize(4 + 0x101, 0x55);
  normalized.clear();
  if (!desklink::NormalizeH264AccessUnit(
          ambiguous_length_prefixed.data(),
          ambiguous_length_prefixed.size(),
          4,
          true,
          &normalized)) {
    return Fail("ambiguous length-prefixed access unit was rejected");
  }
  if (normalized.size() != ambiguous_length_prefixed.size() ||
      normalized[0] != 0 || normalized[1] != 0 || normalized[2] != 0 || normalized[3] != 1 ||
      normalized[4] != 0x65) {
    return Fail("known length-prefixed framing lost to Annex-B heuristic");
  }

  const std::vector<uint8_t> malformed = {0x00, 0x00, 0x00, 0x20, 0x65, 0x01};
  normalized.clear();
  if (desklink::NormalizeH264AccessUnit(
          malformed.data(),
          malformed.size(),
          4,
          true,
          &normalized)) {
    return Fail("malformed length-prefixed access unit was accepted");
  }

  std::cout << "H264 Annex-B normalization smoke passed\n";
  return 0;
}
