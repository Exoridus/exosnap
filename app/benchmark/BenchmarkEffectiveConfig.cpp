#include "BenchmarkEffectiveConfig.h"

#include <exosnap/engine/recorder_session.h>

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
    builder.Add("rate_control", Name(config.nvenc_rate_control));
    builder.Add("cq", static_cast<qint64>(config.cq));
    builder.Add("bitrate_kbps", static_cast<qint64>(config.nvenc_bitrate_kbps));
    builder.Add("encoder_preset", Name(config.nvenc_preset));
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
    builder.Add("audio_track_count", static_cast<qint64>(config.audio_track_plan.tracks.size()));
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

    // ---- Webcam -------------------------------------------------------------
    // The overlay's geometry and keying only enter the fingerprint when the
    // overlay is actually composited. A disabled webcam still carries whatever
    // position the owning frontend last persisted, and those defaults legitimately
    // differ (a bare coordinator has no settings store at all, so it reports
    // 0.0/0.0 where an application reports its saved corner). Hashing an inert
    // value would reject a pair that is recording exactly the same thing — the
    // fingerprint has to be sensitive to what changes the encode and to nothing
    // else, or operators learn to override it.
    //
    // frame_provider is a pointer and deliberately absent; whether a provider is
    // attached is what changes the encode, not its address.
    builder.Add("webcam", config.webcam.enabled);
    if (config.webcam.enabled) {
        builder.Add("webcam_provider_attached", config.webcam.frame_provider != nullptr);
        builder.AddReal("webcam_overlay_x", config.webcam.overlay_x_norm);
        builder.AddReal("webcam_overlay_y", config.webcam.overlay_y_norm);
        builder.AddReal("webcam_overlay_w", config.webcam.overlay_w_norm);
        builder.AddReal("webcam_overlay_h", config.webcam.overlay_h_norm);
        builder.Add("webcam_mirror", config.webcam.mirror);
        builder.AddReal("webcam_opacity", config.webcam.opacity, 3);
        builder.Add("webcam_chroma_key", config.webcam.chroma_key_enabled);
        if (config.webcam.chroma_key_enabled) {
            builder.Add("webcam_chroma_rgb", QString::number(config.webcam.chroma_r) + QLatin1Char(',') +
                                                 QString::number(config.webcam.chroma_g) + QLatin1Char(',') +
                                                 QString::number(config.webcam.chroma_b));
            builder.AddReal("webcam_chroma_tolerance", config.webcam.chroma_tolerance, 3);
            builder.AddReal("webcam_chroma_softness", config.webcam.chroma_softness, 3);
            builder.AddReal("webcam_chroma_spill", config.webcam.chroma_spill_reduction, 3);
        }
    }

    // ---- Split --------------------------------------------------------------
    builder.Add("split_duration_ms", static_cast<qint64>(config.split.duration_ms));
    builder.Add("split_size_bytes", static_cast<qint64>(config.split.size_bytes));

    EffectiveRecordingConfig result;
    result.available = true;
    result.fields = builder.Take();
    const QByteArray digest =
        QCryptographicHash::hash(result.fields.join(QLatin1Char('\n')).toUtf8(), QCryptographicHash::Sha256);
    result.fingerprint = QString::fromLatin1(digest.left(8).toHex());
    return result;
}

} // namespace exosnap::benchmark
