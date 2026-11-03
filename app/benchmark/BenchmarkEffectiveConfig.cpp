#include "BenchmarkEffectiveConfig.h"

#include <exosnap/engine/gamepad_sprite_data.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/visual_scene_runtime.h>

#include <QCryptographicHash>

namespace exosnap::benchmark {
namespace {

using namespace exosnap::engine;

const char* Name(Container value) {
    switch (value) {
    case Container::WebM:
        return "webm";
    case Container::Matroska:
        return "mkv";
    case Container::Mp4:
        return "mp4";
    }
    return "unknown";
}

const char* Name(VideoCodec value) {
    switch (value) {
    case VideoCodec::Av1:
        return "av1";
    case VideoCodec::H264:
        return "h264";
    case VideoCodec::Hevc:
        return "hevc";
    }
    return "unknown";
}

const char* Name(AudioCodec value) {
    switch (value) {
    case AudioCodec::Aac:
        return "aac";
    case AudioCodec::Opus:
        return "opus";
    case AudioCodec::Pcm:
        return "pcm";
    case AudioCodec::Flac:
        return "flac";
    }
    return "unknown";
}

const char* Name(ChromaSubsampling value) {
    switch (value) {
    case ChromaSubsampling::Cs420:
        return "420";
    case ChromaSubsampling::Cs444:
        return "444";
    }
    return "unknown";
}

const char* Name(BitDepth value) {
    switch (value) {
    case BitDepth::Bit8:
        return "8";
    case BitDepth::Bit10:
        return "10";
    }
    return "unknown";
}

const char* Name(HdrMode value) {
    switch (value) {
    case HdrMode::Off:
        return "off";
    case HdrMode::TonemapSdr:
        return "tonemap-sdr";
    case HdrMode::Hdr10:
        return "hdr10";
    }
    return "unknown";
}

const char* Name(RateControlMode value) {
    switch (value) {
    case RateControlMode::ConstantQuality:
        return "cq";
    case RateControlMode::VariableBitrate:
        return "vbr";
    case RateControlMode::ConstantBitrate:
        return "cbr";
    case RateControlMode::Lossless:
        return "lossless";
    }
    return "unknown";
}

const char* Name(NvencPreset value) {
    switch (value) {
    case NvencPreset::P1:
        return "p1";
    case NvencPreset::P2:
        return "p2";
    case NvencPreset::P3:
        return "p3";
    case NvencPreset::P4:
        return "p4";
    case NvencPreset::P5:
        return "p5";
    case NvencPreset::P6:
        return "p6";
    case NvencPreset::P7:
        return "p7";
    }
    return "unknown";
}

const char* Name(FramePacingMode value) {
    switch (value) {
    case FramePacingMode::Smooth:
        return "smooth";
    case FramePacingMode::Newest:
        return "newest";
    }
    return "unknown";
}

const char* Name(OutputFitMode value) {
    switch (value) {
    case OutputFitMode::Contain:
        return "contain";
    }
    return "unknown";
}

const char* Name(MicChannelMode value) {
    switch (value) {
    case MicChannelMode::Auto:
        return "auto";
    case MicChannelMode::PreserveStereo:
        return "preserve-stereo";
    case MicChannelMode::MonoMix:
        return "mono-mix";
    case MicChannelMode::LeftToStereo:
        return "left-to-stereo";
    case MicChannelMode::RightToStereo:
        return "right-to-stereo";
    }
    return "unknown";
}

const char* Name(OpusFrameDuration value) {
    switch (value) {
    case OpusFrameDuration::Ms2_5:
        return "2.5ms";
    case OpusFrameDuration::Ms5:
        return "5ms";
    case OpusFrameDuration::Ms10:
        return "10ms";
    case OpusFrameDuration::Ms20:
        return "20ms";
    }
    return "unknown";
}

const char* Name(AudioSourceKind value) {
    switch (value) {
    case AudioSourceKind::App:
        return "app";
    case AudioSourceKind::Mic:
        return "mic";
    case AudioSourceKind::Sys:
        return "sys";
    case AudioSourceKind::SystemOutput:
        return "sysout";
    }
    return "unknown";
}

// Floats are rounded before they reach the digest. Two runs configured from the
// same source can differ in the last bits of a float that travelled through a
// percentage widget, and a fingerprint that flips on 0.4000001 vs 0.4 rejects
// pairs that are in fact identical.
QString Real(double value, int decimals = 4) {
    return QString::number(value, 'f', decimals);
}

class FieldBuilder {
  public:
    void Add(const char* key, const QString& value) {
        fields_ << (QLatin1String(key) + QLatin1Char('=') + value);
    }
    void Add(const char* key, const char* value) {
        Add(key, QString::fromLatin1(value));
    }
    void Add(const char* key, bool value) {
        Add(key, value ? "true" : "false");
    }
    void Add(const char* key, qint64 value) {
        Add(key, QString::number(value));
    }
    void AddReal(const char* key, double value, int decimals = 4) {
        Add(key, Real(value, decimals));
    }
    [[nodiscard]] QStringList Take() {
        return std::move(fields_);
    }

  private:
    QStringList fields_;
};

bool DescribeVisualScene(FieldBuilder& builder, VisualSceneRuntime& runtime) {
    const auto registered = runtime.RegisterConsumer();
    if (!registered.succeeded())
        return false;
    struct SceneSample {
        VisualSceneRuntime& runtime;
        VisualConsumerId consumer;
        SceneReadLease scene;
        ~SceneSample() {
            scene.Reset();
            runtime.UnregisterConsumer(consumer);
        }
    } sample{runtime, *registered.value(), {}};
    if (runtime.TryReadScene(sample.consumer, sample.scene) != ReadResult::Updated)
        return false;

    builder.Add("visual_scene_sample", "available");
    builder.Add("visual_item_count", static_cast<qint64>(sample.scene.Items().size()));
    for (const auto& item : sample.scene.Items()) {
        const auto& properties = item.properties;
        const auto& placement = properties.placement;
        builder.Add("visual_item_binding", static_cast<qint64>(item.binding_index));
        builder.Add("visual_item_visible", properties.output_visible);
        builder.Add("visual_item_order", static_cast<qint64>(properties.order));
        builder.AddReal("visual_item_x", placement.x);
        builder.AddReal("visual_item_y", placement.y);
        builder.AddReal("visual_item_w", placement.w);
        builder.AddReal("visual_item_h", placement.h);
        builder.Add("visual_item_mirror", placement.mirror);
        builder.AddReal("visual_item_opacity", properties.opacity, 3);
        builder.Add("visual_item_chroma", item.chroma_key.enabled);
        if (item.chroma_key.enabled) {
            builder.Add("visual_item_chroma_rgb", QString::number(item.chroma_key.r) + QLatin1Char(',') +
                                                      QString::number(item.chroma_key.g) + QLatin1Char(',') +
                                                      QString::number(item.chroma_key.b));
            builder.AddReal("visual_item_chroma_tolerance", item.chroma_key.tolerance, 3);
            builder.AddReal("visual_item_chroma_softness", item.chroma_key.softness, 3);
            builder.AddReal("visual_item_chroma_spill", item.chroma_key.spill_reduction, 3);
        }
    }
    builder.Add("visual_binding_count", static_cast<qint64>(sample.scene.Bindings().size()));
    const auto addAtlasRect = [&](const char* key, const SpriteAtlasRect& rect) {
        builder.Add(key, QString::number(rect.x) + QLatin1Char(',') + QString::number(rect.y) + QLatin1Char(',') +
                             QString::number(rect.w) + QLatin1Char(',') + QString::number(rect.h));
    };
    const auto addContentRect = [&](const char* key, const SpriteContentRect& rect) {
        builder.Add(key, Real(rect.x) + QLatin1Char(',') + Real(rect.y) + QLatin1Char(',') + Real(rect.w) +
                             QLatin1Char(',') + Real(rect.h));
    };
    for (const auto& binding : sample.scene.Bindings()) {
        if (const auto* image = std::get_if<CpuImageBinding>(&binding.content)) {
            builder.Add("visual_binding_type", "cpu-image");
            builder.Add("visual_image_width", static_cast<qint64>(image->layout.width));
            builder.Add("visual_image_height", static_cast<qint64>(image->layout.height));
            builder.Add("visual_image_format", static_cast<qint64>(image->layout.format));
            builder.Add("visual_image_matrix", static_cast<qint64>(image->layout.matrix));
            builder.Add("visual_image_range", static_cast<qint64>(image->layout.range));
        } else {
            const auto& pad = std::get<GamepadBinding>(binding.content);
            builder.Add("visual_binding_type", "gamepad");
            builder.Add("visual_gamepad_sprites", pad.sprites != nullptr);
            builder.Add("visual_gamepad_history", pad.history != nullptr);
            if (!pad.sprites)
                continue;
            const auto& sprites = *pad.sprites;
            builder.Add("visual_gamepad_atlas", sprites.atlas != nullptr);
            if (sprites.atlas) {
                builder.Add("visual_atlas_width", static_cast<qint64>(sprites.atlas->width));
                builder.Add("visual_atlas_height", static_cast<qint64>(sprites.atlas->height));
                const auto pixels = QByteArrayView(reinterpret_cast<const char*>(sprites.atlas->pixels.data()),
                                                   static_cast<qsizetype>(sprites.atlas->pixels.size()));
                builder.Add("visual_atlas_sha256",
                            QString::fromLatin1(QCryptographicHash::hash(pixels, QCryptographicHash::Sha256).toHex()));
            }
            builder.Add("visual_gamepad_part_count", static_cast<qint64>(sprites.parts.size()));
            for (const auto& part : sprites.parts) {
                addAtlasRect("visual_part_idle", part.idle);
                addAtlasRect("visual_part_pressed", part.pressed);
                addContentRect("visual_part_bounds", part.bounds);
                builder.Add("visual_part_button", static_cast<qint64>(part.button));
                builder.Add("visual_part_motion", static_cast<qint64>(part.motion));
                builder.Add("visual_part_auxiliary", static_cast<qint64>(part.auxiliary));
                builder.AddReal("visual_part_travel_x", part.travel_x);
                builder.AddReal("visual_part_travel_y", part.travel_y);
                builder.Add("visual_part_control", static_cast<qint64>(part.control));
                builder.Add("visual_part_click_control", static_cast<qint64>(part.click_control));
                builder.Add("visual_part_canonical", part.canonical);
                addAtlasRect("visual_part_knockout", part.knockout);
                addContentRect("visual_part_knockout_bounds", part.knockout_bounds);
            }
        }
    }
    return true;
}

} // namespace

EffectiveRecordingConfig UnavailableEffectiveConfig() {
    return EffectiveRecordingConfig{};
}

EffectiveRecordingConfig DescribeEffectiveConfig(const exosnap::engine::RecorderConfig& config) {
    FieldBuilder builder;

    // ---- Container / codecs -------------------------------------------------
    builder.Add("container", Name(config.container));
    builder.Add("video_codec", Name(config.video_codec));
    builder.Add("audio_codec", Name(config.audio_codec));
    builder.Add("chroma", Name(config.chroma));
    builder.Add("bit_depth", Name(config.bit_depth));
    builder.Add("hdr_mode", Name(config.hdr_mode));

    // ---- Colour signalling --------------------------------------------------
    builder.Add("color_primaries", static_cast<qint64>(config.color.primaries));
    builder.Add("color_transfer", static_cast<qint64>(config.color.transfer));
    builder.Add("color_matrix", static_cast<qint64>(config.color.matrix));
    builder.Add("color_range", static_cast<qint64>(config.color.range));
    builder.Add("color_bits_per_channel", static_cast<qint64>(config.color.bits_per_channel));
    builder.Add("color_hdr", config.color.hdr);

    // ---- Rate control -------------------------------------------------------
    builder.Add("rate_control", Name(config.rate_control_mode));
    builder.Add("cq", static_cast<qint64>(config.cq));
    builder.Add("bitrate_kbps", static_cast<qint64>(config.target_bitrate_kbps));
    if (const NvencTuning* nvenc = GetNvencTuning(config.backend_tuning)) {
        builder.Add("nvenc_preset", Name(nvenc->preset));
        builder.Add("nvenc_bframes", static_cast<qint64>(nvenc->bframes));
        builder.Add("nvenc_b_ref_mode", nvenc->b_ref_mode == NvencBRefMode::Each     ? QStringLiteral("each")
                                        : nvenc->b_ref_mode == NvencBRefMode::Middle ? QStringLiteral("middle")
                                                                                     : QStringLiteral("off"));
        builder.Add("nvenc_lookahead", nvenc->lookahead);
        builder.Add("nvenc_lookahead_depth", static_cast<qint64>(nvenc->lookahead_depth));
        builder.Add("nvenc_spatial_aq", nvenc->spatial_aq);
        builder.Add("nvenc_temporal_aq", nvenc->temporal_aq);
        builder.Add("nvenc_multipass", nvenc->multipass == NvencMultipass::QuarterResolution ? QStringLiteral("quarter")
                                       : nvenc->multipass == NvencMultipass::FullResolution  ? QStringLiteral("full")
                                                                                            : QStringLiteral("single"));
    } else {
        builder.Add("nvenc_preset", QStringLiteral("none"));
    }
    builder.AddReal("keyframe_interval_secs", config.keyframe_interval_secs, 3);

    // ---- Timing / geometry --------------------------------------------------
    builder.Add("frame_rate",
                QString::number(config.frame_rate_num) + QLatin1Char('/') + QString::number(config.frame_rate_den));
    builder.Add("cfr", config.cfr);
    builder.Add("frame_pacing", Name(config.cfr_pacing_mode));
    builder.Add("output_width", static_cast<qint64>(config.output_width));
    builder.Add("output_height", static_cast<qint64>(config.output_height));
    builder.Add("output_fit", Name(config.output_fit));
    builder.Add("capture_cursor", config.capture_cursor);
    builder.Add("crop_region", config.crop_region.has_value());

    // ---- Audio --------------------------------------------------------------
    builder.Add("record_audio", config.record_audio);
    const auto audio_track_count = !config.record_audio                     ? 0u
                                   : config.resolved_audio_plan             ? config.resolved_audio_plan->tracks.size()
                                   : config.audio_track_plan.tracks.empty() ? 1u
                                                                            : config.audio_track_plan.tracks.size();
    builder.Add("audio_track_count", static_cast<qint64>(audio_track_count));
    for (const ResolvedAudioTrack& track : config.audio_track_plan.tracks) {
        QStringList sources;
        sources.reserve(static_cast<qsizetype>(track.sources.size()));
        for (std::size_t index = 0; index < track.sources.size(); ++index) {
            const float gain = index < track.source_gain_linear.size() ? track.source_gain_linear[index] : 1.0f;
            sources << (QString::fromLatin1(Name(track.sources[index])) + QLatin1Char('@') + Real(gain, 3));
        }
        builder.Add("audio_track",
                    QString::number(track.track_index) + QLatin1Char(':') + sources.join(QLatin1Char('+')));
    }
    builder.Add("audio_bitrate_kbps", static_cast<qint64>(config.audio_bitrate_kbps));
    builder.Add("audio_sample_rate", static_cast<qint64>(config.audio_sample_rate));
    builder.Add("audio_channels", static_cast<qint64>(config.audio_channels));
    builder.Add("audio_bit_depth", static_cast<qint64>(config.audio_bit_depth));
    builder.Add("audio_pcm_float", config.audio_pcm_float);
    builder.Add("opus_frame_duration", Name(config.opus_frame_duration));
    builder.Add("opus_complexity", static_cast<qint64>(config.opus_complexity));
    builder.Add("flac_compression_level", static_cast<qint64>(config.flac_compression_level));
    builder.Add("audio_limiter", config.audio_limiter_enabled);
    builder.AddReal("audio_limiter_ceiling_db", config.audio_limiter_ceiling_db, 2);
    builder.Add("audio_clock_slaving", config.audio_clock_slaving_enabled);

    // Only the presence of a device selection, never the identifier: the two
    // machines-of-record in an archived comparison may enumerate the same
    // microphone under different endpoint ids.
    builder.Add("mic_device_selected", config.mic_device_id.has_value());
    builder.Add("mic_channel_mode", Name(config.mic_channel_mode));
    builder.AddReal("mic_gain_linear", config.mic_gain_linear, 3);
    builder.Add("mic_hpf", config.mic_hpf_enabled);
    builder.AddReal("mic_hpf_cutoff_hz", config.mic_hpf_cutoff_hz, 1);
    builder.Add("mic_gate", config.mic_gate_enabled);
    builder.AddReal("mic_gate_threshold_db", config.mic_gate_threshold_db, 2);
    builder.Add("mic_agc", config.mic_agc_enabled);
    builder.AddReal("mic_agc_target_db", config.mic_agc_target_db, 2);
    builder.Add("mic_rnnoise", config.mic_rnnoise_enabled);
    builder.Add("audio_target_pid_bound", config.audio_target_process_id.has_value());

    // This is a control-side scene sample, not acknowledgement of a recorded frame.
    builder.Add("visual_scene_attached", config.visual_scene != nullptr);
    const bool scene_available = !config.visual_scene || DescribeVisualScene(builder, *config.visual_scene);
    if (!scene_available)
        builder.Add("visual_scene_sample", "unavailable");

    // ---- Split --------------------------------------------------------------
    builder.Add("split_duration_ms", static_cast<qint64>(config.split.duration_ms));
    builder.Add("split_size_bytes", static_cast<qint64>(config.split.size_bytes));

    EffectiveRecordingConfig result;
    result.available = scene_available;
    result.fields = builder.Take();
    if (!result.available)
        return result;
    const QByteArray digest =
        QCryptographicHash::hash(result.fields.join(QLatin1Char('\n')).toUtf8(), QCryptographicHash::Sha256);
    result.fingerprint = QString::fromLatin1(digest.left(8).toHex());
    return result;
}

} // namespace exosnap::benchmark
