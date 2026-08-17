#include "SettingsAutomationKeys.h"

#include "SettingsAdapter.h"

#include <QJsonArray>

#include <array>

namespace exosnap::quick {
namespace settings_automation {
namespace {

// --- Enum token tables ------------------------------------------------------
//
// One entry per enumerator, wire token first. The token is the PRODUCT's own
// spelling; the int beside it is what the adapter's setter takes, which is
// static_cast<Enum>(value) in every case. Keeping the pair together here is what
// stops the enumerator ORDER from becoming part of the protocol: reordering the
// enum changes the ints in this table and nothing a client sees.
struct EnumEntry {
    const char* token;
    int value;
};

template <std::size_t N> QStringList TokensOf(const std::array<EnumEntry, N>& entries) {
    QStringList tokens;
    for (const EnumEntry& entry : entries)
        tokens.append(QString::fromLatin1(entry.token));
    return tokens;
}

template <std::size_t N> QJsonValue TokenForValue(const std::array<EnumEntry, N>& entries, int value) {
    for (const EnumEntry& entry : entries) {
        if (entry.value == value)
            return QString::fromLatin1(entry.token);
    }
    // An int the table does not know is not a token to invent: it is a value the
    // product grew and this table did not.
    return QJsonValue(QJsonValue::Null);
}

template <std::size_t N>
bool ValueForToken(const std::array<EnumEntry, N>& entries, const QJsonValue& value, int* out, QString* error) {
    const QString token = value.toString();
    for (const EnumEntry& entry : entries) {
        if (token == QLatin1String(entry.token)) {
            *out = entry.value;
            return true;
        }
    }
    *error = QStringLiteral("\"%1\" is not one of %2").arg(token, TokensOf(entries).join(QLatin1String(", ")));
    return false;
}

const std::array<EnumEntry, 3> kContainers{{
    {"MKV", static_cast<int>(capability::Container::Matroska)},
    {"MP4", static_cast<int>(capability::Container::Mp4)},
    {"WebM", static_cast<int>(capability::Container::WebM)},
}};

const std::array<EnumEntry, 3> kVideoCodecs{{
    {"AV1", static_cast<int>(capability::VideoCodec::Av1)},
    {"HEVC", static_cast<int>(capability::VideoCodec::Hevc)},
    {"H.264", static_cast<int>(capability::VideoCodec::H264)},
}};

const std::array<EnumEntry, 4> kAudioCodecs{{
    {"Opus", static_cast<int>(capability::AudioCodec::Opus)},
    {"AAC", static_cast<int>(capability::AudioCodec::Aac)},
    {"PCM", static_cast<int>(capability::AudioCodec::Pcm)},
    {"FLAC", static_cast<int>(capability::AudioCodec::Flac)},
}};

const std::array<EnumEntry, 2> kBitDepths{{
    {"8", static_cast<int>(capability::BitDepth::Bit8)},
    {"10", static_cast<int>(capability::BitDepth::Bit10)},
}};

const std::array<EnumEntry, 3> kChroma{{
    {"4:2:0", static_cast<int>(capability::ChromaSubsampling::Cs420)},
    {"4:2:2", static_cast<int>(capability::ChromaSubsampling::Cs422)},
    {"4:4:4", static_cast<int>(capability::ChromaSubsampling::Cs444)},
}};

const std::array<EnumEntry, 2> kColorRanges{{
    {"full", static_cast<int>(capability::ColorRange::Full)},
    {"limited", static_cast<int>(capability::ColorRange::Limited)},
}};

const std::array<EnumEntry, 3> kHdrModes{{
    {"off", static_cast<int>(recorder_core::HdrMode::Off)},
    {"tonemapSdr", static_cast<int>(recorder_core::HdrMode::TonemapSdr)},
    {"hdr10", static_cast<int>(recorder_core::HdrMode::Hdr10)},
}};

const std::array<EnumEntry, 7> kEncoderPresets{{
    {"P1", static_cast<int>(recorder_core::NvencPreset::P1)},
    {"P2", static_cast<int>(recorder_core::NvencPreset::P2)},
    {"P3", static_cast<int>(recorder_core::NvencPreset::P3)},
    {"P4", static_cast<int>(recorder_core::NvencPreset::P4)},
    {"P5", static_cast<int>(recorder_core::NvencPreset::P5)},
    {"P6", static_cast<int>(recorder_core::NvencPreset::P6)},
    {"P7", static_cast<int>(recorder_core::NvencPreset::P7)},
}};

const std::array<EnumEntry, 4> kRateControls{{
    {"constantQuality", static_cast<int>(recorder_core::RateControlMode::ConstantQuality)},
    {"variableBitrate", static_cast<int>(recorder_core::RateControlMode::VariableBitrate)},
    {"constantBitrate", static_cast<int>(recorder_core::RateControlMode::ConstantBitrate)},
    {"lossless", static_cast<int>(recorder_core::RateControlMode::Lossless)},
}};

const std::array<EnumEntry, 2> kFramePacing{{
    {"smooth", static_cast<int>(recorder_core::FramePacingMode::Smooth)},
    {"newest", static_cast<int>(recorder_core::FramePacingMode::Newest)},
}};

const std::array<EnumEntry, 3> kKeyframeIntervals{{
    {"2s", static_cast<int>(KeyframeIntervalMode::Seconds2)},
    {"1s", static_cast<int>(KeyframeIntervalMode::Seconds1)},
    {"0.5s", static_cast<int>(KeyframeIntervalMode::Seconds0_5)},
}};

const std::array<EnumEntry, 6> kResolutionModes{{
    {"native", static_cast<int>(OutputResolutionMode::Native)},
    {"uhd2160", static_cast<int>(OutputResolutionMode::UHD2160)},
    {"qhd1440", static_cast<int>(OutputResolutionMode::QHD1440)},
    {"fhd1080", static_cast<int>(OutputResolutionMode::FHD1080)},
    {"hd720", static_cast<int>(OutputResolutionMode::HD720)},
    {"custom", static_cast<int>(OutputResolutionMode::Custom)},
}};

const std::array<EnumEntry, 5> kSplitModes{{
    {"off", static_cast<int>(SplitRecordingMode::Off)},
    {"every15min", static_cast<int>(SplitRecordingMode::Every15Min)},
    {"every30min", static_cast<int>(SplitRecordingMode::Every30Min)},
    {"every60min", static_cast<int>(SplitRecordingMode::Every60Min)},
    {"custom", static_cast<int>(SplitRecordingMode::Custom)},
}};

// --- Accessor factories ------------------------------------------------------
//
// Four shapes, one per value type. Each one binds a pair of SettingsAdapter
// member functions -- the same pair the QML control is bound to -- so a write
// here and a click there take the identical path.

using IntGetter = int (SettingsAdapter::*)() const noexcept;
using IntSetter = void (SettingsAdapter::*)(int);
using BoolGetter = bool (SettingsAdapter::*)() const noexcept;
using BoolSetter = void (SettingsAdapter::*)(bool);

KeyDescriptor BoolKey(const char* key, const char* description, BoolGetter get, BoolSetter set) {
    KeyDescriptor descriptor;
    descriptor.key = QString::fromLatin1(key);
    descriptor.type = ValueType::Bool;
    descriptor.description = QString::fromLatin1(description);
    descriptor.read = [get](const SettingsAdapter& adapter) { return QJsonValue((adapter.*get)()); };
    descriptor.write = [set](SettingsAdapter& adapter, const QJsonValue& value, QString* error) {
        if (!value.isBool()) {
            *error = QStringLiteral("expected a boolean");
            return false;
        }
        (adapter.*set)(value.toBool());
        return true;
    };
    return descriptor;
}

KeyDescriptor IntKey(const char* key, const char* description, IntGetter get, IntSetter set) {
    KeyDescriptor descriptor;
    descriptor.key = QString::fromLatin1(key);
    descriptor.type = ValueType::Int;
    descriptor.description = QString::fromLatin1(description);
    descriptor.read = [get](const SettingsAdapter& adapter) { return QJsonValue((adapter.*get)()); };
    descriptor.write = [set](SettingsAdapter& adapter, const QJsonValue& value, QString* error) {
        if (!value.isDouble()) {
            *error = QStringLiteral("expected a number");
            return false;
        }
        // Range clamping is the adapter's -- and behind it the preset
        // sanitizer's -- job. A bound repeated here would be a second one.
        (adapter.*set)(static_cast<int>(value.toDouble()));
        return true;
    };
    return descriptor;
}

template <std::size_t N>
KeyDescriptor EnumKey(const char* key, const char* description, const std::array<EnumEntry, N>& entries, IntGetter get,
                      IntSetter set) {
    KeyDescriptor descriptor;
    descriptor.key = QString::fromLatin1(key);
    descriptor.type = ValueType::Enum;
    descriptor.allowed = TokensOf(entries);
    descriptor.description = QString::fromLatin1(description);
    descriptor.read = [get, &entries](const SettingsAdapter& adapter) {
        return TokenForValue(entries, (adapter.*get)());
    };
    descriptor.write = [set, &entries](SettingsAdapter& adapter, const QJsonValue& value, QString* error) {
        int resolved = 0;
        if (!ValueForToken(entries, value, &resolved, error))
            return false;
        (adapter.*set)(resolved);
        return true;
    };
    return descriptor;
}

using TextGetter = QString (SettingsAdapter::*)() const;
using TextSetter = void (SettingsAdapter::*)(const QString&);

// `get` and `set` by const reference rather than by value: a pointer to member
// function is not necessarily pointer-sized on MSVC (an inheritance thunk makes
// it wider), and this signature is the one cppcheck measures as large enough to
// matter. The lambdas below copy it once, which is what they need anyway.
KeyDescriptor TextKey(const char* key, const char* description, const TextGetter& get, const TextSetter& set,
                      QStringList allowed = {}) {
    KeyDescriptor descriptor;
    descriptor.key = QString::fromLatin1(key);
    descriptor.type = allowed.isEmpty() ? ValueType::Text : ValueType::Enum;
    descriptor.allowed = std::move(allowed);
    descriptor.description = QString::fromLatin1(description);
    descriptor.read = [get](const SettingsAdapter& adapter) { return QJsonValue((adapter.*get)()); };
    descriptor.write = [set, allowed = descriptor.allowed](SettingsAdapter& adapter, const QJsonValue& value,
                                                           QString* error) {
        if (!value.isString()) {
            *error = QStringLiteral("expected a string");
            return false;
        }
        if (!allowed.isEmpty() && !allowed.contains(value.toString())) {
            *error = QStringLiteral("\"%1\" is not one of %2").arg(value.toString(), allowed.join(QLatin1String(", ")));
            return false;
        }
        (adapter.*set)(value.toString());
        return true;
    };
    return descriptor;
}

QVector<KeyDescriptor> BuildKeys() {
    QVector<KeyDescriptor> keys;

    // --- Recording: container and codecs -------------------------------------
    keys.append(EnumKey("video.container", "Output container", kContainers, &SettingsAdapter::container,
                        &SettingsAdapter::setContainer));
    keys.append(EnumKey("video.videoCodec", "Video codec", kVideoCodecs, &SettingsAdapter::videoCodec,
                        &SettingsAdapter::setVideoCodec));
    keys.append(EnumKey("video.audioCodec", "Audio codec", kAudioCodecs, &SettingsAdapter::audioCodec,
                        &SettingsAdapter::setAudioCodec));

    // --- Recording: geometry and timing --------------------------------------
    keys.append(EnumKey("video.resolutionMode", "Output resolution mode", kResolutionModes,
                        &SettingsAdapter::resolutionMode, &SettingsAdapter::setResolutionMode));
    keys.append(IntKey("video.customWidth", "Custom output width in pixels", &SettingsAdapter::customWidth,
                       &SettingsAdapter::setCustomWidth));
    keys.append(IntKey("video.customHeight", "Custom output height in pixels", &SettingsAdapter::customHeight,
                       &SettingsAdapter::setCustomHeight));
    keys.append(IntKey("video.frameRate", "Target frame rate in fps", &SettingsAdapter::frameRate,
                       &SettingsAdapter::setFrameRate));
    keys.append(BoolKey("video.cfr", "Constant frame rate", &SettingsAdapter::cfr, &SettingsAdapter::setCfr));
    keys.append(EnumKey("video.framePacing", "CFR frame pacing mode", kFramePacing, &SettingsAdapter::framePacing,
                        &SettingsAdapter::setFramePacing));

    // --- Recording: encoder ---------------------------------------------------
    keys.append(EnumKey("video.encoderPreset", "NVENC speed/quality preset", kEncoderPresets,
                        &SettingsAdapter::encoderPreset, &SettingsAdapter::setEncoderPreset));
    keys.append(EnumKey("video.rateControl", "Rate-control mode", kRateControls, &SettingsAdapter::rateControl,
                        &SettingsAdapter::setRateControl));
    keys.append(IntKey("video.cq", "Constant-quality target (1 best .. 51 worst)", &SettingsAdapter::cq,
                       &SettingsAdapter::setCq));
    keys.append(IntKey("video.bitrateKbps", "Target bitrate for VBR/CBR", &SettingsAdapter::bitrateKbps,
                       &SettingsAdapter::setBitrateKbps));
    keys.append(EnumKey("video.keyframeInterval", "Keyframe interval", kKeyframeIntervals,
                        &SettingsAdapter::keyframeInterval, &SettingsAdapter::setKeyframeInterval));

    // --- Recording: colour ----------------------------------------------------
    keys.append(EnumKey("video.bitDepth", "Video bit depth", kBitDepths, &SettingsAdapter::bitDepth,
                        &SettingsAdapter::setBitDepth));
    keys.append(
        EnumKey("video.chroma", "Chroma subsampling", kChroma, &SettingsAdapter::chroma, &SettingsAdapter::setChroma));
    keys.append(EnumKey("video.colorRange", "Y'CbCr quantization range", kColorRanges, &SettingsAdapter::colorRange,
                        &SettingsAdapter::setColorRange));
    keys.append(EnumKey("video.hdrMode", "HDR handling mode", kHdrModes, &SettingsAdapter::hdrMode,
                        &SettingsAdapter::setHdrMode));
    keys.append(BoolKey("video.captureCursor", "Include the mouse cursor", &SettingsAdapter::captureCursor,
                        &SettingsAdapter::setCaptureCursor));

    // --- Audio ----------------------------------------------------------------
    keys.append(IntKey("audio.sampleRate", "Output sample rate in Hz", &SettingsAdapter::audioSampleRate,
                       &SettingsAdapter::setAudioSampleRate));
    keys.append(IntKey("audio.channels", "Output channel count", &SettingsAdapter::audioChannels,
                       &SettingsAdapter::setAudioChannels));
    keys.append(IntKey("audio.bitDepth", "Output bit depth for lossless codecs", &SettingsAdapter::audioBitDepth,
                       &SettingsAdapter::setAudioBitDepth));
    keys.append(IntKey("audio.bitrateKbps", "Audio bitrate", &SettingsAdapter::audioBitrateKbps,
                       &SettingsAdapter::setAudioBitrateKbps));
    keys.append(BoolKey("audio.systemEnabled", "Record the system audio source", &SettingsAdapter::systemAudioEnabled,
                        &SettingsAdapter::setSystemAudioEnabled));
    keys.append(BoolKey("audio.appEnabled", "Record the application audio source", &SettingsAdapter::appAudioEnabled,
                        &SettingsAdapter::setAppAudioEnabled));
    keys.append(BoolKey("audio.microphoneEnabled", "Record the microphone source", &SettingsAdapter::microphoneEnabled,
                        &SettingsAdapter::setMicrophoneEnabled));

    // --- Split ----------------------------------------------------------------
    keys.append(EnumKey("split.timeMode", "Automatic split by duration", kSplitModes, &SettingsAdapter::splitMode,
                        &SettingsAdapter::setSplitMode));
    keys.append(IntKey("split.customMinutes", "Custom split interval in minutes", &SettingsAdapter::splitCustomMinutes,
                       &SettingsAdapter::setSplitCustomMinutes));
    keys.append(IntKey("split.customSizeMb", "Custom split size threshold in MiB", &SettingsAdapter::splitCustomSizeMb,
                       &SettingsAdapter::setSplitCustomSizeMb));

    // --- Webcam ---------------------------------------------------------------
    keys.append(BoolKey("webcam.enabled", "Webcam picture-in-picture", &SettingsAdapter::webcamEnabled,
                        &SettingsAdapter::setWebcamEnabled));
    keys.append(BoolKey("webcam.mirror", "Mirror the webcam image", &SettingsAdapter::webcamMirror,
                        &SettingsAdapter::setWebcamMirror));

    // --- Application ----------------------------------------------------------
    // Grouped apart because they are what the user tells ExoSnap about ExoSnap,
    // not about a recording -- and they persist through a different store.
    keys.append(
        TextKey("app.appearance", "Appearance", &SettingsAdapter::appearanceId, &SettingsAdapter::setAppearanceId));
    keys.append(TextKey("app.accent", "Accent colour", &SettingsAdapter::accentId, &SettingsAdapter::setAccentId));
    keys.append(
        BoolKey("app.expertMode", "Expert mode", &SettingsAdapter::expertMode, &SettingsAdapter::setExpertMode));
    keys.append(BoolKey("app.showNotifications", "Show notification toasts", &SettingsAdapter::showNotifications,
                        &SettingsAdapter::setShowNotifications));
    keys.append(BoolKey("app.keepRunningInTray", "Close to tray", &SettingsAdapter::keepRunningInTray,
                        &SettingsAdapter::setKeepRunningInTray));
    keys.append(BoolKey("app.openEditorWhenFinished", "Open the editor after a recording",
                        &SettingsAdapter::openEditorWhenFinished, &SettingsAdapter::setOpenEditorWhenFinished));
    keys.append(BoolKey("app.checkUpdatesOnStart", "Check for updates on startup", &SettingsAdapter::autoUpdateCheck,
                        &SettingsAdapter::setAutoUpdateCheck));
    keys.append(TextKey("app.updateChannel", "Update channel", &SettingsAdapter::updateChannel,
                        &SettingsAdapter::setUpdateChannel, {QStringLiteral("Stable"), QStringLiteral("Preview")}));
    keys.append(BoolKey("app.showRecordingOverlay", "Recording status overlay", &SettingsAdapter::showRecordingOverlay,
                        &SettingsAdapter::setShowRecordingOverlay));
    keys.append(BoolKey("app.showDiagnosticsOverlay", "Live diagnostics overlay",
                        &SettingsAdapter::showDiagnosticsOverlay, &SettingsAdapter::setShowDiagnosticsOverlay));
    keys.append(BoolKey("app.showQuickControls", "Quick-control pill overlay", &SettingsAdapter::showQuickControls,
                        &SettingsAdapter::setShowQuickControls));
    keys.append(BoolKey("app.presentDiagnosticsOptIn", "Opt in to elevation-gated present diagnostics",
                        &SettingsAdapter::presentDiagnosticsOptIn, &SettingsAdapter::setPresentDiagnosticsOptIn));
    return keys;
}

} // namespace

QString ValueTypeName(ValueType type) {
    switch (type) {
    case ValueType::Bool:
        return QStringLiteral("bool");
    case ValueType::Int:
        return QStringLiteral("int");
    case ValueType::Number:
        return QStringLiteral("number");
    case ValueType::Enum:
        return QStringLiteral("enum");
    case ValueType::Text:
        return QStringLiteral("string");
    }
    return QStringLiteral("string");
}

const QVector<KeyDescriptor>& AllKeys() {
    static const QVector<KeyDescriptor> keys = BuildKeys();
    return keys;
}

const KeyDescriptor* FindKey(const QString& key) {
    for (const KeyDescriptor& descriptor : AllKeys()) {
        if (descriptor.key == key)
            return &descriptor;
    }
    return nullptr;
}

QJsonObject DescribeKeys() {
    QJsonArray described;
    for (const KeyDescriptor& descriptor : AllKeys()) {
        QJsonObject entry;
        entry.insert(QStringLiteral("key"), descriptor.key);
        entry.insert(QStringLiteral("type"), ValueTypeName(descriptor.type));
        entry.insert(QStringLiteral("description"), descriptor.description);
        if (!descriptor.allowed.isEmpty()) {
            QJsonArray allowed;
            for (const QString& value : descriptor.allowed)
                allowed.append(value);
            entry.insert(QStringLiteral("values"), allowed);
        }
        described.append(entry);
    }

    QJsonObject json;
    json.insert(QStringLiteral("keys"), described);
    json.insert(QStringLiteral("count"), described.size());
    // Stated so nobody has to discover it: a write is reconciled by the product,
    // and the reconciled value is what the key reads back as.
    json.insert(QStringLiteral("writeSemantics"), QStringLiteral("reconciledByProduct"));
    return json;
}

QJsonObject ReadKeys(const SettingsAdapter& adapter, const QString& key, QString* error) {
    QJsonObject values;
    if (key.isEmpty()) {
        for (const KeyDescriptor& descriptor : AllKeys())
            values.insert(descriptor.key, descriptor.read(adapter));
    } else {
        const KeyDescriptor* descriptor = FindKey(key);
        if (descriptor == nullptr) {
            if (error != nullptr)
                *error = QStringLiteral("No settings key named \"%1\"").arg(key);
            return {};
        }
        values.insert(descriptor->key, descriptor->read(adapter));
    }

    QJsonObject json;
    json.insert(QStringLiteral("values"), values);
    return json;
}

bool WriteKey(SettingsAdapter& adapter, const QString& key, const QJsonValue& value, QString* error) {
    const KeyDescriptor* descriptor = FindKey(key);
    if (descriptor == nullptr) {
        *error = QStringLiteral("No settings key named \"%1\"").arg(key);
        return false;
    }
    return descriptor->write(adapter, value, error);
}

} // namespace settings_automation
} // namespace exosnap::quick
