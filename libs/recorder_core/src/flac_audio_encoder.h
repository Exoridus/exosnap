#pragma once

// FlacAudioEncoder: lossless FLAC encoder (libFLAC) for Matroska A_FLAC.
//
// FLAC is a lossless compressed codec. This class wraps libFLAC's stream
// encoder behind the same IAudioEncoder interface as the Opus/AAC/PCM encoders
// so the audio thread and mux path treat it identically. It converts captured
// interleaved Float32 PCM into interleaved 16-bit samples (FLAC__int32, valued
// per bits_per_sample=16) and feeds libFLAC, which buffers internally and emits
// encoded FLAC frames via a write callback. Because libFLAC buffers, a single
// FeedFloat32 call may produce zero or more output packets (like the AAC path).
//
// Matroska A_FLAC requires the CodecPrivate to be the *native FLAC stream
// header*: the "fLaC" 4-byte marker followed by the STREAMINFO metadata block
// (plus any other metadata blocks the encoder writes before the first audio
// frame). libFLAC's write callback distinguishes header writes (samples==0)
// from audio-frame writes (samples>0); we accumulate the former into the
// CodecPrivate buffer and emit the latter as packets.
//
// Fixed format for this slice: 48 kHz, stereo, 16-bit, compression level 5.

#include <recorder_core/interfaces/IAudioEncoder.h>
#include <recorder_core/packet_types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace recorder_core {

class FlacAudioEncoder : public IAudioEncoder {
  public:
    FlacAudioEncoder() = default;
    ~FlacAudioEncoder() override;

    FlacAudioEncoder(const FlacAudioEncoder&) = delete;
    FlacAudioEncoder& operator=(const FlacAudioEncoder&) = delete;

    bool Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) override;

    void FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns, uint64_t& accumulated_frames,
                     uint32_t sample_rate, uint32_t channels, std::vector<EncodedAudioPacket>& out_packets) override;

    // Finish the FLAC stream and drain any remaining buffered frames.
    void Flush(std::vector<EncodedAudioPacket>& out_packets) override;

    // The native FLAC header ("fLaC" + STREAMINFO + any leading metadata blocks),
    // captured from the first (samples==0) write-callback invocations during Init.
    // This is the mandatory A_FLAC Matroska CodecPrivate.
    std::vector<uint8_t> CodecPrivateBytes() const override;

    void Shutdown() override;

    // Set the output bit depth before Init(). Accepted values: 16, 24.
    // 16 is the default (backward-compatible). Must be called before Init().
    void SetBitDepth(uint32_t bits) noexcept;

    // Set the FLAC compression level before Init(). Accepted range: [0, 8].
    // 5 is the default. Must be called before Init().
    void SetCompressionLevel(int level) noexcept;

    // Return the effective bit depth (as configured, or 16 if never set).
    [[nodiscard]] uint32_t BitsPerSample() const noexcept {
        return m_bits_per_sample;
    }

    // Convert one interleaved Float32 sample to a FLAC__int32 value at the
    // configured bit depth. Clamps to [-1, 1] and rounds to nearest.
    // Exposed static for unit testing; bits must be 16 or 24.
    static int32_t Float32ToInt(float sample, uint32_t bits) noexcept;

    // Convenience wrappers matching the original API (16-bit only).
    static int32_t Float32ToS16(float sample) noexcept;

    // The default sample format constants.
    static constexpr uint32_t kBitsPerSample = 16;
    static constexpr uint32_t kCompressionLevel = 5;

    // Handle the bytes libFLAC's write callback hands us. samples==0 →
    // header/metadata bytes (CodecPrivate); samples>0 → an encoded audio frame
    // appended to the pending-packet buffer. Public so the C-ABI trampoline in
    // the .cpp (which must match libFLAC's exact typedef) can forward to it.
    int OnWrite(const uint8_t* buffer, size_t bytes, uint32_t samples);

  private:
    // Opaque libFLAC encoder handle (FLAC__StreamEncoder*). Held as void* so this
    // header does not drag the libFLAC C headers into consumers (audio_thread.cpp).
    void* m_encoder = nullptr;
    uint32_t m_sample_rate = 0;
    uint32_t m_channels = 0;
    uint32_t m_bits_per_sample = kBitsPerSample;
    int m_compression_level = static_cast<int>(kCompressionLevel);

    // True until the first audio-frame write arrives; while true, write-callback
    // bytes are header bytes and go into m_codec_private.
    bool m_capturing_header = true;
    std::vector<uint8_t> m_codec_private;

    // The encoder buffers frames internally; the write callback appends finished
    // frames here. FeedFloat32/Flush drain this into out_packets. PTS is derived
    // from m_emitted_samples (the input position of each frame's first sample),
    // which advances per emitted frame independently of when libFLAC flushes.
    std::vector<EncodedAudioPacket> m_pending_frames;
    uint64_t m_emitted_samples = 0;

    // Scratch buffer for interleaved Float32 → int16 conversion (reused).
    std::vector<int32_t> m_int_scratch;

    bool m_finished = false;
};

} // namespace recorder_core
