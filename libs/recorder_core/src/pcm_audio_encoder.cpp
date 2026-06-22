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
// Float32ToS24LE
// ---------------------------------------------------------------------------

void PcmAudioEncoder::Float32ToS24LE(float sample, uint8_t* dst) noexcept {
    // Full-scale S24: range [-8388607, 8388607] (leaves -8388608 unreachable for
    // symmetry, same reasoning as S16). Clamp, scale, round to nearest.
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    const long scaled = std::lround(clamped * 8388607.0f);
    const long bounded = std::clamp(scaled, -8388608L, 8388607L);
    const auto u = static_cast<uint32_t>(static_cast<int32_t>(bounded));
    dst[0] = static_cast<uint8_t>(u & 0xFFu);
    dst[1] = static_cast<uint8_t>((u >> 8u) & 0xFFu);
    dst[2] = static_cast<uint8_t>((u >> 16u) & 0xFFu);
}

// ---------------------------------------------------------------------------
// Float32ToS32
// ---------------------------------------------------------------------------

int32_t PcmAudioEncoder::Float32ToS32(float sample) noexcept {
    // Full-scale S32: range [-2147483647, 2147483647] (leaves INT32_MIN
    // unreachable for symmetry). Clamp, scale, round to nearest.
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    // Use double for the scaling to avoid precision loss at 32-bit integer range.
    const long long scaled = std::llround(static_cast<double>(clamped) * 2147483647.0);
    // Clamp defensively to int32 range (llround of [-1,1]*2147483647 is already
    // in range, but clamp handles NaN-derived paths).
    const long long bounded = std::clamp(scaled, static_cast<long long>(INT32_MIN), static_cast<long long>(INT32_MAX));
    return static_cast<int32_t>(bounded);
}

// ---------------------------------------------------------------------------
// SetBitDepth
// ---------------------------------------------------------------------------

void PcmAudioEncoder::SetBitDepth(uint32_t bits) noexcept {
    // Only accept supported depths; silently keep the previous value for anything else.
    if (bits == 16 || bits == 24 || bits == 32) {
        m_bit_depth = bits;
    }
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

    const uint32_t bytes_per_sample = m_bit_depth / 8u;
    pkt.bytes.resize(total_float_samples * bytes_per_sample);

    if (m_bit_depth == 16) {
        // Convert interleaved Float32 -> interleaved S16LE.
        for (size_t i = 0; i < total_float_samples; ++i) {
            const int16_t s = Float32ToS16(data[i]);
            pkt.bytes[i * 2] = static_cast<uint8_t>(static_cast<uint16_t>(s) & 0xFFu);
            pkt.bytes[i * 2 + 1] = static_cast<uint8_t>((static_cast<uint16_t>(s) >> 8u) & 0xFFu);
        }
    } else if (m_bit_depth == 24) {
        // Convert interleaved Float32 -> interleaved packed S24LE (3 bytes/sample).
        for (size_t i = 0; i < total_float_samples; ++i) {
            Float32ToS24LE(data[i], &pkt.bytes[i * 3]);
        }
    } else {
        // m_bit_depth == 32: Convert interleaved Float32 -> interleaved S32LE.
        for (size_t i = 0; i < total_float_samples; ++i) {
            const int32_t s = Float32ToS32(data[i]);
            const auto u = static_cast<uint32_t>(s);
            pkt.bytes[i * 4] = static_cast<uint8_t>(u & 0xFFu);
            pkt.bytes[i * 4 + 1] = static_cast<uint8_t>((u >> 8u) & 0xFFu);
            pkt.bytes[i * 4 + 2] = static_cast<uint8_t>((u >> 16u) & 0xFFu);
            pkt.bytes[i * 4 + 3] = static_cast<uint8_t>((u >> 24u) & 0xFFu);
        }
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
