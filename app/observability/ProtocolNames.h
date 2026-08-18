#pragma once

// ProtocolNames.h -- stable wire tokens for the enums the observability surfaces
// carry.
//
// Codec, container and audio-codec spellings are NOT re-invented here: they come
// from ui::CodecLabels, which is the project's single naming canon
// (feedback_codec_naming_canon), and they are spelling constants rather than
// translated copy -- there is no qsTr() anywhere in that header. A second table
// of codec names is exactly the drift that header exists to prevent.
//
// Everything else below has no canonical spelling yet, so it gets one here, and
// the rule for it is: lowerCamelCase, derived from the enumerator's MEANING, and
// never from its integer value. `ipc.describe` publishes these, so an enumerator
// order change must not be able to change what a client reads.

#include "ui/CodecLabels.h"

#include <capability/config_types.h>
#include <recorder_core/codec_types.h>
#include <recorder_core/frame_pacing.h>
#include <recorder_core/pipeline_diagnostics.h>

#include <QString>

namespace exosnap::observability {

[[nodiscard]] inline QString LifecycleName(recorder_core::DiagnosticsLifecycle value) {
    using recorder_core::DiagnosticsLifecycle;
    switch (value) {
    case DiagnosticsLifecycle::Idle:
        return QStringLiteral("idle");
    case DiagnosticsLifecycle::Initializing:
        return QStringLiteral("initializing");
    case DiagnosticsLifecycle::Recording:
        return QStringLiteral("recording");
    case DiagnosticsLifecycle::Paused:
        return QStringLiteral("paused");
    case DiagnosticsLifecycle::Stopping:
        return QStringLiteral("stopping");
    case DiagnosticsLifecycle::Completed:
        return QStringLiteral("completed");
    case DiagnosticsLifecycle::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("idle");
}

[[nodiscard]] inline QString CaptureSourceTypeName(recorder_core::CaptureSourceType value) {
    using recorder_core::CaptureSourceType;
    switch (value) {
    case CaptureSourceType::Display:
        return QStringLiteral("display");
    case CaptureSourceType::Window:
        return QStringLiteral("window");
    case CaptureSourceType::Region:
        return QStringLiteral("region");
    case CaptureSourceType::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

[[nodiscard]] inline QString PresentModeName(recorder_core::PresentMode value) {
    using recorder_core::PresentMode;
    switch (value) {
    case PresentMode::Composed:
        return QStringLiteral("composed");
    case PresentMode::IndependentFlip:
        return QStringLiteral("independentFlip");
    case PresentMode::ExclusiveFullscreen:
        return QStringLiteral("exclusiveFullscreen");
    case PresentMode::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

[[nodiscard]] inline QString SplitTriggerName(recorder_core::DiagnosticsSplitTrigger value) {
    using recorder_core::DiagnosticsSplitTrigger;
    switch (value) {
    case DiagnosticsSplitTrigger::None:
        return QStringLiteral("none");
    case DiagnosticsSplitTrigger::AutomaticDuration:
        return QStringLiteral("automaticDuration");
    case DiagnosticsSplitTrigger::AutomaticSize:
        return QStringLiteral("automaticSize");
    case DiagnosticsSplitTrigger::ManualButton:
        return QStringLiteral("manualButton");
    case DiagnosticsSplitTrigger::Hotkey:
        return QStringLiteral("hotkey");
    }
    return QStringLiteral("none");
}

[[nodiscard]] inline QString EncoderPresetName(recorder_core::NvencPreset value) {
    using recorder_core::NvencPreset;
    switch (value) {
    case NvencPreset::P1:
        return QStringLiteral("P1");
    case NvencPreset::P2:
        return QStringLiteral("P2");
    case NvencPreset::P3:
        return QStringLiteral("P3");
    case NvencPreset::P4:
        return QStringLiteral("P4");
    case NvencPreset::P5:
        return QStringLiteral("P5");
    case NvencPreset::P6:
        return QStringLiteral("P6");
    case NvencPreset::P7:
        return QStringLiteral("P7");
    }
    return QStringLiteral("P4");
}

[[nodiscard]] inline QString RateControlName(recorder_core::RateControlMode value) {
    using recorder_core::RateControlMode;
    switch (value) {
    case RateControlMode::ConstantQuality:
        return QStringLiteral("constantQuality");
    case RateControlMode::VariableBitrate:
        return QStringLiteral("variableBitrate");
    case RateControlMode::ConstantBitrate:
        return QStringLiteral("constantBitrate");
    case RateControlMode::Lossless:
        return QStringLiteral("lossless");
    }
    return QStringLiteral("constantQuality");
}

[[nodiscard]] inline QString ChromaName(recorder_core::ChromaSubsampling value) {
    return value == recorder_core::ChromaSubsampling::Cs444 ? QStringLiteral("4:4:4") : QStringLiteral("4:2:0");
}

[[nodiscard]] inline QString ChromaName(capability::ChromaSubsampling value) {
    switch (value) {
    case capability::ChromaSubsampling::Cs420:
        return QStringLiteral("4:2:0");
    case capability::ChromaSubsampling::Cs422:
        return QStringLiteral("4:2:2");
    case capability::ChromaSubsampling::Cs444:
        return QStringLiteral("4:4:4");
    }
    return QStringLiteral("4:2:0");
}

[[nodiscard]] inline int BitDepthValue(recorder_core::BitDepth value) noexcept {
    return value == recorder_core::BitDepth::Bit10 ? 10 : 8;
}

[[nodiscard]] inline int BitDepthValue(capability::BitDepth value) noexcept {
    return value == capability::BitDepth::Bit10 ? 10 : 8;
}

[[nodiscard]] inline QString ColorRangeName(bool full_range) {
    return full_range ? QStringLiteral("full") : QStringLiteral("limited");
}

[[nodiscard]] inline QString ColorRangeName(capability::ColorRange value) {
    return ColorRangeName(value == capability::ColorRange::Full);
}

[[nodiscard]] inline QString HdrModeName(recorder_core::HdrMode value) {
    using recorder_core::HdrMode;
    switch (value) {
    case HdrMode::Off:
        return QStringLiteral("off");
    case HdrMode::TonemapSdr:
        return QStringLiteral("tonemapSdr");
    case HdrMode::Hdr10:
        return QStringLiteral("hdr10");
    }
    return QStringLiteral("tonemapSdr");
}

[[nodiscard]] inline QString FramePacingName(recorder_core::FramePacingMode value) {
    return value == recorder_core::FramePacingMode::Newest ? QStringLiteral("newest") : QStringLiteral("smooth");
}

} // namespace exosnap::observability
