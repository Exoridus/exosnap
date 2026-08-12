#pragma once

// Deterministic drive loop for the Edit -> Export half of the product flow.
//
// Why this exists: `--auto-record` proves a recording is produced, and the
// visual harness proves the Editor RENDERS. Neither proves the Editor WORKS —
// that real media decodes, that seeking lands, that a trim is applied, and that
// an export writes a file a decoder will accept. The Qt Quick cutover made that
// gap load-bearing (ADR 0064): the whole edit surface was reimplemented, and a
// synthetic fixture cannot tell a working decode path from a broken one.
//
// What it drives: the same QObject adapters the QML binds to
// (EditSessionAdapter, EditPlayerAdapter, EditTimelineAdapter,
// EditExportAdapter) — the production objects, invoked through the same
// Q_INVOKABLE entry points the QML handlers call. It therefore covers the
// application's edit logic and the real FFmpeg decode/export path.
//
// What it deliberately does NOT cover: QML hit-testing. Whether a click at a
// given pixel reaches the right button is a rendering/input question this cannot
// answer and must not be reported as if it could. That stays a manual check.
//
// No synthetic input is generated and no window is activated, so a run cannot
// collide with whatever the developer is doing (CLAUDE.md, "Coordinate before
// driving the running application").
//
// Compiled only when EXOSNAP_ENABLE_AUTO_RECORD_HARNESS is defined (see
// EXOSNAP_BUILD_BENCHMARK_HARNESS in app/CMakeLists.txt).

#include <QString>

class QCoreApplication;
class QQuickWindow;

namespace exosnap::quick {

class QuickApplication;

struct AutoEditOptions {
    // Media to open. Empty means "whatever the preceding --auto-record produced",
    // which is how the end-to-end flow chains without a path round-trip.
    QString media_path;
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
