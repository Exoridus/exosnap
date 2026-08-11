#include "EditContextFactory.h"

#include "ui/CodecLabels.h"
#include "viewmodels/RecordViewModel.h"

#include <QStringList>

namespace exosnap {
namespace {

using exosnap::ui::audioCodecLabel;
using exosnap::ui::containerLabel;
using exosnap::ui::frameRateLabel;
using exosnap::ui::videoCodecLabel;

} // namespace

EditContext MakeEditContext(const CompletedRecording& recording) {
    EditContext ctx;
    ctx.output_path = recording.file_path;
    // Best-effort fallback: correct for an MKV recording, where the output IS
    // the edit master. An MP4 recording keeps its master elsewhere and only the
    // live session knows the path — see MakeEditContextForCurrentSession.
    ctx.mkv_master_path = recording.file_path;
    ctx.duration = QString::fromStdWString(RecordViewModel::FormatElapsed(recording.totalDurationSeconds()));
    ctx.duration_seconds = recording.totalDurationSeconds();
    ctx.size =
        recording.totalSizeBytes() > 0
            ? QString::fromStdWString(RecordViewModel::FormatBytes(static_cast<uint64_t>(recording.totalSizeBytes())))
            : QString{};
    if (recording.output_width > 0 && recording.output_height > 0)
        ctx.resolution = QStringLiteral("%1x%2").arg(recording.output_width).arg(recording.output_height);
    ctx.fps = frameRateLabel(recording.frame_rate_num, recording.frame_rate_den) + QStringLiteral(" ") +
              (recording.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR"));
    ctx.video_codec = videoCodecLabel(recording.video_codec);
    ctx.audio_codec = audioCodecLabel(recording.audio_codec);
    ctx.container = containerLabel(recording.container);
    ctx.markers = recording.markers;
    ctx.marker_sidecar_path = recording.marker_sidecar_path;
    return ctx;
}

EditContext MakeEditContextForCurrentSession(const CompletedRecording& recording, const QString& mkv_master_path,
                                             double peak_av_drift_ms, bool av_drift_available,
                                             const recorder_core::RecordingDiagnosticsSnapshot& completed_snapshot) {
    EditContext ctx = MakeEditContext(recording);
    // An empty master path means the session never learned one; keep the
    // fallback rather than handing the player an empty file name.
    if (!mkv_master_path.isEmpty())
        ctx.mkv_master_path = mkv_master_path;
    ctx.peak_av_drift_ms = peak_av_drift_ms;
    ctx.av_drift_available = av_drift_available;
    ctx.completed_snapshot = completed_snapshot;
    return ctx;
}

EditContext MakeMinimalEditContext(const QString& output_path) {
    EditContext ctx;
    ctx.output_path = output_path;
    ctx.mkv_master_path = output_path;
    return ctx;
}

} // namespace exosnap
