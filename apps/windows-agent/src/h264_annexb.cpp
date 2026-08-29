#include "h264_annexb.h"

#include <algorithm>
#include <iterator>

namespace desklink {
namespace {

constexpr uint8_t kStartCode[] = {0, 0, 0, 1};

bool HasStartCodeAt(const uint8_t* data, size_t size, size_t offset, size_t* prefix_size = nullptr) {
  if (!data || offset >= size) return false;
  if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
      data[offset + 2] == 0 && data[offset + 3] == 1) {
    if (prefix_size) *prefix_size = 4;
    return true;
  }
  if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
      data[offset + 2] == 1) {
    if (prefix_size) *prefix_size = 3;
    return true;
  }
  return false;
}

void AppendAnnexBNal(
    const uint8_t* data,
    size_t size,
    std::vector<uint8_t>* output) {
  output->insert(output->end(), std::begin(kStartCode), std::end(kStartCode));
  output->insert(output->end(), data, data + size);
}

bool ConvertLengthPrefixed(
    const uint8_t* data,
    size_t size,
    uint8_t nal_length_size,
    std::vector<uint8_t>* output) {
  if (!data || size == 0 || !output || nal_length_size < 1 || nal_length_size > 4) return false;

  std::vector<uint8_t> converted;
  converted.reserve(size + 16);
  size_t offset = 0;
  size_t nal_count = 0;
  while (offset < size) {
    if (size - offset < nal_length_size) return false;

    uint32_t nal_size = 0;
    for (uint8_t i = 0; i < nal_length_size; ++i) {
      nal_size = (nal_size << 8) | data[offset + i];
    }
    offset += nal_length_size;
    if (nal_size == 0 || nal_size > size - offset) return false;

    AppendAnnexBNal(data + offset, nal_size, &converted);
    offset += nal_size;
    ++nal_count;
  }

  if (nal_count == 0 || offset != size) return false;
  output->swap(converted);
  return true;
}

bool ParseAvcDecoderConfigurationRecord(
    const uint8_t* data,
    size_t size,
    std::vector<uint8_t>* output,
    uint8_t* nal_length_size) {
  if (!data || size < 7 || !output || !nal_length_size || data[0] != 1) return false;

  const uint8_t length_size = static_cast<uint8_t>((data[4] & 0x03) + 1);
  if (length_size < 1 || length_size > 4) return false;

  std::vector<uint8_t> converted;
  size_t offset = 6;
  size_t parameter_count = 0;
  const uint8_t sps_count = static_cast<uint8_t>(data[5] & 0x1f);
  for (uint8_t i = 0; i < sps_count; ++i) {
    if (size - offset < 2) return false;
    const uint16_t nal_size = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
    offset += 2;
    if (nal_size == 0 || nal_size > size - offset) return false;
    AppendAnnexBNal(data + offset, nal_size, &converted);
    offset += nal_size;
    ++parameter_count;
  }

  if (offset >= size) return false;
  const uint8_t pps_count = data[offset++];
  for (uint8_t i = 0; i < pps_count; ++i) {
    if (size - offset < 2) return false;
    const uint16_t nal_size = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
    offset += 2;
    if (nal_size == 0 || nal_size > size - offset) return false;
    AppendAnnexBNal(data + offset, nal_size, &converted);
    offset += nal_size;
    ++parameter_count;
  }

  if (parameter_count == 0) return false;
  output->swap(converted);
  *nal_length_size = length_size;
  return true;
}

}  // namespace

bool H264LooksAnnexB(const uint8_t* data, size_t size) {
  if (!data || size < 4) return false;
  const size_t scan_limit = std::min<size_t>(size, 16);
  for (size_t offset = 0; offset < scan_limit; ++offset) {
    if (HasStartCodeAt(data, size, offset)) return true;
  }
  return false;
}

bool NormalizeH264SequenceHeader(
    const uint8_t* data,
    size_t size,
    std::vector<uint8_t>* annexb,
    uint8_t* nal_length_size,
    bool* access_units_length_prefixed) {
  if (!data || size == 0 || !annexb || !nal_length_size || !access_units_length_prefixed) {
    return false;
  }

  if (H264LooksAnnexB(data, size)) {
    annexb->assign(data, data + size);
    *access_units_length_prefixed = false;
    return true;
  }

  if (!ParseAvcDecoderConfigurationRecord(data, size, annexb, nal_length_size)) return false;
  *access_units_length_prefixed = true;
  return true;
}

bool NormalizeH264AccessUnit(
    const uint8_t* data,
    size_t size,
    uint8_t nal_length_size,
    bool access_units_length_prefixed,
    std::vector<uint8_t>* annexb) {
  if (!data || size == 0 || !annexb) return false;

  if (access_units_length_prefixed) {
    if (ConvertLengthPrefixed(data, size, nal_length_size, annexb)) return true;
    if (H264LooksAnnexB(data, size)) {
      annexb->assign(data, data + size);
      return true;
    }
  } else {
    if (H264LooksAnnexB(data, size)) {
      annexb->assign(data, data + size);
      return true;
    }
    if (ConvertLengthPrefixed(data, size, nal_length_size, annexb)) return true;
  }

  // If framing metadata was missing or a vendor MFT changes representation,
  // accept only a candidate length size that parses the entire access unit.
  for (const uint8_t candidate : {uint8_t{4}, uint8_t{2}, uint8_t{1}}) {
    if (candidate != nal_length_size && ConvertLengthPrefixed(data, size, candidate, annexb)) {
      return true;
    }
  }
  return false;
}

bool H264AnnexBHasParameterSet(const std::vector<uint8_t>& bytes) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    size_t prefix_size = 0;
    if (!HasStartCodeAt(bytes.data(), bytes.size(), offset, &prefix_size)) {
      ++offset;
      continue;
    }

    const size_t nal_offset = offset + prefix_size;
    if (nal_offset >= bytes.size()) return false;
    const uint8_t nal_type = static_cast<uint8_t>(bytes[nal_offset] & 0x1f);
    if (nal_type == 7 || nal_type == 8) return true;
    offset = nal_offset + 1;
  }
  return false;
}

}  // namespace desklink
