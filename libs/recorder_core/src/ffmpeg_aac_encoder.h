#pragma once

// FfmpegAacEncoder: AAC-LC encoder built on FFmpeg's native libavcodec AAC
// encoder (avcodec_find_encoder(AV_CODEC_ID_AAC)).
//
// This is the migration target for ExoSnap's AAC audio path, replacing the
// statically-linked FDK-AAC fork (FdkAacEncoder). See ADR 0052, which
// supersedes ADR 0043: moving to FFmpeg's native encoder sidesteps the
// fdk-aac-free fork-specific patent-risk argument entirely.
//
// SEQUENCING NOTE: the encoder is only usable at runtime once the pinned
// exosnap-ffmpeg-build release ships an avcodec DLL compiled with
// --enable-encoder=aac (r5+). Against the currently pinned r4 DLL (no encoders)
// Init() fails cleanly with a distinguishable message rather than crashing.
//
// Behaviour matches FdkAacEncoder's IAudioEncoder contract:
//   * AAC-LC only, 44.1/48 kHz, mono/stereo.
//   * Configurable bitrate, default 192 kbit/s (identical to FdkAacEncoder).
//   * Raw AAC access units (no ADTS); CodecPrivateBytes() returns the
//     AudioSpecificConfig from AVCodecContext::extradata, matching what the
//     Matroska A_AAC writer and the MP4 remux path already expect.

#include <recorder_core/interfaces/IAudioEncoder.h>

#include <cstdint>
#include <string>
#include <vector>

// Opaque FFmpeg types — kept out of consumers' translation units (matches how
// fdk_aac_encoder.h forward-declares AACENCODER).
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
struct AVAudioFifo;

namespace recorder_core {

class FfmpegAacEncoder : public IAudioEncoder {
  public:
    FfmpegAacEncoder() = default;
    ~FfmpegAacEncoder() override;

    FfmpegAacEncoder(const FfmpegAacEncoder&) = delete;
    FfmpegAacEncoder& operator=(const FfmpegAacEncoder&) = delete;

    // Set AAC bitrate before Init(). bitrate_kbps=0 keeps the default (192 kbps).
    // Valid range [64, 320] kbps; values outside are clamped.
    void SetBitrateKbps(uint32_t bitrate_kbps) noexcept;

    // Clamp to the valid range [64, 320] kbps; 0 maps to the 192 kbps default.
    // Exposed public for unit testing (mirrors FdkAacEncoder::ResolveBitrateKbps).
    static uint32_t ResolveBitrateKbps(uint32_t kbps) noexcept;

    bool Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) override;

    void FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns, uint64_t& accumulated_frames,
                     uint32_t sample_rate, uint32_t channels, std::vector<EncodedAudioPacket>& out_packets) override;

    void Flush(std::vector<EncodedAudioPacket>& out_packets) override;

    std::vector<uint8_t> CodecPrivateBytes() const override;

    void Shutdown() override;

  private:
    static constexpr int kFrameSizeSamples = 1024; // AAC-LC fixed frame size
    static constexpr uint32_t kDefaultBitrateKbps = 192;

    // Read all packets the encoder currently has ready, appending them to
    // out_packets. Each packet's PTS is taken from libavcodec (frame-pts driven,
    // so encoder delay is accounted for) and rescaled to nanoseconds.
    void ReceiveAvailable(uint64_t pts_origin_ns, std::vector<EncodedAudioPacket>& out_packets);

    uint32_t m_bitrate_kbps = 0; // 0 = use kDefaultBitrateKbps

    AVCodecContext* m_ctx = nullptr;
    AVFrame* m_frame = nullptr; // reusable FLTP frame fed to the encoder
    AVPacket* m_pkt = nullptr;  // reusable output packet
    SwrContext* m_swr = nullptr;
    AVAudioFifo* m_fifo = nullptr; // accumulates FLTP samples to exact frame sizes

    uint32_t m_sample_rate = 0;
    uint32_t m_channels = 0;
    int m_frame_size = kFrameSizeSamples; // AVCodecContext::frame_size after open

    uint64_t m_input_samples = 0;  // per-channel samples fed to the encoder (frame pts)
    uint64_t m_output_samples = 0; // per-channel samples represented by packets already emitted (drives pts_ns)
    uint64_t m_pts_origin_ns = 0;  // pts_ns base captured from the first Feed call
    bool m_pts_origin_set = false; // whether m_pts_origin_ns has been captured

    std::vector<uint8_t> m_codec_private; // AudioSpecificConfig (extradata)
};

} // namespace recorder_core
