#include "FixActionDispatcher.h"

#include "../models/RecordingPreset.h"

#include <capability/codec_selection.h>

namespace exosnap::diagnostics {

FixResult ApplyAutoFix(std::string_view fix_id, const capability::CapabilitySet& caps, OutputSettingsModel& output,
                       VideoSettingsModel& video) {
    if (fix_id == "fix.capture.monitor_instead") {
        // rec.capture.exclusive_window: the selected window is in exclusive
        // fullscreen and cannot be captured. Retargeting to the hosting monitor is
        // a capture-selection change, not a settings change, so the host performs it.
        return {FixOutcome::RetargetToHostingMonitor};
    }

    if (fix_id == "fix.frame_pacing.smooth") {
        video.frame_pacing = recorder_core::FramePacingMode::Smooth;
        return {FixOutcome::SettingsChanged};
    }

    if (fix_id == "fix.codec.video.default") {
        // rec.003: the configured video codec is unavailable. Fall back to the best
        // codec this GPU + container actually supports, never a blind H.264 —
        // H.264 stays only as the last-resort default.
        output.video_codec =
            capability::BestAvailableVideoCodec(caps, output.container).value_or(capability::VideoCodec::H264);
        return {FixOutcome::SettingsChanged};
    }

    if (fix_id == "fix.profile.codec.best") {
        // rec.profile.codec: the shared resolver picks the identical codec the
        // recommendation named, then the audio codec is reconciled for the result.
        if (const auto best = capability::BestAvailableVideoCodec(caps, output.container))
            output.video_codec = *best;
        ReconcileContainerCodecs(output);
        return {FixOutcome::SettingsChanged};
    }

    if (fix_id == "fix.codec.audio.default") {
        output.audio_codec = capability::AudioCodec::Aac;
        return {FixOutcome::SettingsChanged};
    }

    if (fix_id == "fix.audio.opus_to_aac") {
        output.audio_codec = capability::AudioCodec::Aac;
        ReconcileContainerCodecs(output);
        return {FixOutcome::SettingsChanged};
    }

    if (fix_id == "fix.color.range") {
        // rec.color.range: Full range is crushed in players that ignore the range
        // flag, so Limited is the compatible-everywhere choice.
        output.color_range = capability::ColorRange::Limited;
        return {FixOutcome::SettingsChanged};
    }

    if (fix_id == "fix.hdr.codec.av1" || fix_id == "fix.hdr.codec.hevc") {
        // rec.hdr.h264: H.264 has no HDR10-native path. The engine already chose
        // whichever of AV1/HEVC is GPU-selectable and keyed the fix id off that
        // choice, so apply exactly the codec the FixAction proposed.
        output.video_codec = fix_id == "fix.hdr.codec.av1" ? capability::VideoCodec::Av1 : capability::VideoCodec::Hevc;
        ReconcileContainerCodecs(output);
        return {FixOutcome::SettingsChanged};
    }

    return {FixOutcome::Unknown};
}

FixResult ResolveAssistedFix(std::string_view fix_id) {
    if (fix_id.empty())
        return {FixOutcome::Unknown};
    if (fix_id == "fix.output.change_folder" || fix_id == "fix.output.fat32_folder")
        return {FixOutcome::NavigateSettingsOutput};
    // fix.container.mkv / fix.fps.cap / fix.profile.select and any future assisted
    // fix land in the format & quality area.
    return {FixOutcome::NavigateSettingsFormat};
}

std::string_view SettingsSectionFor(FixOutcome outcome) noexcept {
    switch (outcome) {
    case FixOutcome::NavigateSettingsOutput:
        return "settings/output";
    case FixOutcome::NavigateSettingsFormat:
        return "settings/format";
    case FixOutcome::Unknown:
    case FixOutcome::SettingsChanged:
    case FixOutcome::RetargetToHostingMonitor:
        break;
    }
    return "";
}

} // namespace exosnap::diagnostics
