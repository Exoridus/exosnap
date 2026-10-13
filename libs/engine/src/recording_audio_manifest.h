#pragma once

#include "matroska_stream_writer.h"

namespace exosnap::engine {

inline Outcome<AudioManifest1> BuildRecordingAudioManifest(const ResolvedAudioPlan& plan,
                                                           const MatroskaStreamConfig& config,
                                                           const RecordingId& recording_id, uint64_t origin_r_ns) {
    AudioManifest1 manifest;
    manifest.recording_id = recording_id;
    manifest.media_id = NewId<MediaId>();
    manifest.segment_id = NewId<SegmentId>();
    manifest.segment_origin_r_ns = static_cast<int64_t>(origin_r_ns);
    manifest.continuity_group = 1;
    manifest.default_monitor_track_ids = plan.default_monitor_track_ids;
    manifest.monitor_selection_origin = plan.monitor_selection_origin;
    for (const auto& source : plan.sources) {
        const auto& spec = source.spec;
        manifest.sources.push_back({spec.id, spec.channel_mode, spec.dsp, spec.post_dsp_gain_linear, spec.muted});
    }
    for (const auto& track : plan.tracks) {
        AudioManifestTrack output;
        auto& container = output.container;
        container.track_id = track.id;
        container.stream_index = track.mux_index;
        container.track_uid = 2 + track.mux_index.value;
        container.name = track.name;
        container.role = track.role == AudioTrackRole::Mix    ? "mix"
                         : track.role == AudioTrackRole::Stem ? "stem"
                                                              : "standard";
        const auto& encoded = config.audio_tracks[track.mux_index.value];
        const char* codec = config.audio_codec == StreamAudioCodec::Opus   ? "A_OPUS"
                            : config.audio_codec == StreamAudioCodec::Aac  ? "A_AAC"
                            : config.audio_codec == StreamAudioCodec::Flac ? "A_FLAC"
                            : config.audio_float                           ? "A_PCM/FLOAT/IEEE"
                                                                           : "A_PCM/INT/LIT";
        const bool lossless =
            config.audio_codec == StreamAudioCodec::Pcm || config.audio_codec == StreamAudioCodec::Flac;
        container.codec = {codec,
                           encoded.codec_private,
                           config.audio_sample_rate,
                           config.audio_channels,
                           lossless ? config.audio_bit_depth : 0,
                           encoded.codec_delay_samples,
                           config.audio_channels == 1 ? "mono" : "stereo"};
        if (config.audio_codec == StreamAudioCodec::Opus && encoded.codec_private.size() >= 12)
            container.codec.codec_delay_samples =
                encoded.codec_private[10] | (static_cast<uint32_t>(encoded.codec_private[11]) << 8);
        for (const auto& route : track.routes) {
            container.source_ids.push_back(route.source_id);
            output.routes.push_back({route.id, route.source_id, route.gain_linear});
        }
        output.fixed_average_divisor = track.fixed_average_divisor;
        output.limiter = track.limiter;
        manifest.tracks.push_back(std::move(output));
    }
    const auto valid = ValidateAudioManifest1(manifest);
    if (!valid.succeeded())
        return *valid.error();
    return manifest;
}

} // namespace exosnap::engine
