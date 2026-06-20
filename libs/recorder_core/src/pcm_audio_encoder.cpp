#include "pcm_audio_encoder.h"

#include <algorithm>
#include <cmath>

namespace recorder_core {

// ---------------------------------------------------------------------------
// Float32ToS16
// ---------------------------------------------------------------------------

int16_t PcmAudioEncoder::Float32ToS16(float sample) noexcept {
    // Clamp to [-1, 1] then scale by 32767 and round to nearest. Using 32767
    // (not 32768) keeps full-scale +1.0 at +32767 and -1.0 at -32767, leaving
    // the asymmetric -32768 reachable only by inputs below -1.0 (which we clamp
    // away). This matches the float->PCM16 conversion used by the AAC path.
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    const long scaled = std::lround(clamped * 32767.0f);
    // lround already produced a value in [-32767, 32767]; clamp defensively to
    // the full int16 range so a NaN-derived path can never overflow.
    const long bounded = std::clamp(scaled, -32768L, 32767L);
    return static_cast<int16_t>(bounded);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool PcmAudioEncoder::Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) {
    if (sample_rate == 0 || channels == 0) {
        out_error = "PcmAudioEncoder::Init requires non-zero sample_rate and channels";
        return false;
    }
    m_sample_rate = sample_rate;
    m_channels = channels;
    return true;
}

// ---------------------------------------------------------------------------
// FeedFloat32
// ---------------------------------------------------------------------------

void PcmAudioEncoder::FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns,
                                  uint64_t& accumulated_frames, uint32_t sample_rate, uint32_t channels,
                                  std::vector<EncodedAudioPacket>& out_packets) {
    (void)pts_ns;
    if (data == nullptr || total_float_samples == 0 || channels == 0) {
        return;
    }
    if (sample_rate != m_sample_rate || channels != m_channels) {
        return;
    }

    EncodedAudioPacket pkt;
    // PTS derived from the running frame counter, identical to the Opus/AAC path.
    pkt.pts_ns = (accumulated_frames * 1000000000ULL) / sample_rate;

    // Convert interleaved Float32 -> interleaved S16LE. Two bytes per sample,
    // little-endian, which is exactly A_PCM/INT_LIT's on-wire layout.
    pkt.bytes.resize(total_float_samples * 2);
    for (size_t i = 0; i < total_float_samples; ++i) {
        const int16_t s = Float32ToS16(data[i]);
        pkt.bytes[i * 2] = static_cast<uint8_t>(static_cast<uint16_t>(s) & 0xFFu);
        pkt.bytes[i * 2 + 1] = static_cast<uint8_t>((static_cast<uint16_t>(s) >> 8u) & 0xFFu);
    }
    out_packets.push_back(std::move(pkt));

    // Advance the frame counter by the number of PCM frames in this buffer.
    accumulated_frames += static_cast<uint64_t>(total_float_samples / channels);
}

// ---------------------------------------------------------------------------
// Flush / CodecPrivateBytes / Shutdown
// ---------------------------------------------------------------------------

void PcmAudioEncoder::Flush(std::vector<EncodedAudioPacket>& out_packets) {
    (void)out_packets; // No buffered state — nothing to drain.
}

std::vector<uint8_t> PcmAudioEncoder::CodecPrivateBytes() const {
    return {}; // A_PCM/INT_LIT has no CodecPrivate.
}

void PcmAudioEncoder::Shutdown() {
    m_sample_rate = 0;
    m_channels = 0;
}

} // namespace recorder_core
