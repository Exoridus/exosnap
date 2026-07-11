#pragma once

// A/V clock-drift estimation from audio device timing.
//
// Video frames are paced on the QPC timeline (CFR frame index x frame
// interval, scheduled against QueryPerformanceCounter). Audio PTS is derived
// from the accumulated sample counter, i.e. from the sound device's own
// crystal. Real A/V drift is therefore the rate difference between those two
// clocks — something the difference of two independently buffered pipeline
// PTS values (which only measures encoder/queue latency) can never see.
//
// WASAPI hands us the ground truth with every capture packet:
// IAudioCaptureClient::GetBuffer reports the packet's device position (in
// frames of the device clock) together with the QPC time at which that
// position was recorded. Feeding those pairs here yields
//
//   drift_ms = (qpc_elapsed) - (device_elapsed)
//
// normalized at the first observation (constant pipeline offsets cancel) and
// smoothed over a rolling window of observations so per-packet QPC jitter does
// not read as drift.
//
// Sign convention: positive = the audio device clock runs SLOW relative to
// QPC, so recorded audio events land at earlier PTS than their matching video
// frames — audio leads video on playback. Negative = audio lags video.
//
// A discontinuity (device underrun) does not disturb the estimate: the device
// position keeps counting through the lost frames — the same property the
// discontinuity gap fill uses to repair the sample timeline — so both axes
// advance together across the gap.
//
// Pure and hardware-free; unit-tested with synthetic position sequences.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace recorder_core {

class AudioClockDriftEstimator {
  public:
    // ~128 capture packets at the typical 10 ms WASAPI period smooths over
    // roughly 1.3 s of observations.
    static constexpr size_t kDefaultWindowSize = 128;

    explicit AudioClockDriftEstimator(size_t window_size = kDefaultWindowSize)
        : window_(window_size == 0 ? 1 : window_size, 0.0) {
    }

    void AddObservation(uint64_t device_position_ns, uint64_t qpc_position_ns) noexcept {
        if (!has_baseline_) {
            has_baseline_ = true;
            base_device_ns_ = device_position_ns;
            base_qpc_ns_ = qpc_position_ns;
        }
        const double device_elapsed_ms =
            static_cast<double>(static_cast<int64_t>(device_position_ns - base_device_ns_)) / 1e6;
        const double qpc_elapsed_ms = static_cast<double>(static_cast<int64_t>(qpc_position_ns - base_qpc_ns_)) / 1e6;
        const double drift_ms = qpc_elapsed_ms - device_elapsed_ms;

        if (count_ == window_.size()) {
            sum_ -= window_[head_];
        } else {
            ++count_;
        }
        window_[head_] = drift_ms;
        sum_ += drift_ms;
        head_ = (head_ + 1) % window_.size();
        if (head_ == 0) {
            // Rebuild the running sum once per wrap so floating-point error
            // cannot accumulate over hours of recording.
            sum_ = 0.0;
            for (size_t i = 0; i < count_; ++i) {
                sum_ += window_[i];
            }
        }
    }

    [[nodiscard]] bool HasEstimate() const noexcept {
        return count_ > 0;
    }

    // Smoothed drift in milliseconds (see sign convention above). 0.0 until
    // the first observation.
    [[nodiscard]] double DriftMs() const noexcept {
        return count_ > 0 ? sum_ / static_cast<double>(count_) : 0.0;
    }

  private:
    bool has_baseline_ = false;
    uint64_t base_device_ns_ = 0;
    uint64_t base_qpc_ns_ = 0;

    std::vector<double> window_;
    size_t head_ = 0;
    size_t count_ = 0;
    double sum_ = 0.0;
};

} // namespace recorder_core
