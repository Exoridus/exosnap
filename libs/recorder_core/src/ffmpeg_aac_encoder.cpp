#include "ffmpeg_aac_encoder.h"

#include <recorder_core/logging/logging.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <string>

namespace recorder_core {

namespace {
constexpr const char* kLogComponent = "ffmpeg_aac_encoder";

// The native FFmpeg AAC encoder consumes planar float. Hardcoding it avoids the
// AVCodec.sample_fmts / avcodec_get_supported_config churn across FFmpeg
// versions; avcodec_open2 still validates the choice.
constexpr AVSampleFormat kEncoderSampleFmt = AV_SAMPLE_FMT_FLTP;

std::string AvErr(int ret) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(ret, buf, sizeof(buf));
    return std::string(buf);
}

void LogWarn(const std::string& msg) {
    logging::log(logging::LogLevel::Warn, kLogComponent, msg);
}
} // namespace

// ---------------------------------------------------------------------------
// Bitrate resolution (unchanged by the ADR 0052 encoder swap: default
// 192 kbps, clamped to [64, 320]).
// ---------------------------------------------------------------------------

void FfmpegAacEncoder::SetBitrateKbps(uint32_t bitrate_kbps) noexcept {
    m_bitrate_kbps = bitrate_kbps;
}

/*static*/ uint32_t FfmpegAacEncoder::ResolveBitrateKbps(uint32_t kbps) noexcept {
    if (kbps == 0) {
        return kDefaultBitrateKbps;
    }
    return std::clamp(kbps, 64u, 320u);
}

FfmpegAacEncoder::~FfmpegAacEncoder() {
    Shutdown();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool FfmpegAacEncoder::Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) {
    Shutdown();

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        // Distinguishable, non-crashing failure. This is the expected outcome
        // against a decoder/mux-only avcodec build (exosnap-ffmpeg-build r4),
        // which compiles in zero encoders. The unit tests key off the
        // "avcodec_find_encoder" marker to tell this apart from a real error.
        out_error = "FFmpeg AAC encoder unavailable: avcodec_find_encoder(AV_CODEC_ID_AAC) returned null "
                    "(no AAC encoder compiled into this avcodec build)";
        return false;
    }

    m_ctx = avcodec_alloc_context3(codec);
    if (m_ctx == nullptr) {
        out_error = "avcodec_alloc_context3 failed for AAC encoder";
        return false;
    }

    m_ctx->sample_rate = static_cast<int>(sample_rate);
    m_ctx->bit_rate = static_cast<int64_t>(ResolveBitrateKbps(m_bitrate_kbps)) * 1000;
    m_ctx->sample_fmt = kEncoderSampleFmt;
    m_ctx->time_base = AVRational{1, static_cast<int>(sample_rate)};
    av_channel_layout_default(&m_ctx->ch_layout, static_cast<int>(channels));
    // Publish the AudioSpecificConfig as extradata rather than in-band, matching
    // the raw-AAC + CodecPrivate framing the Matroska/MP4 writers expect.
    m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(m_ctx, codec, nullptr);
    if (ret < 0) {
        out_error = "avcodec_open2 (AAC) failed: " + AvErr(ret);
        Shutdown();
        return false;
    }

    m_frame_size = (m_ctx->frame_size > 0) ? m_ctx->frame_size : kFrameSizeSamples;

    if (m_ctx->extradata != nullptr && m_ctx->extradata_size > 0) {
        m_codec_private.assign(m_ctx->extradata, m_ctx->extradata + m_ctx->extradata_size);
    }

    // Resampler: interleaved Float32 input -> planar float the encoder wants.
    // Same rate and layout, so this only de-interleaves; no rate conversion.
    AVChannelLayout in_layout;
    av_channel_layout_default(&in_layout, static_cast<int>(channels));
    ret = swr_alloc_set_opts2(&m_swr, &m_ctx->ch_layout, kEncoderSampleFmt, static_cast<int>(sample_rate), &in_layout,
                              AV_SAMPLE_FMT_FLT, static_cast<int>(sample_rate), 0, nullptr);
    av_channel_layout_uninit(&in_layout);
    if (ret < 0 || m_swr == nullptr) {
        out_error = "swr_alloc_set_opts2 failed: " + AvErr(ret);
        Shutdown();
        return false;
    }
    if ((ret = swr_init(m_swr)) < 0) {
        out_error = "swr_init failed: " + AvErr(ret);
        Shutdown();
        return false;
    }

    m_fifo = av_audio_fifo_alloc(kEncoderSampleFmt, static_cast<int>(channels), m_frame_size * 2);
    if (m_fifo == nullptr) {
        out_error = "av_audio_fifo_alloc failed";
        Shutdown();
        return false;
    }

    m_frame = av_frame_alloc();
    if (m_frame == nullptr) {
        out_error = "av_frame_alloc failed";
        Shutdown();
        return false;
    }
    m_frame->format = kEncoderSampleFmt;
    m_frame->sample_rate = static_cast<int>(sample_rate);
    m_frame->nb_samples = m_frame_size;
    av_channel_layout_copy(&m_frame->ch_layout, &m_ctx->ch_layout);
    if ((ret = av_frame_get_buffer(m_frame, 0)) < 0) {
        out_error = "av_frame_get_buffer failed: " + AvErr(ret);
        Shutdown();
        return false;
    }

    m_pkt = av_packet_alloc();
    if (m_pkt == nullptr) {
        out_error = "av_packet_alloc failed";
        Shutdown();
        return false;
    }

    m_sample_rate = sample_rate;
    m_channels = channels;
    m_input_samples = 0;
    m_output_samples = 0;
    m_pts_origin_ns = 0;
    m_pts_origin_set = false;
    return true;
}

// ---------------------------------------------------------------------------
// ReceiveAvailable — drain all packets the encoder currently has ready.
// ---------------------------------------------------------------------------

void FfmpegAacEncoder::ReceiveAvailable(uint64_t pts_origin_ns, std::vector<EncodedAudioPacket>& out_packets) {
    if (m_ctx == nullptr || m_pkt == nullptr) {
        return;
    }
    for (;;) {
        int ret = avcodec_receive_packet(m_ctx, m_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            LogWarn("avcodec_receive_packet failed: " + AvErr(ret));
            break;
        }

        // The native AAC encoder does not reliably round-trip AVFrame::pts through
        // avcodec_receive_packet for every packet (encoder priming/lookahead can
        // leave early packets at AV_NOPTS_VALUE) -- derive pts_ns from our own
        // running output-sample counter instead of trusting the codec's
        // returned packet pts.
        EncodedAudioPacket pkt;
        const uint64_t rate = (m_sample_rate > 0) ? m_sample_rate : 1;
        pkt.pts_ns = pts_origin_ns + m_output_samples * 1000000000ULL / rate;
        m_output_samples += static_cast<uint64_t>(kFrameSizeSamples);
        pkt.bytes.assign(m_pkt->data, m_pkt->data + m_pkt->size);
        out_packets.push_back(std::move(pkt));

        av_packet_unref(m_pkt);
    }
}

// ---------------------------------------------------------------------------
// FeedFloat32
// ---------------------------------------------------------------------------

void FfmpegAacEncoder::FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns,
                                   uint64_t& accumulated_frames, uint32_t /*sample_rate*/, uint32_t /*channels*/,
                                   std::vector<EncodedAudioPacket>& out_packets) {
    if (m_ctx == nullptr || m_swr == nullptr || m_fifo == nullptr || m_frame == nullptr || data == nullptr ||
        total_float_samples == 0 || m_channels == 0) {
        return;
    }

    if (!m_pts_origin_set) {
        m_pts_origin_ns = pts_ns;
        m_pts_origin_set = true;
    }

    const int in_samples = static_cast<int>(total_float_samples / m_channels); // per-channel
    if (in_samples <= 0) {
        return;
    }

    // De-interleave Float32 -> planar float into a temporary buffer.
    uint8_t** converted = nullptr;
    int linesize = 0;
    const int out_max = static_cast<int>(swr_get_out_samples(m_swr, in_samples));
    if (av_samples_alloc_array_and_samples(&converted, &linesize, static_cast<int>(m_channels), out_max,
                                           kEncoderSampleFmt, 0) < 0) {
        LogWarn("av_samples_alloc_array_and_samples failed");
        return;
    }

    const uint8_t* in_ptr = reinterpret_cast<const uint8_t*>(data);
    const int converted_samples = swr_convert(m_swr, converted, out_max, &in_ptr, in_samples);
    if (converted_samples > 0) {
        av_audio_fifo_write(m_fifo, reinterpret_cast<void**>(converted), converted_samples);
    }
    if (converted != nullptr) {
        av_freep(&converted[0]);
        av_freep(&converted);
    }

    // Encode every whole frame currently buffered.
    while (av_audio_fifo_size(m_fifo) >= m_frame_size) {
        if (av_frame_make_writable(m_frame) < 0) {
            break;
        }
        const int read = av_audio_fifo_read(m_fifo, reinterpret_cast<void**>(m_frame->data), m_frame_size);
        if (read < m_frame_size) {
            break;
        }
        m_frame->nb_samples = m_frame_size;
        m_frame->pts = static_cast<int64_t>(m_input_samples);
        m_input_samples += static_cast<uint64_t>(m_frame_size);

        int ret = avcodec_send_frame(m_ctx, m_frame);
        if (ret < 0) {
            LogWarn("avcodec_send_frame failed: " + AvErr(ret));
            break;
        }
        ReceiveAvailable(m_pts_origin_ns, out_packets);
        accumulated_frames += static_cast<uint64_t>(kFrameSizeSamples);
    }
}

// ---------------------------------------------------------------------------
// Flush — zero-pad the trailing partial frame, then EOS-drain the encoder.
// ---------------------------------------------------------------------------

void FfmpegAacEncoder::Flush(std::vector<EncodedAudioPacket>& out_packets) {
    if (m_ctx == nullptr || m_fifo == nullptr || m_frame == nullptr) {
        return;
    }

    const int remaining = av_audio_fifo_size(m_fifo);
    if (remaining > 0 && av_frame_make_writable(m_frame) >= 0) {
        // Silence the whole frame, then overwrite the leading part with the
        // remaining real samples — i.e. zero-pad the tail up to a full frame
        // (AAC-LC cannot encode a sub-frame; padding is the smaller loss).
        av_samples_set_silence(m_frame->data, 0, m_frame_size, static_cast<int>(m_channels), kEncoderSampleFmt);
        const int to_read = std::min(remaining, m_frame_size);
        av_audio_fifo_read(m_fifo, reinterpret_cast<void**>(m_frame->data), to_read);
        m_frame->nb_samples = m_frame_size;
        m_frame->pts = static_cast<int64_t>(m_input_samples);
        m_input_samples += static_cast<uint64_t>(m_frame_size);

        int ret = avcodec_send_frame(m_ctx, m_frame);
        if (ret < 0) {
            LogWarn("avcodec_send_frame (flush pad) failed: " + AvErr(ret));
        } else {
            ReceiveAvailable(m_pts_origin_ns, out_packets);
        }
    }

    // EOS: a null frame flushes the encoder's internal delay line.
    int ret = avcodec_send_frame(m_ctx, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        LogWarn("avcodec_send_frame (EOS) failed: " + AvErr(ret));
        return;
    }
    ReceiveAvailable(m_pts_origin_ns, out_packets);
}

std::vector<uint8_t> FfmpegAacEncoder::CodecPrivateBytes() const {
    return m_codec_private;
}

// ---------------------------------------------------------------------------
// Shutdown — release every owned FFmpeg resource; safe to call repeatedly.
// ---------------------------------------------------------------------------

void FfmpegAacEncoder::Shutdown() {
    if (m_swr != nullptr) {
        swr_free(&m_swr);
    }
    if (m_fifo != nullptr) {
        av_audio_fifo_free(m_fifo);
        m_fifo = nullptr;
    }
    if (m_frame != nullptr) {
        av_frame_free(&m_frame);
    }
    if (m_pkt != nullptr) {
        av_packet_free(&m_pkt);
    }
    if (m_ctx != nullptr) {
        avcodec_free_context(&m_ctx);
    }

    m_sample_rate = 0;
    m_channels = 0;
    m_frame_size = kFrameSizeSamples;
    m_input_samples = 0;
    m_output_samples = 0;
    m_pts_origin_ns = 0;
    m_pts_origin_set = false;
    m_codec_private.clear();
}

} // namespace recorder_core
