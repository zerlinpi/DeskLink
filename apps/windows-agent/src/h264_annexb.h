#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace desklink {

bool H264LooksAnnexB(const uint8_t* data, size_t size);

bool NormalizeH264SequenceHeader(
    const uint8_t* data,
    size_t size,
    std::vector<uint8_t>* annexb,
    uint8_t* nal_length_size);

bool NormalizeH264AccessUnit(
    const uint8_t* data,
    size_t size,
    uint8_t nal_length_size,
    std::vector<uint8_t>* annexb);

bool H264AnnexBHasParameterSet(const std::vector<uint8_t>& bytes);

}  // namespace desklink
