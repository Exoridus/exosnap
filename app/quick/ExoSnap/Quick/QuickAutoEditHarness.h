#pragma once

// Drives productive edit adapters and media pipelines. Pointer interaction and
// physical display/audio acceptance require separate tests.
// Available only in harness-enabled builds.

#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cmath>
#include <vector>

class QCoreApplication;
class QQuickWindow;

namespace exosnap::quick {

class QuickApplication;

// Samples are finite millisecond durations. The p99 requires at least 1,000 observations.
[[nodiscard]] inline QJsonObject AutoEditLatencySummary(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    QJsonObject result{{QStringLiteral("samples"), static_cast<int>(samples.size())},
                       {QStringLiteral("unit"), QStringLiteral("ms")},
                       {QStringLiteral("method"), QStringLiteral("nearest-rank")},
                       {QStringLiteral("minimumP99Samples"), 1000},
                       {QStringLiteral("p99Status"),
                        samples.size() >= 1000 ? QStringLiteral("available") : QStringLiteral("insufficient_samples")}};
    if (!samples.empty()) {
        result.insert(QStringLiteral("maximum"), samples.back());
        for (const auto p : {50, 95, 99}) {
            if (p == 99 && samples.size() < 1000)
                continue;
            const auto rank = static_cast<size_t>(std::ceil(p * samples.size() / 100.0));
            result.insert(QStringLiteral("p%1").arg(p), samples.at(rank - 1));
        }
    }
    return result;
}

struct AutoEditOptions {
    bool isolated_qualification = false;
    // Media to open. Empty means "whatever the preceding --auto-record produced",
    // which is how the end-to-end flow chains without a path round-trip.
    QString media_path;
    QString additional_media_path;
    // Clip length in seconds, for media_path runs only.
    //
    // This is not a shortcut around the decoder. The Editor takes its length from
    // the EditContext, and the product builds that context from the recording
    // session it just finished — a bare file path has no session, which is why
    // MakeMinimalEditContext leaves the duration at zero and why the "Edit"
    // notification action on a foreign path opens a zero-length timeline. Rather
    // than paper over that with a probe the product does not perform, the value
    // is supplied explicitly so a standalone run exercises trim and seek against
    // a real clip. The chained --auto-record path needs none of this: its context
    // is the genuine CompletedRecording.
    double media_duration_seconds = 0.0;
    // Trim window as a fraction of the clip, applied through the same
    // requestTrim() the drag handles call. Defaults cut a middle section, so the
    // exported duration differs from the source in BOTH directions and a
    // silently-ignored trim cannot pass.
    double trim_start_fraction = 0.25;
    double trim_end_fraction = 0.75;
    // Where the JSON evidence goes. Empty writes nothing and only sets the exit
    // code.
    QString report_path;
    // Skip the export leg. Used when the run only has to prove load/decode/seek.
    bool export_enabled = true;
    // Optional PNG of the Editor with the clip loaded, decoded thumbnails on the
    // strip, the real audio rows and the trim applied. This is the only way to
    // judge the Editor's layout honestly: the no-media visual harness renders an
    // empty transport row and a timeline with no clip edges, which is a state no
    // user reaches and a state no layout should be designed around.
    QString screenshot_path;
    // Window size for that capture. 860x700 is the product minimum.
    int screenshot_width = 0;
    int screenshot_height = 0;
};

// True when argv asks for an edit run at all.
[[nodiscard]] bool AutoEditRequested(const QStringList& args);

// Parses the --auto-edit* options. Returns false and fills `error` on a
// malformed value rather than silently substituting a default.
[[nodiscard]] bool ParseAutoEditOptions(const QStringList& args, AutoEditOptions* options, QString* error);

// Opens the clip, waits for the real decode to report a duration and its audio
// tracks, plays, seeks, trims and exports, then writes the report.
//
// Returns the process exit code: 0 when every stage passed, 1 on a stage
// failure, 2 when the clip never opened.
// `window` may be null; the screenshot stage is then reported as unavailable
// rather than silently skipped.
[[nodiscard]] int RunQuickAutoEdit(QCoreApplication& app, QuickApplication& application, QQuickWindow* window,
                                   const AutoEditOptions& options);

} // namespace exosnap::quick
