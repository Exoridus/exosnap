#pragma once

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <cstdint>
#include <filesystem>

inline bool RemuxTimestampMedia(const std::filesystem::path& destination, int64_t offset_us) {
    const auto source = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "timestamp_av.mkv";
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, source.string().c_str(), nullptr, nullptr) < 0)
        return false;
    AVFormatContext* output = nullptr;
    bool ok = avformat_find_stream_info(input, nullptr) >= 0 &&
              avformat_alloc_output_context2(&output, nullptr, "matroska", nullptr) >= 0 && output;
    for (unsigned i = 0; ok && i < input->nb_streams; ++i) {
        auto* stream = avformat_new_stream(output, nullptr);
        ok = stream && avcodec_parameters_copy(stream->codecpar, input->streams[i]->codecpar) >= 0;
        if (ok) {
            stream->codecpar->codec_tag = 0;
            stream->time_base = input->streams[i]->time_base;
            stream->avg_frame_rate = input->streams[i]->avg_frame_rate;
        }
    }
    if (ok) {
        output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
        output->max_delay = 0;
        ok = avio_open(&output->pb, destination.string().c_str(), AVIO_FLAG_WRITE) >= 0;
    }
    AVDictionary* options = nullptr;

    ok = ok && avformat_write_header(output, &options) >= 0;
    av_dict_free(&options);
    AVPacket* packet = av_packet_alloc();
    ok = ok && packet;
    while (ok && av_read_frame(input, packet) >= 0) {
        const auto index = packet->stream_index;
        const auto time_base = output->streams[index]->time_base;
        av_packet_rescale_ts(packet, input->streams[index]->time_base, time_base);
        const auto offset = av_rescale_q(offset_us, AVRational{1, AV_TIME_BASE}, time_base);
        if (packet->pts != AV_NOPTS_VALUE)
            packet->pts += offset;
        if (packet->dts != AV_NOPTS_VALUE)
            packet->dts += offset;
        packet->pos = -1;
        ok = av_interleaved_write_frame(output, packet) >= 0;
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (ok)
        ok = av_write_trailer(output) >= 0;
    if (output) {
        if (output->pb)
            avio_closep(&output->pb);
        avformat_free_context(output);
    }
    avformat_close_input(&input);
    return ok;
}
