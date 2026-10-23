#pragma once

// Generates the ambiguous-transport source the Edit audio-choice tests need: a
// foreign Matroska file with one H.264 video stream and two unnamed stereo PCM
// streams that carry no ExoSnap manifest, so the editor cannot tell the mix
// from the sources and has to ask. The video packets are the first closed GOP
// (two seconds) of the committed reordered_h264.mp4; both audio streams hold a
// constant -12 dBFS signal with an unspecified channel order. The output is
// bit-exact, so every run imports the same bytes.

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

namespace exosnap::quick::test {

inline bool WriteTransportAmbiguousFixture(const std::filesystem::path& source, const std::filesystem::path& target) {
    constexpr int kRate = 48000;
    constexpr int64_t kBlockMs = 10;
    constexpr int64_t kDurationMs = 2000;
    constexpr float kLevel = 0.25F;

    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, source.string().c_str(), nullptr, nullptr) < 0)
        return false;
    bool ok = avformat_find_stream_info(input, nullptr) >= 0;
    const int video_index = ok ? av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
    ok = ok && video_index >= 0;

    AVFormatContext* output = nullptr;
    ok = ok && avformat_alloc_output_context2(&output, nullptr, "matroska", nullptr) >= 0 && output != nullptr;
    AVStream* video = ok ? avformat_new_stream(output, nullptr) : nullptr;
    std::array<AVStream*, 2> audio{};
    ok = ok && video != nullptr && avcodec_parameters_copy(video->codecpar, input->streams[video_index]->codecpar) >= 0;
    if (ok) {
        output->flags |= AVFMT_FLAG_BITEXACT;
        video->codecpar->codec_tag = 0;
        // Tagged like a recording, so composed preview and render export accept it.
        video->codecpar->color_range = AVCOL_RANGE_MPEG;
        video->codecpar->color_primaries = AVCOL_PRI_BT709;
        video->codecpar->color_trc = AVCOL_TRC_BT709;
        video->codecpar->color_space = AVCOL_SPC_BT709;
        video->time_base = input->streams[video_index]->time_base;
        for (auto*& stream : audio) {
            stream = avformat_new_stream(output, nullptr);
            if (stream == nullptr) {
                ok = false;
                break;
            }
            stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            stream->codecpar->codec_id = AV_CODEC_ID_PCM_F32LE;
            stream->codecpar->format = AV_SAMPLE_FMT_FLT;
            stream->codecpar->sample_rate = kRate;
            stream->codecpar->bits_per_coded_sample = 32;
            stream->codecpar->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
            stream->codecpar->ch_layout.nb_channels = 2;
            stream->time_base = AVRational{1, 1000};
        }
    }
    if (ok) {
        audio.front()->disposition = AV_DISPOSITION_DEFAULT;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "default_mode", "passthrough", 0);
        ok = avio_open(&output->pb, target.string().c_str(), AVIO_FLAG_WRITE) >= 0 &&
             avformat_write_header(output, &options) >= 0;
        av_dict_free(&options);
    }

    AVPacket* packet = av_packet_alloc();
    AVPacket* audio_packet = av_packet_alloc();
    ok = ok && packet != nullptr && audio_packet != nullptr;
    constexpr int kBlockFrames = static_cast<int>(kRate * kBlockMs / 1000);
    constexpr int kBlockBytes = kBlockFrames * 2 * static_cast<int>(sizeof(float));
    int64_t next_audio_ms = 0;
    const auto write_audio_until = [&](int64_t end_ms) {
        while (ok && next_audio_ms < std::min(end_ms, kDurationMs)) {
            for (auto* stream : audio) {
                ok = ok && av_new_packet(audio_packet, kBlockBytes) >= 0;
                if (!ok)
                    return;
                auto* samples = reinterpret_cast<float*>(audio_packet->data);
                std::fill(samples, samples + kBlockFrames * 2, kLevel);
                audio_packet->stream_index = stream->index;
                audio_packet->pts = audio_packet->dts =
                    av_rescale_q(next_audio_ms, AVRational{1, 1000}, stream->time_base);
                audio_packet->duration = av_rescale_q(kBlockMs, AVRational{1, 1000}, stream->time_base);
                audio_packet->flags |= AV_PKT_FLAG_KEY;
                ok = av_interleaved_write_frame(output, audio_packet) >= 0;
            }
            next_audio_ms += kBlockMs;
        }
    };
    const auto input_base = ok ? input->streams[video_index]->time_base : AVRational{1, 1};
    while (ok && av_read_frame(input, packet) >= 0) {
        if (packet->stream_index != video_index ||
            av_rescale_q(packet->pts, input_base, AVRational{1, 1000}) >= kDurationMs) {
            av_packet_unref(packet);
            continue;
        }
        write_audio_until(
            av_rescale_q(packet->dts == AV_NOPTS_VALUE ? packet->pts : packet->dts, input_base, AVRational{1, 1000}));
        packet->stream_index = video->index;
        av_packet_rescale_ts(packet, input_base, video->time_base);
        ok = ok && av_interleaved_write_frame(output, packet) >= 0;
        av_packet_unref(packet);
    }
    write_audio_until(kDurationMs);

    av_packet_free(&audio_packet);
    av_packet_free(&packet);
    if (ok)
        ok = av_write_trailer(output) >= 0;
    if (output != nullptr)
        avio_closep(&output->pb);
    avformat_free_context(output);
    avformat_close_input(&input);
    return ok;
}

// The generated fixture for this process, written once on first use. Tests
// that must not touch it (rename, append, sidecar) copy it first. Empty when
// generation failed.
inline QString TransportAmbiguousFixture() {
    static QTemporaryDir directory;
    static const QString path = [] {
        if (!directory.isValid())
            return QString();
        const auto source =
            QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
                .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
        const auto target = directory.filePath(QStringLiteral("transport-ambiguous.mkv"));
        return WriteTransportAmbiguousFixture(std::filesystem::path(source.toStdWString()),
                                              std::filesystem::path(target.toStdWString()))
                   ? target
                   : QString();
    }();
    return path;
}

} // namespace exosnap::quick::test
