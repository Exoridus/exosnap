#include "models/RecordingFailurePolicy.h"

#include "ui/CodecLabels.h"

namespace exosnap::models {

std::optional<RecordingFailureReport> BuildRecordingFailureReport(const UiRecordingResult& result) {
    if (result.succeeded)
        return std::nullopt;
    // The disk-space auto-stop already has an actionable "Storage running low"
    // notification with a Change folder action. A second, modal telling of the
    // same event would report what the user has just been told and offer less.
    if (IsDiskSpaceAutoStop(result))
        return std::nullopt;

    RecordingFailureReport report;
    // Whether anything reached disk decides the whole framing: "could not start"
    // and "stopped unexpectedly" send the user to different places.
    const bool has_partial = result.output_file_bytes > 0;
    report.title =
        has_partial ? QStringLiteral("Recording stopped unexpectedly") : QStringLiteral("Recording could not start");
    report.summary = has_partial
                         ? QStringLiteral("The recording was interrupted before it finished. A partial file may "
                                          "have been saved to your output folder.")
                         : QStringLiteral("ExoSnap couldn't start this recording. The details below may help "
                                          "identify why.");
    report.phase = QString::fromStdWString(result.error_phase);
    report.code = QString::fromStdWString(result.hresult_text);
    report.detail = QString::fromStdWString(result.error_detail);
    report.container = ui::containerLabel(result.container);
    report.video_codec = ui::videoCodecLabel(result.video_codec);
    report.audio_codec = ui::audioCodecLabel(result.audio_codec);
    return report;
}

} // namespace exosnap::models
