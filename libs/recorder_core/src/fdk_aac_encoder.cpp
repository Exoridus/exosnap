#include "fdk_aac_encoder.h"

#include <aacenc_lib.h>

#include <algorithm>
#include <cstdio>

namespace recorder_core {

// ---------------------------------------------------------------------------
// SetBitrateKbps / ResolveBitrateKbps
// ---------------------------------------------------------------------------

void FdkAacEncoder::SetBitrateKbps(uint32_t bitrate_kbps) noexcept {
    m_bitrate_kbps = bitrate_kbps;
}

/*static*/ uint32_t FdkAacEncoder::ResolveBitrateKbps(uint32_t kbps) noexcept {
    if (kbps == 0) {
        return kDefaultBitrateKbps;
    }
    return std::clamp(kbps, 64u, 320u);
}

FdkAacEncoder::~FdkAacEncoder() {
    Shutdown();
}

bool FdkAacEncoder::Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) {
    Shutdown();

    AACENC_ERROR err = aacEncOpen(&m_enc, 0, channels);
    if (err != AACENC_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "aacEncOpen failed: 0x%08X", static_cast<unsigned>(err));
        out_error = buf;
        m_enc = nullptr;
        return false;
    }

    if ((err = aacEncoder_SetParam(m_enc, AACENC_AOT, AOT_AAC_LC)) != AACENC_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "AACENC_AOT failed: 0x%08X", static_cast<unsigned>(err));
        out_error = buf;
        Shutdown();
        return false;
    }

    if ((err = aacEncoder_SetParam(m_enc, AACENC_SAMPLERATE, sample_rate)) != AACENC_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "AACENC_SAMPLERATE failed: 0x%08X", static_cast<unsigned>(err));
        out_error = buf;
        Shutdown();
        return false;
    }

    const UINT channel_mode = (channels == 2) ? MODE_2 : MODE_1;
    if ((err = aacEncoder_SetParam(m_enc, AACENC_CHANNELMODE, channel_mode)) != AACENC_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "AACENC_CHANNELMODE failed: 0x%08X", static_cast<unsigned>(err));
        out_error = buf;
        Shutdown();
        return false;
    }

    {
        const UINT aac_bps = static_cast<UINT>(ResolveBitrateKbps(m_bitrate_kbps) * 1000u);
        if ((err = aacEncoder_SetParam(m_enc, AACENC_BITRATE, aac_bps)) != AACENC_OK) {
            char buf[64];
            snprintf(buf, sizeof(buf), "AACENC_BITRATE failed: 0x%08X", static_cast<unsigned>(err));
            out_error = buf;
            Shutdown();
            return false;
        }
    }

    if ((err = aacEncoder_SetParam(m_enc, AACENC_TRANSMUX, TT_MP4_RAW)) != AACENC_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "AACENC_TRANSMUX failed: 0x%08X", static_cast<unsigned>(err));
        out_error = buf;
        Shutdown();
        return false;
    }

    if ((err = aacEncoder_SetParam(m_enc, AACENC_AFTERBURNER, 1)) != AACENC_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "AACENC_AFTERBURNER failed: 0x%08X", static_cast<unsigned>(err));
        out_error = buf;
        Shutdown();
        return false;
    }

    // Trigger encoder initialization by calling aacEncEncode with NULL args.
    if ((err = aacEncEncode(m_enc, nullptr, nullptr, nullptr, nullptr)) != AACENC_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "aacEncEncode (init) failed: 0x%08X", static_cast<unsigned>(err));
        out_error = buf;
        Shutdown();
        return false;
    }

    // Read codec private (AudioSpecificConfig) immediately after init.
    AACENC_InfoStruct info2{};
    if ((err = aacEncInfo(m_enc, &info2)) != AACENC_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "aacEncInfo failed: 0x%08X", static_cast<unsigned>(err));
        out_error = buf;
        Shutdown();
        return false;
    }
    m_codec_private.assign(info2.confBuf, info2.confBuf + info2.confSize);

    m_sample_rate = sample_rate;
    m_channels = channels;
    m_frame_buf.clear();
    return true;
}

void FdkAacEncoder::FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns,
                                uint64_t& accumulated_frames, uint32_t sample_rate, uint32_t channels,
                                std::vector<EncodedAudioPacket>& out_packets) {
    if (m_enc == nullptr || data == nullptr || total_float_samples == 0) {
        return;
    }

    // Convert Float32 -> Int16 and append to internal accumulation buffer.
    const size_t prev_size = m_frame_buf.size();
    m_frame_buf.resize(prev_size + total_float_samples);
    for (size_t i = 0; i < total_float_samples; ++i) {
        m_frame_buf[prev_size + i] = static_cast<int16_t>(std::clamp(data[i], -1.0f, 1.0f) * 32767.0f);
    }

    const size_t frame_samples = static_cast<size_t>(kFrameSizeSamples) * channels;

    while (m_frame_buf.size() >= frame_samples) {
        // Build input buffer descriptor (PCM INT16).
        void* in_buf_ptr = m_frame_buf.data();
        INT in_buf_id = IN_AUDIO_DATA;
        INT in_buf_size = static_cast<INT>(frame_samples * sizeof(int16_t));
        INT in_buf_el_size = sizeof(int16_t);

        AACENC_BufDesc in_buf{};
        in_buf.numBufs = 1;
        in_buf.bufs = &in_buf_ptr;
        in_buf.bufferIdentifiers = &in_buf_id;
        in_buf.bufSizes = &in_buf_size;
        in_buf.bufElSizes = &in_buf_el_size;

        // Build output buffer descriptor.
        std::vector<uint8_t> out_bytes(8192);
        void* out_buf_ptr = out_bytes.data();
        INT out_buf_id = OUT_BITSTREAM_DATA;
        INT out_buf_size = static_cast<INT>(out_bytes.size());
        INT out_buf_el_size = 1;

        AACENC_BufDesc out_buf{};
        out_buf.numBufs = 1;
        out_buf.bufs = &out_buf_ptr;
        out_buf.bufferIdentifiers = &out_buf_id;
        out_buf.bufSizes = &out_buf_size;
        out_buf.bufElSizes = &out_buf_el_size;

        AACENC_InArgs in_args{};
        in_args.numInSamples = static_cast<INT>(frame_samples);
        in_args.numAncBytes = 0;

        AACENC_OutArgs out_args{};

        AACENC_ERROR enc_err = aacEncEncode(m_enc, &in_buf, &out_buf, &in_args, &out_args);
        if (enc_err != AACENC_OK && enc_err != AACENC_ENCODE_EOF) {
            // Encoding error — discard this frame and continue.
            m_frame_buf.erase(m_frame_buf.begin(), m_frame_buf.begin() + frame_samples);
            accumulated_frames += static_cast<uint64_t>(kFrameSizeSamples);
            continue;
        }

        if (out_args.numOutBytes > 0) {
            EncodedAudioPacket pkt;
            pkt.pts_ns = pts_ns + accumulated_frames * 1000000000ULL / sample_rate;
            pkt.bytes.assign(out_bytes.begin(), out_bytes.begin() + out_args.numOutBytes);
            out_packets.push_back(std::move(pkt));
        }

        accumulated_frames += static_cast<uint64_t>(kFrameSizeSamples);
        m_accumulated_frames += static_cast<uint64_t>(kFrameSizeSamples);
        m_frame_buf.erase(m_frame_buf.begin(), m_frame_buf.begin() + frame_samples);
    }
}

void FdkAacEncoder::DrainEncoder(std::vector<EncodedAudioPacket>& out_packets) {
    if (m_enc == nullptr) {
        return;
    }

    // Signal EOS by setting numInSamples = -1.
    AACENC_BufDesc in_buf{};
    in_buf.numBufs = 0;
    in_buf.bufs = nullptr;
    in_buf.bufferIdentifiers = nullptr;
    in_buf.bufSizes = nullptr;
    in_buf.bufElSizes = nullptr;

    AACENC_InArgs in_args{};
    in_args.numInSamples = -1;
    in_args.numAncBytes = 0;

    for (;;) {
        std::vector<uint8_t> out_bytes(8192);
        void* out_buf_ptr = out_bytes.data();
        INT out_buf_id = OUT_BITSTREAM_DATA;
        INT out_buf_size = static_cast<INT>(out_bytes.size());
        INT out_buf_el_size = 1;

        AACENC_BufDesc out_buf{};
        out_buf.numBufs = 1;
        out_buf.bufs = &out_buf_ptr;
        out_buf.bufferIdentifiers = &out_buf_id;
        out_buf.bufSizes = &out_buf_size;
        out_buf.bufElSizes = &out_buf_el_size;

        AACENC_OutArgs out_args{};

        AACENC_ERROR enc_err = aacEncEncode(m_enc, &in_buf, &out_buf, &in_args, &out_args);
        if (out_args.numOutBytes == 0) {
            break;
        }
        if (enc_err != AACENC_OK && enc_err != AACENC_ENCODE_EOF) {
            break;
        }

        EncodedAudioPacket pkt;
        pkt.pts_ns = (m_sample_rate > 0) ? m_accumulated_frames * 1000000000ULL / m_sample_rate : 0;
        m_accumulated_frames += static_cast<uint64_t>(kFrameSizeSamples);
        pkt.bytes.assign(out_bytes.begin(), out_bytes.begin() + out_args.numOutBytes);
        out_packets.push_back(std::move(pkt));
    }
}

void FdkAacEncoder::Flush(std::vector<EncodedAudioPacket>& out_packets) {
    if (m_enc == nullptr) {
        return;
    }

    // Pad the remaining samples in m_frame_buf to a full frame with zeros, then encode.
    if (!m_frame_buf.empty()) {
        const size_t frame_samples = static_cast<size_t>(kFrameSizeSamples) * m_channels;
        if (m_frame_buf.size() < frame_samples) {
            m_frame_buf.resize(frame_samples, static_cast<int16_t>(0));
        }

        void* in_buf_ptr = m_frame_buf.data();
        INT in_buf_id = IN_AUDIO_DATA;
        INT in_buf_size = static_cast<INT>(frame_samples * sizeof(int16_t));
        INT in_buf_el_size = static_cast<INT>(sizeof(int16_t));

        AACENC_BufDesc in_buf{};
        in_buf.numBufs = 1;
        in_buf.bufs = &in_buf_ptr;
        in_buf.bufferIdentifiers = &in_buf_id;
        in_buf.bufSizes = &in_buf_size;
        in_buf.bufElSizes = &in_buf_el_size;

        std::vector<uint8_t> out_bytes(8192);
        void* out_buf_ptr = out_bytes.data();
        INT out_buf_id = OUT_BITSTREAM_DATA;
        INT out_buf_size = static_cast<INT>(out_bytes.size());
        INT out_buf_el_size = 1;

        AACENC_BufDesc out_buf{};
        out_buf.numBufs = 1;
        out_buf.bufs = &out_buf_ptr;
        out_buf.bufferIdentifiers = &out_buf_id;
        out_buf.bufSizes = &out_buf_size;
        out_buf.bufElSizes = &out_buf_el_size;

        AACENC_InArgs in_args{};
        in_args.numInSamples = static_cast<INT>(frame_samples);
        in_args.numAncBytes = 0;

        AACENC_OutArgs out_args{};

        AACENC_ERROR enc_err = aacEncEncode(m_enc, &in_buf, &out_buf, &in_args, &out_args);
        if ((enc_err == AACENC_OK || enc_err == AACENC_ENCODE_EOF) && out_args.numOutBytes > 0) {
            EncodedAudioPacket pkt;
            pkt.pts_ns = (m_sample_rate > 0) ? m_accumulated_frames * 1000000000ULL / m_sample_rate : 0;
            m_accumulated_frames += static_cast<uint64_t>(kFrameSizeSamples);
            pkt.bytes.assign(out_bytes.begin(), out_bytes.begin() + out_args.numOutBytes);
            out_packets.push_back(std::move(pkt));
        }

        m_frame_buf.clear();
    }

    DrainEncoder(out_packets);
}

std::vector<uint8_t> FdkAacEncoder::CodecPrivateBytes() const {
    return m_codec_private;
}

void FdkAacEncoder::Shutdown() {
    if (m_enc != nullptr) {
        aacEncClose(&m_enc);
        m_enc = nullptr;
    }
    m_sample_rate = 0;
    m_channels = 0;
    m_accumulated_frames = 0;
    m_codec_private.clear();
    m_frame_buf.clear();
}

} // namespace recorder_core
