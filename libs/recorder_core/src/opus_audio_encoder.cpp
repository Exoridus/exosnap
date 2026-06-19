#include "opus_audio_encoder.h"

#include "codec_private.h"

#include <recorder_core/packet_types.h>

#include <opus.h>

#include <algorithm>
#include <vector>

namespace recorder_core {

// ---------------------------------------------------------------------------
// SetEncodingParams (call before Init)
// ---------------------------------------------------------------------------

void OpusAudioEncoder::SetEncodingParams(uint32_t bitrate_kbps, OpusFrameDuration frame_duration,
                                         int complexity) noexcept {
    m_bitrate_kbps = bitrate_kbps;
    m_frame_duration = frame_duration;
    m_frame_size_samples = OpusFrameSizeSamples(frame_duration); // keep accessor in sync pre-Init
    m_complexity = std::clamp(complexity, 0, 10);
}

// ---------------------------------------------------------------------------
// Internal: clamp Opus bitrate
// ---------------------------------------------------------------------------

/*static*/ uint32_t OpusAudioEncoder::ClampOpusBitrateKbps(uint32_t kbps) noexcept {
    if (kbps == 0) {
        return 0; // 0 = use libopus auto default
    }
    // Opus valid range is 6-510 kbps; we use a practical minimum of 32 kbps.
    return std::clamp(kbps, 32u, 510u);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

OpusAudioEncoder::~OpusAudioEncoder() {
    Shutdown();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool OpusAudioEncoder::Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) {
    Shutdown();

    // Resolve frame size from the configured duration.
    m_frame_size_samples = OpusFrameSizeSamples(m_frame_duration);

    int err = OPUS_OK;
    m_encoder = opus_encoder_create(static_cast<opus_int32>(sample_rate), static_cast<int>(channels),
                                    OPUS_APPLICATION_AUDIO, &err);
    if (m_encoder == nullptr || err != OPUS_OK) {
        out_error = "opus_encoder_create failed: ";
        out_error += opus_strerror(err);
        Shutdown();
        return false;
    }

    // Enable VBR (default in libopus, set explicitly per roadmap).
    if (opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1)) != OPUS_OK) {
        out_error = "OPUS_SET_VBR failed";
        Shutdown();
        return false;
    }

    // Set bitrate (VBR target). 0 means use libopus auto default.
    const uint32_t clamped_kbps = ClampOpusBitrateKbps(m_bitrate_kbps);
    if (clamped_kbps > 0) {
        const opus_int32 bps = static_cast<opus_int32>(clamped_kbps * 1000u);
        if (opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bps)) != OPUS_OK) {
            out_error = "OPUS_SET_BITRATE failed";
            Shutdown();
            return false;
        }
    }
    // When clamped_kbps == 0, leave the libopus default. Callers should set 160 kbps explicitly.

    // Set complexity.
    if (opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(m_complexity)) != OPUS_OK) {
        out_error = "OPUS_SET_COMPLEXITY failed";
        Shutdown();
        return false;
    }

    int lookahead = 0;
    if (opus_encoder_ctl(m_encoder, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK || lookahead < 0) {
        out_error = "OPUS_GET_LOOKAHEAD failed";
        Shutdown();
        return false;
    }

    uint8_t opus_head[19] = {};
    codec_private::BuildOpusCodecPrivate(sample_rate, channels, static_cast<uint16_t>(lookahead), opus_head);
    m_codec_private.assign(opus_head, opus_head + 19);

    m_sample_rate = sample_rate;
    m_channels = channels;
    m_frame_buffer.clear();
    m_emitted_frames = 0;
    return true;
}

// ---------------------------------------------------------------------------
// FeedFloat32
// ---------------------------------------------------------------------------

void OpusAudioEncoder::FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns,
                                   uint64_t& accumulated_frames, uint32_t sample_rate, uint32_t channels,
                                   std::vector<EncodedAudioPacket>& out_packets) {
    (void)pts_ns;
    if (m_encoder == nullptr || data == nullptr || total_float_samples == 0) {
        return;
    }
    if (sample_rate != m_sample_rate || channels != m_channels || channels == 0) {
        return;
    }

    const size_t frame_float_samples = static_cast<size_t>(m_frame_size_samples) * channels;
    m_frame_buffer.insert(m_frame_buffer.end(), data, data + total_float_samples);
    m_emitted_frames = accumulated_frames;

    while (m_frame_buffer.size() >= frame_float_samples) {
        std::vector<uint8_t> out_buf(4000);
        const int encoded_bytes = opus_encode_float(m_encoder, m_frame_buffer.data(), m_frame_size_samples,
                                                    out_buf.data(), static_cast<opus_int32>(out_buf.size()));

        if (encoded_bytes > 0) {
            EncodedAudioPacket pkt;
            pkt.pts_ns = (accumulated_frames * 1000000000ULL) / sample_rate;
            pkt.bytes.assign(out_buf.begin(), out_buf.begin() + encoded_bytes);
            out_packets.push_back(std::move(pkt));
        }

        accumulated_frames += static_cast<uint64_t>(m_frame_size_samples);
        m_emitted_frames = accumulated_frames;
        m_frame_buffer.erase(m_frame_buffer.begin(), m_frame_buffer.begin() + frame_float_samples);
    }
}

// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

void OpusAudioEncoder::Flush(std::vector<EncodedAudioPacket>& out_packets) {
    if (m_encoder == nullptr || m_channels == 0 || m_sample_rate == 0 || m_frame_buffer.empty()) {
        m_frame_buffer.clear();
        return;
    }

    const size_t frame_float_samples = static_cast<size_t>(m_frame_size_samples) * m_channels;
    if (m_frame_buffer.size() < frame_float_samples) {
        m_frame_buffer.resize(frame_float_samples, 0.0f);
    }

    std::vector<uint8_t> out_buf(4000);
    const int encoded_bytes = opus_encode_float(m_encoder, m_frame_buffer.data(), m_frame_size_samples, out_buf.data(),
                                                static_cast<opus_int32>(out_buf.size()));
    if (encoded_bytes > 0) {
        EncodedAudioPacket pkt;
        pkt.pts_ns = (m_emitted_frames * 1000000000ULL) / m_sample_rate;
        pkt.bytes.assign(out_buf.begin(), out_buf.begin() + encoded_bytes);
        out_packets.push_back(std::move(pkt));
        m_emitted_frames += static_cast<uint64_t>(m_frame_size_samples);
    }

    m_frame_buffer.clear();
}

// ---------------------------------------------------------------------------
// ResetState
// ---------------------------------------------------------------------------

uint64_t OpusAudioEncoder::ResetState() {
    const uint64_t discarded = static_cast<uint64_t>(m_frame_buffer.size()) / static_cast<uint64_t>(m_channels);
    m_frame_buffer.clear();
    if (m_encoder) {
        opus_encoder_ctl(m_encoder, OPUS_RESET_STATE);
    }
    m_emitted_frames += discarded;
    return discarded;
}

// ---------------------------------------------------------------------------
// CodecPrivateBytes
// ---------------------------------------------------------------------------

std::vector<uint8_t> OpusAudioEncoder::CodecPrivateBytes() const {
    return m_codec_private;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void OpusAudioEncoder::Shutdown() {
    if (m_encoder != nullptr) {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }
    m_sample_rate = 0;
    m_channels = 0;
    m_codec_private.clear();
    m_frame_buffer.clear();
    m_emitted_frames = 0;
    // Preserve m_frame_size_samples, m_bitrate_kbps, m_complexity, m_frame_duration
    // so they survive a Shutdown/Init cycle (useful for reinit after error).
    m_frame_size_samples = OpusFrameSizeSamples(m_frame_duration);
}

} // namespace recorder_core
