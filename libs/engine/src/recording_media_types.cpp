#include <exosnap/engine/recording_media_types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace exosnap::engine {
namespace {

int HexValue(char value) noexcept {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

} // namespace

std::string FormatIdentity(const std::array<uint8_t, 16>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            result.push_back('-');
        result.push_back(digits[bytes[index] >> 4]);
        result.push_back(digits[bytes[index] & 0x0f]);
    }
    return result;
}

std::optional<std::array<uint8_t, 16>> ParseIdentity(std::string_view text) {
    if (text.size() != 36)
        return std::nullopt;
    std::array<uint8_t, 16> bytes{};
    size_t offset = 0;
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            if (text[offset++] != '-')
                return std::nullopt;
        }
        const int high = HexValue(text[offset++]);
        const int low = HexValue(text[offset++]);
        if (high < 0 || low < 0)
            return std::nullopt;
        bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return bytes;
}

std::array<uint8_t, 16> NewIdentity() {
    std::random_device entropy;
    std::array<uint8_t, 16> bytes{};
    for (size_t index = 0; index < bytes.size(); index += 4) {
        const uint32_t value = entropy();
        for (size_t byte = 0; byte < 4; ++byte)
            bytes[index + byte] = static_cast<uint8_t>(value >> (8 * byte));
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
    return bytes;
}

} // namespace exosnap::engine
