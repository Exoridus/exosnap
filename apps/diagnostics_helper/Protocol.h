#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace exosnap::diagnostics::ipc {

// Each local message-mode pipe write is one complete record. No wire field is
// used as an allocation size, process handle, path, provider GUID or ETW flag.
inline constexpr uint32_t kMagic = 0x444f5845; // EXOD, little endian
inline constexpr uint16_t kVersion = 1;
enum class Kind : uint16_t { Begin = 1, Ready, Present, Lost, Failure, BeginDpcOnly, Dpc, DpcUnavailable };

struct alignas(8) Record {
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    Kind kind{};
    uint64_t value = 0; // QPC frequency, timestamp, loss delta or fixed failure code
    uint64_t window = 0;
    uint32_t process = 0;
    uint8_t mode = 0;
    uint8_t flags = 0; // bit 0 tearing, bit 1 uncapped, bit 2 discarded
    uint16_t related_count = 0;
    std::array<uint32_t, 16> related{};
    std::array<char, 64> driver{}; // bounded basename only in DPC records
};

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(Record) == 160 && offsetof(Record, related) == 32);
static_assert(std::is_trivially_copyable_v<Record>);

[[nodiscard]] constexpr bool Valid(const Record& record, size_t bytes) noexcept {
    if (bytes != sizeof(Record) || record.magic != kMagic || record.version != kVersion)
        return false;
    bool terminated = false;
    for (const char c : record.driver) {
        if (!c) {
            terminated = true;
        } else if (terminated || record.kind != Kind::Dpc ||
                   !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                     c == '-' || c == '_')) {
            return false;
        }
    }
    if (!terminated)
        return false;
    if (record.kind == Kind::Dpc) {
        // value and window are peak/mean nanoseconds. process carries sample age in ms.
        if (record.window > record.value || record.process > 15000 || record.mode || record.flags ||
            record.related_count)
            return false;
    } else if (record.kind == Kind::Present) {
        if (!record.process || !record.value || record.mode > 8 || (record.flags & ~7u) ||
            record.related_count > record.related.size())
            return false;
    } else {
        if (record.process || record.window || record.mode || record.flags || record.related_count)
            return false;
        switch (record.kind) {
        case Kind::DpcUnavailable:
        case Kind::BeginDpcOnly:
        case Kind::Begin:
            if (record.value)
                return false;
            break;
        case Kind::Ready:
            if (!record.value || record.value > INT64_MAX)
                return false;
            break;
        case Kind::Lost:
            break;
        case Kind::Failure:
            if (record.value < 1 || record.value > 5)
                return false;
            break;
        default:
            return false;
        }
    }
    for (size_t i = record.related_count; i < record.related.size(); ++i) {
        if (record.related[i] != 0)
            return false;
    }
    return true;
}

} // namespace exosnap::diagnostics::ipc
