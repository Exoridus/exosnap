#pragma once

#include <QString>

#include <cstdint>

#include <capability/codec_selection.h>
#include <capability/config_types.h>
#include <exosnap/engine/codec_types.h>

#include "../models/OutputSettingsModel.h"

// Canonical user-visible labels for codecs / containers / formats / frame rates.
//
// Single source of truth — keep ALL display labels routed through here so the
// two enum families (capability:: and exosnap::engine::) never drift apart again
// (a WEBM-vs-WebB divergence between ConfigPage and RecordPage is exactly what
// motivated this). Casing canon (see feedback_codec_naming_canon): acronyms are
// ALL-CAPS (MKV, MP4, AAC, PCM, FLAC, HEVC, AV1); proper names keep their
// canonical casing (Opus never OPUS, WebM never WEBM); H.264 keeps the dot.
// File EXTENSIONS are not labels — those stay lowercase with a dot (.mkv/.mp4/
// .webm) and live in FilenameBuilder, not here.

namespace exosnap::ui {

inline QString containerLabel(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return QStringLiteral("MKV");
    case capability::Container::Mp4:
        return QStringLiteral("MP4");
    case capability::Container::WebM:
        return QStringLiteral("WebM");
    }
    return QStringLiteral("MKV");
}

inline QString containerLabel(exosnap::engine::Container container) {
    switch (container) {
    case exosnap::engine::Container::Matroska:
        return QStringLiteral("MKV");
    case exosnap::engine::Container::Mp4:
        return QStringLiteral("MP4");
    case exosnap::engine::Container::WebM:
        return QStringLiteral("WebM");
    }
    return QStringLiteral("MKV");
}

inline QString videoCodecLabel(capability::VideoCodec codec) {
    // Delegate to the pure (non-Qt) spelling canon so the engine/diagnostics layer
    // and the UI can never drift (feedback_codec_naming_canon).
    const std::string_view label = capability::VisibleVideoCodecLabel(codec);
    return QString::fromUtf8(label.data(), static_cast<int>(label.size()));
}

inline QString videoCodecLabel(exosnap::engine::VideoCodec codec) {
    switch (codec) {
    case exosnap::engine::VideoCodec::H264:
        return QStringLiteral("H.264");
    case exosnap::engine::VideoCodec::Hevc:
        return QStringLiteral("HEVC");
    case exosnap::engine::VideoCodec::Av1:
        return QStringLiteral("AV1");
    }
    return QStringLiteral("AV1");
}

inline QString audioCodecLabel(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::Opus:
        return QStringLiteral("Opus");
    case capability::AudioCodec::Aac:
        return QStringLiteral("AAC");
    case capability::AudioCodec::Pcm:
        return QStringLiteral("PCM");
    case capability::AudioCodec::Flac:
        return QStringLiteral("FLAC");
    }
    return QStringLiteral("AAC");
}

inline QString audioCodecLabel(exosnap::engine::AudioCodec codec) {
    switch (codec) {
    case exosnap::engine::AudioCodec::Opus:
        return QStringLiteral("Opus");
    case exosnap::engine::AudioCodec::Aac:
        return QStringLiteral("AAC");
    case exosnap::engine::AudioCodec::Pcm:
        return QStringLiteral("PCM");
    case exosnap::engine::AudioCodec::Flac:
        return QStringLiteral("FLAC");
    }
    return QStringLiteral("AAC");
}

inline QString frameRateLabel(uint32_t numerator, uint32_t denominator) {
    if (numerator == 0 || denominator == 0) {
        return QStringLiteral("60 fps");
    }
    if (denominator == 1) {
        return QStringLiteral("%1 fps").arg(numerator);
    }
    return QStringLiteral("%1/%2 fps").arg(numerator).arg(denominator);
}

inline QString resolutionLabel(const OutputResolutionSettings& resolution) {
    if (resolution.mode == OutputResolutionMode::Custom && resolution.custom_width > 0 &&
        resolution.custom_height > 0) {
        return QStringLiteral("%1\xC3\x97%2").arg(resolution.custom_width).arg(resolution.custom_height);
    }
    return QString::fromWCharArray(OutputResolutionModeName(resolution.mode));
}

} // namespace exosnap::ui
