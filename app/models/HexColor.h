#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace exosnap {

// Reads a colour typed or pasted as hex: surrounding whitespace and one leading
// '#' are ignored, case does not matter, and the three-digit shorthand expands
// the way CSS does (#fa0 is #ffaa00). Anything else is not a colour.
[[nodiscard]] inline std::optional<std::array<uint8_t, 3>> ParseHexColor(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    if (!text.empty() && text.front() == '#')
        text.remove_prefix(1);
    if (text.size() != 3 && text.size() != 6)
        return std::nullopt;

    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::array<uint8_t, 3> rgb{};
    for (size_t channel = 0; channel < 3; ++channel) {
        int high = 0;
        int low = 0;
        if (text.size() == 3) {
            high = low = digit(text[channel]);
        } else {
            high = digit(text[channel * 2]);
            low = digit(text[channel * 2 + 1]);
        }
        if (high < 0 || low < 0)
            return std::nullopt;
        rgb[channel] = static_cast<uint8_t>(high * 16 + low);
    }
    return rgb;
}

// Six lowercase hex digits, without the '#'.
[[nodiscard]] inline std::string FormatHexColor(uint8_t r, uint8_t g, uint8_t b) {
    char buffer[7];
    std::snprintf(buffer, sizeof(buffer), "%02x%02x%02x", r, g, b);
    return buffer;
}

} // namespace exosnap
