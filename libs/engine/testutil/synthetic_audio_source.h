#pragma once

#include <exosnap/engine/interfaces/IAudioCaptureSource.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace exosnap::engine::testutil {

// Fixed-duration 48 kHz stereo PCM without retaining future packets in memory.
// Each borrowed buffer stays valid until ReleaseBuffer, just like capture.
class SyntheticAudioSource : public IAudioCaptureSource {
  public:
    SyntheticAudioSource(std::shared_ptr<std::atomic<bool>> drained, size_t packet_count)
        : drained_(std::move(drained)), packet_count_(packet_count) {
        packet_.fill(0.1f);
    }

    bool Init(std::string& out_error) override {
        initialized_ = true;
        out_error.clear();
        return true;
    }
    uint32_t PendingFrameCount() override {
        if (!initialized_ || acquired_)
            return 0;
        if (next_ < packet_count_)
            return kFramesPerPacket;
        if (drained_)
            drained_->store(true);
        return 0;
    }
    bool AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) override {
        out_buf = {};
        if (!initialized_ || acquired_ || next_ >= packet_count_) {
            out_error.clear();
            return false;
        }
        acquired_ = true;
        out_buf.bytes = reinterpret_cast<const uint8_t*>(packet_.data());
        out_buf.num_frames = kFramesPerPacket;
        out_buf.silent = false;
        out_error.clear();
        return true;
    }
    void ReleaseBuffer() override {
        if (!acquired_)
            return;
        acquired_ = false;
        ++next_;
        if (next_ >= packet_count_ && drained_)
            drained_->store(true);
    }
    uint32_t SampleRate() const override {
        return kSampleRate;
    }
    uint32_t Channels() const override {
        return kChannels;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return endpoint_;
    }
    void Shutdown() override {
    }

  private:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;
    static constexpr uint32_t kFramesPerPacket = 960; // 20 ms

    std::shared_ptr<std::atomic<bool>> drained_;
    size_t packet_count_ = 0;
    bool initialized_ = false;
    bool acquired_ = false;
    size_t next_ = 0;
    std::array<float, static_cast<size_t>(kFramesPerPacket) * kChannels> packet_{};
    std::string endpoint_ = "SyntheticCapture";
};

} // namespace exosnap::engine::testutil
