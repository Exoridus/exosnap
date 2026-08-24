#pragma once
// A capture source with no device behind it.
//
// Windows offers no user-mode way to create a virtual audio device -- that is a
// kernel driver, which would mean test-signing the machine the release is
// validated on. This seam is the better answer anyway: it reaches the states a
// real card never produces on demand (a mid-stream discontinuity with a known
// gap length, a device clock drifting against QPC, a digitally silent buffer
// that is not a dead endpoint) and it runs on a CI machine with no sound card.
//
// Header-only and free of Qt, D3D and WASAPI, so any test that needs an audio
// source can take one without inheriting a device dependency.

#include "exosnap/engine/interfaces/IAudioCaptureSource.h"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

namespace exosnap::engine::testing {

// One buffer the source will hand out, in the order they were queued.
struct FakeAudioBuffer {
    uint32_t frames = 0;
    bool silent = false;
    bool data_discontinuity = false;
    // Frames lost immediately BEFORE this buffer. The consumer must fill the gap
    // with silence, or every later PTS is early by this much.
    uint32_t gap_frames = 0;
};

class FakeAudioCaptureSource final : public IAudioCaptureSource {
  public:
    struct Config {
        uint32_t sample_rate = 48000;
        uint32_t channels = 2;
        AudioSampleFormat format = AudioSampleFormat::Float32;
        std::string endpoint_name = "Fake capture endpoint";
        // Fails Init() with this message when set, for the paths that must cope
        // with a source that never starts.
        std::string init_error;
        // Reported by LastCaptureHresult(); a drain is expected to surface it
        // rather than a generic failure code.
        int32_t last_capture_hresult = 0;
        // Sine written into every non-silent buffer, so a test can tell real
        // audio from a silence fill by looking at the samples.
        double tone_hz = 1000.0;
        double amplitude = 0.25;
    };

    explicit FakeAudioCaptureSource(Config config = {}) : config_(std::move(config)) {
    }

    // Queue `count` ordinary buffers of `frames` each.
    void PushBuffers(uint32_t count, uint32_t frames) {
        for (uint32_t i = 0; i < count; ++i)
            pending_.push_back(FakeAudioBuffer{frames, false, false, 0});
    }

    void PushBuffer(const FakeAudioBuffer& buffer) {
        pending_.push_back(buffer);
    }

    // A gap the source knows the length of: the next buffer is flagged
    // discontinuous and reports how many frames went missing before it.
    void PushGap(uint32_t lost_frames, uint32_t next_buffer_frames) {
        pending_.push_back(FakeAudioBuffer{next_buffer_frames, false, true, lost_frames});
    }

    // A discontinuity whose length the platform could not report. `gap_frames`
    // stays 0, which is the case a consumer must not silently treat as "no gap".
    void PushUnmeasuredDiscontinuity(uint32_t next_buffer_frames) {
        pending_.push_back(FakeAudioBuffer{next_buffer_frames, false, true, 0});
    }

    // Device clock reported for the NEXT acquired buffer. `drift_ns` offsets the
    // QPC side against the device side, which is exactly what the A/V clock-drift
    // metric measures.
    void SetDeviceTiming(uint64_t device_position_ns, uint64_t qpc_position_ns) {
        timing_.device_position_ns = device_position_ns;
        timing_.qpc_position_ns = qpc_position_ns;
        has_timing_ = true;
    }

    void SetDegraded(uint32_t sources, uint32_t degraded) {
        source_count_ = sources;
        degraded_count_ = degraded;
    }

    [[nodiscard]] int init_count() const noexcept {
        return init_count_;
    }
    [[nodiscard]] int shutdown_count() const noexcept {
        return shutdown_count_;
    }
    [[nodiscard]] bool buffer_held() const noexcept {
        return buffer_held_;
    }
    [[nodiscard]] uint64_t frames_delivered() const noexcept {
        return frames_delivered_;
    }

    // --- IAudioCaptureSource -------------------------------------------------

    bool Init(std::string& out_error) override {
        ++init_count_;
        if (!config_.init_error.empty()) {
            out_error = config_.init_error;
            return false;
        }
        running_ = true;
        return true;
    }

    uint32_t CaptureSourceCount() const override {
        return source_count_;
    }

    uint32_t DegradedSourceCount() const override {
        return degraded_count_;
    }

    uint32_t PendingFrameCount() override {
        if (!running_ || cursor_ >= pending_.size())
            return 0;
        return pending_[cursor_].frames;
    }

    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override {
        if (!running_) {
            out_error = "fake source is not running";
            return false;
        }
        if (cursor_ >= pending_.size()) {
            out_error = "no pending buffer";
            return false;
        }
        // A source that hands out a second buffer before the first is released
        // would let a consumer read freed memory on a real backend; refuse it
        // here so that bug fails in a test instead.
        if (buffer_held_) {
            out_error = "AcquireBuffer without a matching ReleaseBuffer";
            return false;
        }

        const FakeAudioBuffer& next = pending_[cursor_];
        Fill(next);

        out_buf.bytes = bytes_.data();
        out_buf.num_frames = next.frames;
        out_buf.silent = next.silent;
        out_buf.data_discontinuity = next.data_discontinuity;
        out_buf.gap_frames = next.gap_frames;

        buffer_held_ = true;
        return true;
    }

    void ReleaseBuffer() override {
        if (!buffer_held_)
            return;
        frames_delivered_ += pending_[cursor_].frames;
        ++cursor_;
        buffer_held_ = false;
    }

    uint32_t SampleRate() const override {
        return config_.sample_rate;
    }

    uint32_t Channels() const override {
        return config_.channels;
    }

    AudioSampleFormat SampleFormat() const override {
        return config_.format;
    }

    const std::string& EndpointName() const override {
        return config_.endpoint_name;
    }

    int32_t LastCaptureHresult() const override {
        return config_.last_capture_hresult;
    }

    bool LastBufferDeviceTiming(AudioDeviceTiming& out_timing) const override {
        if (!has_timing_)
            return false;
        out_timing = timing_;
        return true;
    }

    void Shutdown() override {
        ++shutdown_count_;
        running_ = false;
        buffer_held_ = false;
    }

  private:
    void Fill(const FakeAudioBuffer& buffer) {
        const size_t sample_bytes = config_.format == AudioSampleFormat::Float32 ? 4u : 2u;
        bytes_.assign(static_cast<size_t>(buffer.frames) * config_.channels * sample_bytes, 0);
        if (buffer.silent || config_.amplitude == 0.0)
            return;

        for (uint32_t frame = 0; frame < buffer.frames; ++frame) {
            const double t = static_cast<double>(phase_ + frame) / static_cast<double>(config_.sample_rate);
            const double value = config_.amplitude * std::sin(2.0 * std::numbers::pi * config_.tone_hz * t);
            for (uint32_t channel = 0; channel < config_.channels; ++channel) {
                const size_t index = (static_cast<size_t>(frame) * config_.channels + channel) * sample_bytes;
                if (config_.format == AudioSampleFormat::Float32) {
                    const auto sample = static_cast<float>(value);
                    std::memcpy(bytes_.data() + index, &sample, sizeof(sample));
                } else {
                    const auto sample = static_cast<int16_t>(value * 32767.0);
                    std::memcpy(bytes_.data() + index, &sample, sizeof(sample));
                }
            }
        }
        // Continuous across buffers AND across a gap: the tone keeps the phase a
        // real device would have kept, so a consumer that mis-fills a gap shows
        // up as a discontinuity in the waveform rather than a silent pass.
        phase_ += buffer.frames + buffer.gap_frames;
    }

    Config config_;
    std::vector<FakeAudioBuffer> pending_;
    std::vector<uint8_t> bytes_;
    size_t cursor_ = 0;
    uint64_t phase_ = 0;
    uint64_t frames_delivered_ = 0;
    bool running_ = false;
    bool buffer_held_ = false;
    bool has_timing_ = false;
    AudioDeviceTiming timing_{};
    uint32_t source_count_ = 1;
    uint32_t degraded_count_ = 0;
    int init_count_ = 0;
    int shutdown_count_ = 0;
};

} // namespace exosnap::engine::testing
