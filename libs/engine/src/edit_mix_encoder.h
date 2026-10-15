#pragma once

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/interfaces/IAudioEncoder.h>

#include "ffmpeg_aac_encoder.h"
#include "matroska_stream_writer.h"
#include "pcm_audio_encoder.h"

#include <cstdint>
#include <memory>
#include <string>

namespace exosnap::engine {

// The 48 kHz stereo result-mix encoder shared by rendered and copied-video
// editor exports: PCM24 for Matroska, AAC-LC 192 kbit/s for the MP4 profile.
// Packets keep the encoder's own timestamps, whose first AAC packet carries the
// priming samples; the track declares that codec delay so that decoders and the
// MP4 remux drop it and the first input sample lands at timeline zero.
struct EditMixEncoder {
    std::unique_ptr<IAudioEncoder> encoder;
    bool aac = false;

    bool Init(bool use_aac, std::string& error) {
        aac = use_aac;
        if (aac) {
            auto created = std::make_unique<FfmpegAacEncoder>();
            created->SetBitrateKbps(192);
            encoder = std::move(created);
        } else {
            auto created = std::make_unique<PcmAudioEncoder>();
            created->SetBitDepth(24);
            encoder = std::move(created);
        }
        return encoder->Init(48000, 2, error);
    }

    // Describes the single mix track in both the writer configuration and the
    // manifest, which the writer cross-checks before opening the file.
    void Configure(MatroskaStreamConfig& config, AudioManifest1& manifest) const {
        config.audio_codec = aac ? StreamAudioCodec::Aac : StreamAudioCodec::Pcm;
        config.audio_sample_rate = 48000;
        config.audio_channels = 2;
        config.audio_bit_depth = 24;
        config.audio_float = false;
        config.audio_tracks[0].codec_private = encoder->CodecPrivateBytes();
        config.audio_tracks[0].codec_delay_samples = encoder->CodecDelaySamples();
        if (manifest.tracks.empty())
            return;
        auto& codec = manifest.tracks[0].container.codec;
        codec.codec_id = aac ? "A_AAC" : "A_PCM/INT/LIT";
        codec.codec_private = config.audio_tracks[0].codec_private;
        codec.bit_depth = aac ? 0 : 24;
        codec.codec_delay_samples = encoder->CodecDelaySamples();
    }
};

} // namespace exosnap::engine
