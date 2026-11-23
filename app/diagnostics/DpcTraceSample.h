#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace exosnap::diagnostics {

struct DpcTraceSample {
    double elapsed_us;
    uint64_t routine;
};

[[nodiscard]] inline uint64_t ReadTracePointer(const uint8_t* bytes, uint32_t pointer_size) noexcept {
    uint64_t pointer = 0;
    std::memcpy(&pointer, bytes, pointer_size == 8 ? sizeof(uint64_t) : sizeof(uint32_t));
    return pointer;
}

[[nodiscard]] inline std::optional<DpcTraceSample> DecodeDpcTraceSample(std::span<const uint8_t> payload,
                                                                        uint8_t opcode, uint32_t pointer_size,
                                                                        int64_t end_qpc, int64_t qpc_freq) noexcept {
    // DPC variants and ISR share the entry QPC and routine pointer. ISR also has a fixed four-byte suffix.
    constexpr uint8_t kIsrOpcode = 67;
    const unsigned suffix_size = opcode == kIsrOpcode ? 4u : 0u;
    if (opcode < 66 || opcode > 69 || (pointer_size != 4 && pointer_size != 8) || qpc_freq <= 0 ||
        payload.size() < sizeof(int64_t) + pointer_size + suffix_size)
        return std::nullopt;

    int64_t begin_qpc = 0;
    std::memcpy(&begin_qpc, payload.data(), sizeof(begin_qpc));
    if (begin_qpc <= 0 || end_qpc < begin_qpc)
        return std::nullopt;

    const double elapsed_us = static_cast<double>(end_qpc - begin_qpc) * 1e6 / static_cast<double>(qpc_freq);
    return DpcTraceSample{elapsed_us, ReadTracePointer(payload.data() + sizeof(begin_qpc), pointer_size)};
}

} // namespace exosnap::diagnostics
