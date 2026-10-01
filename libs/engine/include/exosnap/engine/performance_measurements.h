#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace exosnap::engine {

// Opt-in CPU submission/wait measurements. GPU execution time is measured separately.
enum class PerformanceStage {
    DecodeReadback,
    DecodeNormalize,
    CpuConversion,
    GpuUpload,
    ShaderCompile,
    ResourceCreation,
    QueueWait,
    DecodeDropped,
    PresentDropped,
    MailboxReplaced,
    AudioPollWake,
    Count
};
inline constexpr std::array<const char*, static_cast<size_t>(PerformanceStage::Count)> kPerformanceStageNames = {
    "decode_readback", "decode_normalize", "cpu_conversion",  "gpu_upload",       "shader_compile", "resource_creation",
    "queue_wait",      "decode_dropped",   "present_dropped", "mailbox_replaced", "audio_poll_wake"};
struct PerformanceMeasurement {
    uint64_t calls = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
};
namespace performance_detail {
struct Counters {
    std::atomic<uint64_t> calls{0}, total_ns{0}, max_ns{0};
};
inline std::atomic<bool> enabled{false};
inline std::array<Counters, static_cast<size_t>(PerformanceStage::Count)> stages;
} // namespace performance_detail
inline void EnablePerformanceMeasurements(bool enabled) {
    performance_detail::enabled.store(enabled, std::memory_order_relaxed);
}
inline PerformanceMeasurement ReadPerformanceMeasurement(PerformanceStage stage) {
    auto& s = performance_detail::stages[static_cast<size_t>(stage)];
    return {s.calls.load(std::memory_order_relaxed), s.total_ns.load(std::memory_order_relaxed),
            s.max_ns.load(std::memory_order_relaxed)};
}
inline void RecordPerformanceEvent(PerformanceStage stage) {
    if (performance_detail::enabled.load(std::memory_order_relaxed))
        performance_detail::stages[static_cast<size_t>(stage)].calls.fetch_add(1, std::memory_order_relaxed);
}
class ScopedPerformanceMeasurement {
  public:
    explicit ScopedPerformanceMeasurement(PerformanceStage stage)
        : stage_(stage), enabled_(performance_detail::enabled.load(std::memory_order_relaxed)) {
        if (enabled_)
            start_ = std::chrono::steady_clock::now();
    }
    ~ScopedPerformanceMeasurement() {
        if (!enabled_)
            return;
        const auto ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count());
        auto& s = performance_detail::stages[static_cast<size_t>(stage_)];
        s.calls.fetch_add(1, std::memory_order_relaxed);
        s.total_ns.fetch_add(ns, std::memory_order_relaxed);
        auto previous = s.max_ns.load(std::memory_order_relaxed);
        while (previous < ns && !s.max_ns.compare_exchange_weak(previous, ns, std::memory_order_relaxed)) {
        }
    }
    ScopedPerformanceMeasurement(const ScopedPerformanceMeasurement&) = delete;
    ScopedPerformanceMeasurement& operator=(const ScopedPerformanceMeasurement&) = delete;

  private:
    PerformanceStage stage_;
    bool enabled_;
    std::chrono::steady_clock::time_point start_{};
};
} // namespace exosnap::engine
