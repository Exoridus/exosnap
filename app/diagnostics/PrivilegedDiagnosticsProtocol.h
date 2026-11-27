#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace exosnap::diagnostics::privileged {

// Local x64 Windows wire format. Each pipe message has exactly one fixed-size
// frame. Reject a different version instead of interpreting its layout.
inline constexpr uint32_t kMagic = 0x44505845; // EXPD
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kMaxPresents = 512;
inline constexpr uint32_t kMaxRelated = 12;

enum class PresentStatus : uint32_t { Off, Open, Unavailable };

struct Request {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t present = 0;
};

struct Present {
    uint64_t qpc = 0;
    uint64_t hwnd = 0;
    uint32_t pid = 0;
    int32_t mode = 0;
    int32_t sync_interval = 0;
    uint32_t flags = 0;
    uint32_t related_count = 0;
    std::array<uint32_t, kMaxRelated> related{};
    uint32_t reserved = 0;
};

struct Response {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    PresentStatus present_state = PresentStatus::Off;
    uint32_t present_count = 0;
    uint32_t dpc_open = 0;
    uint32_t reserved = 0;
    int64_t frequency = 0;
    uint64_t lost = 0;
    uint64_t dpc_qpc = 0;
    double dpc_max_us = 0;
    double dpc_average_us = 0;
    std::array<char, 128> dpc_driver{};
    std::array<Present, kMaxPresents> presents{};
};

static_assert(sizeof(Request) == 12);
static_assert(sizeof(Present) == 88);
static_assert(sizeof(Response) == 45248);

[[nodiscard]] inline bool IsValid(const Request& frame) {
    return frame.magic == kMagic && frame.version == kVersion && frame.present <= 1;
}

[[nodiscard]] inline bool IsValid(const Response& frame) {
    if (frame.magic != kMagic || frame.version != kVersion || frame.present_count > kMaxPresents ||
        frame.dpc_open > 1 || frame.reserved != 0)
        return false;
    switch (frame.present_state) {
    case PresentStatus::Open:
        if (frame.frequency <= 0)
            return false;
        break;
    case PresentStatus::Off:
    case PresentStatus::Unavailable:
        if (frame.present_count != 0 || frame.frequency != 0 || frame.lost != 0)
            return false;
        break;
    default:
        return false;
    }
    const auto valid_present = [](const Present& row) {
        return row.pid != 0 && row.qpc != 0 && row.related_count <= kMaxRelated && row.flags <= 3 && row.reserved == 0;
    };
    if (!std::all_of(frame.presents.begin(), frame.presents.begin() + frame.present_count, valid_present))
        return false;
    if (!std::isfinite(frame.dpc_max_us) || !std::isfinite(frame.dpc_average_us) || frame.dpc_average_us < 0 ||
        frame.dpc_max_us < frame.dpc_average_us)
        return false;
    const auto end = std::find(frame.dpc_driver.begin(), frame.dpc_driver.end(), '\0');
    if (end == frame.dpc_driver.end())
        return false;
    if (frame.dpc_qpc == 0)
        return frame.dpc_average_us == 0 && frame.dpc_max_us == 0 && end == frame.dpc_driver.begin();
    return frame.dpc_open != 0 && std::all_of(frame.dpc_driver.begin(), end, [](char c) {
               return c >= 0x20 && c <= 0x7e && c != '/' && c != '\\' && c != ':';
           });
}

} // namespace exosnap::diagnostics::privileged
