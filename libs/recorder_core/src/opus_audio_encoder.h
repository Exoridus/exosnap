#pragma once

#include <recorder_core/interfaces/IAudioEncoder.h>
#include <recorder_core/recorder_session.h>

#include <cstdint>
#include <string>
#include <vector>

struct OpusEncoder;

namespace recorder_core {

class OpusAudioEncoder : public IAudioEncoder {
  public:
    ~OpusAudioEncoder() override;

    // Configure audio encoding parameters before calling Init().
    // bitrate_kbps: target VBR bitrate [32, 510], or 0 for libopus default (~160 kbps stereo).
    // frame_duration: Opus frame size (20/10/5/2.5 ms). Default = Ms20.
    // complexity: 0–10 (10 = best quality / highest CPU). Default = 10.
    void SetEncodingParams(uint32_t bitrate_kbps, OpusFrameDuration frame_duration = OpusFrameDuration::Ms20,
                           int complexity = 10) noexcept;

    bool Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) override;

    void FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns, uint64_t& accumulated_frames,
                     uint32_t sample_rate, uint32_t channels, std::vector<EncodedAudioPacket>& out_packets) override;

    void Flush(std::vector<EncodedAudioPacket>& out_packets) override;

    // Mid-stream discontinuity: reset encoder state and discard partial frame buffer.
    // Returns the number of audio frames that were discarded (to advance PTS).
    uint64_t ResetState();

    std::vector<uint8_t> CodecPrivateBytes() const override;

    uint64_t EmittedFrames() const {
        return m_emitted_frames;
    }

    // Returns the configured frame size in samples (set at Init time).
    int FrameSizeSamples() const noexcept {
        return m_frame_size_samples;
    }

    void Shutdown() override;

    // Clamp bitrate to the valid Opus range [32 000, 510 000] bps.
    // Returns 0 when bitrate_kbps == 0 (means: use libopus default).
    // Exposed as public for unit testing.
    static uint32_t ClampOpusBitrateKbps(uint32_t kbps) noexcept;

  private:
    // Configurable encoding params (set via SetEncodingParams before Init).
    uint32_t m_bitrate_kbps = 0; // 0 = libopus default
    OpusFrameDuration m_frame_duration = OpusFrameDuration::Ms20;
    int m_complexity = 10;

    // Resolved at Init() time from m_frame_duration.
    int m_frame_size_samples = 960; // default 20 ms @ 48 kHz

    ::OpusEncoder* m_encoder = nullptr;
    uint32_t m_sample_rate = 0;
    uint32_t m_channels = 0;
    std::vector<uint8_t> m_codec_private;
    std::vector<float> m_frame_buffer;
    uint64_t m_emitted_frames = 0;
};

} // namespace recorder_core
