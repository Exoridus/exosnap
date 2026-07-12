#pragma once

// Fixed-bucket geometric latency histogram — pure, allocation-free, mergeable.
//
// Backs the whole-session encode-latency / frame-time distributions of the perf
// measurement infrastructure. A fixed-bucket log histogram was chosen over exact
// sample retention (unbounded at 60 fps over hours) and over P²/t-digest
// (approximate, not mergeable, no raw distribution to serialise): Add() is O(1)
// with no allocation, memory is constant (64 * 8 bytes), two histograms Merge()
// exactly, and BucketCounts() serialises trivially into the engine JSONL so the
// distribution round-trips into the analysis script.
//
// 64 buckets total: 63 geometric buckets spanning [kLoMs, kHiMs) plus one
// overflow bucket (index 63) for samples >= kHiMs. Samples below kLoMs clamp into
// bucket 0. Quantile() interpolates linearly inside the containing bucket, so the
// quantile error is bounded by the bucket width (~16% relative) and is known.
//
// Pure value type — no threading, no GPU, no NVENC. The aggregator owns instances
// under its own mutex.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace recorder_core {

class LatencyHistogram {
  public:
    static constexpr std::size_t kBucketCount = 64;
    static constexpr std::size_t kGeoBuckets = kBucketCount - 1; // 63 geometric, 1 overflow
    static constexpr double kLoMs = 0.05;
    static constexpr double kHiMs = 500.0;

    // Per-bucket geometric ratio; Ratio()^kGeoBuckets == kHiMs/kLoMs so the top
    // geometric edge lands exactly on kHiMs.
    [[nodiscard]] static double Ratio() noexcept {
        return std::pow(kHiMs / kLoMs, 1.0 / static_cast<double>(kGeoBuckets));
    }

    void Add(double ms) noexcept {
        ++counts_[BucketIndex(ms)];
        ++total_;
    }

    void Merge(const LatencyHistogram& other) noexcept {
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            counts_[i] += other.counts_[i];
        }
        total_ += other.total_;
    }

    void Clear() noexcept {
        counts_.fill(0);
        total_ = 0;
    }

    [[nodiscard]] uint64_t count() const noexcept {
        return total_;
    }

    [[nodiscard]] const std::array<uint64_t, kBucketCount>& BucketCounts() const noexcept {
        return counts_;
    }

    // Bucket index for a sample. NaN / non-positive -> bucket 0; >= kHiMs ->
    // overflow bucket kGeoBuckets (63); < kLoMs -> bucket 0.
    [[nodiscard]] static std::size_t BucketIndex(double ms) noexcept {
        if (!(ms > 0.0)) {
            return 0; // NaN or <= 0
        }
        if (ms >= kHiMs) {
            return kGeoBuckets; // overflow
        }
        if (ms < kLoMs) {
            return 0;
        }
        const double idx = std::log(ms / kLoMs) / std::log(Ratio());
        auto b = static_cast<std::size_t>(idx); // floor, in [0, kGeoBuckets)
        if (b >= kGeoBuckets) {
            b = kGeoBuckets - 1;
        }
        return b;
    }

    // Lower edge (inclusive) of a bucket, in ms. Bucket 0 absorbs sub-kLoMs
    // samples so its floor is reported as 0. The overflow bucket floors at kHiMs.
    [[nodiscard]] static double BucketLowEdge(std::size_t b) noexcept {
        if (b == 0) {
            return 0.0;
        }
        if (b >= kGeoBuckets) {
            return kHiMs;
        }
        return kLoMs * std::pow(Ratio(), static_cast<double>(b));
    }

    // Upper edge (exclusive) of a bucket, in ms. The overflow bucket has no
    // finite upper edge; kHiMs is returned as a conservative floor.
    [[nodiscard]] static double BucketHighEdge(std::size_t b) noexcept {
        if (b >= kGeoBuckets) {
            return kHiMs;
        }
        return kLoMs * std::pow(Ratio(), static_cast<double>(b + 1));
    }

    // Linear-interpolated quantile, q in [0, 1]. Empty histogram -> 0.
    [[nodiscard]] double Quantile(double q) const noexcept {
        if (total_ == 0) {
            return 0.0;
        }
        if (q < 0.0) {
            q = 0.0;
        }
        if (q > 1.0) {
            q = 1.0;
        }
        const double target = q * static_cast<double>(total_);
        uint64_t cumulative = 0;
        for (std::size_t b = 0; b < kBucketCount; ++b) {
            const uint64_t c = counts_[b];
            if (c == 0) {
                continue;
            }
            if (static_cast<double>(cumulative + c) >= target) {
                if (b >= kGeoBuckets) {
                    return kHiMs; // overflow bucket: report its floor
                }
                const double lo = BucketLowEdge(b);
                const double hi = BucketHighEdge(b);
                double into = (target - static_cast<double>(cumulative)) / static_cast<double>(c);
                if (into < 0.0) {
                    into = 0.0;
                }
                if (into > 1.0) {
                    into = 1.0;
                }
                return lo + into * (hi - lo);
            }
            cumulative += c;
        }
        return kHiMs;
    }

  private:
    std::array<uint64_t, kBucketCount> counts_{};
    uint64_t total_ = 0;
};

} // namespace recorder_core
