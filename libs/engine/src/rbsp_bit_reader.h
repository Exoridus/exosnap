#pragma once

// Minimal MSB-first bit reader over an RBSP (Raw Byte Sequence Payload) with
// emulation-prevention-byte removal (0x00 0x00 0x03 -> 0x00 0x00).
//
// Shared between the HEVC (annexb_to_hvcc.cpp) and H.264 (annexb_to_avcc.cpp)
// SPS parsers: both bitstreams use the same Annex-B RBSP bit-packing and
// Exp-Golomb ue(v) coding (ITU-T H.264 7.2 / H.265 7.2).

#include <cstddef>
#include <cstdint>

namespace exosnap::engine::annexb::detail {

class RbspBitReader {
  public:
    RbspBitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {
    }

    bool ReadBit(uint32_t& out) {
        if (byte_pos_ >= size_)
            return false;
        const uint8_t byte = data_[byte_pos_];
        out = (byte >> (7u - bit_pos_)) & 0x1u;
        if (++bit_pos_ == 8u) {
            bit_pos_ = 0u;
            ++byte_pos_;
            // Skip the emulation_prevention_three_byte: 0x00 0x00 0x03 → the 0x03
            // is not part of the RBSP payload. Detect when the next byte is 0x03
            // and the two bytes before it (within the NAL) were both 0x00.
            if (byte_pos_ >= 2u && byte_pos_ < size_ && data_[byte_pos_] == 0x03u && data_[byte_pos_ - 1u] == 0x00u &&
                data_[byte_pos_ - 2u] == 0x00u) {
                ++byte_pos_;
            }
        }
        return true;
    }

    bool ReadBits(uint32_t count, uint32_t& out) {
        out = 0u;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t bit = 0;
            if (!ReadBit(bit))
                return false;
            out = (out << 1u) | bit;
        }
        return true;
    }

    // Exp-Golomb unsigned: count leading zero bits n, then read n more bits.
    bool ReadUe(uint32_t& out) {
        uint32_t zeros = 0;
        uint32_t bit = 0;
        while (true) {
            if (!ReadBit(bit))
                return false;
            if (bit == 1u)
                break;
            if (++zeros > 31u)
                return false; // malformed / out of range
        }
        uint32_t suffix = 0;
        if (zeros > 0u && !ReadBits(zeros, suffix))
            return false;
        out = (1u << zeros) - 1u + suffix;
        return true;
    }

  private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t byte_pos_ = 0;
    uint32_t bit_pos_ = 0;
};

} // namespace exosnap::engine::annexb::detail
