#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sunbridge {
inline constexpr std::size_t kEncodedVideoHeaderSize = 24;

inline bool IsEncodedVideoKeyFrame(const std::vector<std::uint8_t>& bytes) {
    // CFV1 carries its IDR flag after the four-byte format signature.
    // The first byte is always 'C', including for ordinary delta frames.
    return bytes.size() > kEncodedVideoHeaderSize &&
        bytes[0] == 'C' && bytes[1] == 'F' && bytes[2] == 'V' && bytes[3] == '1' &&
        bytes[4] != 0;
}
} // namespace sunbridge
