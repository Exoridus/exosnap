#pragma once

// FfmpegAacEncoder: AAC-LC encoder built on FFmpeg's native libavcodec AAC
// encoder (avcodec_find_encoder(AV_CODEC_ID_AAC)).
//
// This is the migration target for ExoSnap's AAC audio path, replacing the
// previously statically-linked third-party AAC-LC encoder. See ADR 0052,
// which supersedes ADR 0043: moving to FFmpeg's native encoder sidesteps
// that prior fork's patent-risk argument entirely.
//
// SEQUENCING NOTE: the encoder is only usable at runtime once the pinned
// exosnap-ffmpeg-build release ships an avcodec DLL compiled with
// --enable-encoder=aac (r5+). Against the currently pinned r4 DLL (no encoders)
// Init() fails cleanly with a distinguishable message rather than crashing.
//
// Behaviour matches the retired encoder's IAudioEncoder contract:
//   * AAC-LC only, 44.1/48 kHz, mono/stereo.
//   * Configurable bitrate, default 192 kbit/s (same default as before).
//   * Raw AAC access units (no ADTS); CodecPrivateBytes() returns the
//     AudioSpecificConfig from AVCodecContext::extradata, matching what the
//     Matroska A_AAC writer and the MP4 remux path already expect.

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/interfaces/IAudioEncoder.h>

#include <cstdint>
#include <string>
#include <vector>

// Opaque FFmpeg types — kept out of consumers' translation units.
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
struct AVAudioFifo;

namespace exosnap::engine {

class FfmpegAacEncoder : public IAudioEncoder {
  public:
    FfmpegAacEncoder() = default;
    ~FfmpegAacEncoder() override;

    FfmpegAacEncoder(const FfmpegAacEncoder&) = delete;
    FfmpegAacEncoder& operator=(const FfmpegAacEncoder&) = delete;

    // Set AAC bitrate before Init(). bitrate_kbps=0 keeps the default (192 kbps).
    // Valid range [kAacBitrateKbpsMin, kAacBitrateKbpsMax] kbps; values outside
    // are clamped.
    void SetBitrateKbps(uint32_t bitrate_kbps) noexcept;

    // Clamp to the valid [kAacBitrateKbpsMin, kAacBitrateKbpsMax] kbps range;
    // 0 maps to the 192 kbps default. Exposed public for unit testing.
    static uint32_t ResolveBitrateKbps(uint32_t kbps) noexcept;

    bool Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) override;

    void FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns, uint64_t& accumulated_frames,
                     uint32_t sample_rate, uint32_t channels, std::vector<EncodedAudioPacket>& out_packets) override;

    void Flush(std::vector<EncodedAudioPacket>& out_packets) override;

    std::vector<uint8_t> CodecPrivateBytes() const override;
    uint32_t CodecDelaySamples() const noexcept override {
        return m_codec_delay_samples;
    }

    void Shutdown() override;

  private:
    static constexpr int kFrameSizeSamples = 1024; // AAC-LC fixed frame size
    static constexpr uint32_t kDefaultBitrateKbps = 192;

    // Read all packets the encoder currently has ready, appending them to
    // out_packets. Each packet's PTS is taken from libavcodec (frame-pts driven,
    // so encoder delay is accounted for) and rescaled to nanoseconds.
    void ReceiveAvailable(uint64_t pts_origin_ns, std::vector<EncodedAudioPacket>& out_packets);

    // Encode every whole frame currently held by the FIFO. Shared by FeedFloat32
    // and Flush so the trailing samples the converter drain adds go out through
    // exactly the same path as steady-state audio. accumulated_frames may be null
    // (the flush path has no caller-side frame counter to advance).
    void EncodeBufferedFrames(uint64_t* accumulated_frames, std::vector<EncodedAudioPacket>& out_packets);

    uint32_t m_bitrate_kbps = 0; // 0 = use kDefaultBitrateKbps

    AVCodecContext* m_ctx = nullptr;
    AVFrame* m_frame = nullptr; // reusable FLTP frame fed to the encoder
    AVPacket* m_pkt = nullptr;  // reusable output packet
    SwrContext* m_swr = nullptr;
    AVAudioFifo* m_fifo = nullptr; // accumulates FLTP samples to exact frame sizes

    uint32_t m_sample_rate = 0;
    uint32_t m_channels = 0;
    int m_frame_size = kFrameSizeSamples; // AVCodecContext::frame_size after open
    uint32_t m_codec_delay_samples = 0;   // AVCodecContext::initial_padding after open

    uint64_t m_input_samples = 0;  // per-channel samples fed to the encoder (frame pts)
    uint64_t m_output_samples = 0; // per-channel samples represented by packets already emitted (drives pts_ns)
    uint64_t m_pts_origin_ns = 0;  // pts_ns base captured from the first Feed call
    bool m_pts_origin_set = false; // whether m_pts_origin_ns has been captured

    std::vector<uint8_t> m_codec_private; // AudioSpecificConfig (extradata)
};

} // namespace exosnap::engine
