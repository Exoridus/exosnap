#include "QuickAutoEditHarness.h"

#include "EditExportAdapter.h"
#include "EditPlayerAdapter.h"
#include "EditSessionAdapter.h"
#include "EditTimelineAdapter.h"
#include "QuickApplication.h"

#include "models/EditContextFactory.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QSaveFile>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace exosnap::quick {
namespace {

// A keyframe scan and a thumbnail run both decode; on a 1440p AV1 clip that is
// seconds of real work, not a UI animation. Generous, and only a hang reaches it.
constexpr int kClipOpenTimeoutMs = 30000;
// An export remuxes the trimmed range. Bounded by the clip length rather than by
// encoding, so this covers a long clip on a slow disk.
constexpr int kExportTimeoutMs = 180000;
// Playback has to advance far enough that a stuck decoder is distinguishable
// from a slow one.
constexpr int kPlaybackObserveMs = 1500;
constexpr qint64 kMinPlaybackAdvanceMs = 200;

QString optionValue(const QStringList& args, const QString& name) {
    const int index = args.indexOf(name);
    if (index < 0 || index + 1 >= args.size())
        return {};
    return args.at(index + 1);
}

// Spins the event loop until `predicate` holds or the deadline passes. Returns
// whether the predicate held. Everything here is asynchronous by design — the
// decode runs on a worker thread and reports back through queued signals — so a
// harness that did not pump the loop would observe nothing at all.
template <typename Predicate> bool waitFor(const char* what, int timeout_ms, int poll_ms, Predicate predicate) {
    QElapsedTimer clock;
    clock.start();
    qint64 last_report_ms = 0;
    while (!predicate()) {
        const qint64 elapsed = clock.elapsed();
        if (elapsed >= timeout_ms) {
            qWarning().noquote() << QStringLiteral("auto-edit: gave up waiting for %1 after %2 ms")
                                        .arg(QString::fromLatin1(what))
                                        .arg(elapsed);
            return false;
        }
        // A stage that is progressing and a stage that is wedged look identical
        // from outside the process; without this a hung run is indistinguishable
        // from a slow decode until the whole gate times out.
        if (elapsed - last_report_ms >= 2000) {
            last_report_ms = elapsed;
            qInfo().noquote() << QStringLiteral("auto-edit: still waiting for %1 (%2 ms)")
                                     .arg(QString::fromLatin1(what))
                                     .arg(elapsed);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, poll_ms);
        QThread::msleep(static_cast<unsigned long>(std::max(1, poll_ms / 2)));
    }
    return true;
}

// Runs the event loop for a fixed interval. Not a sleep: the decode worker
// reports back through queued signals, so a blocking wait would observe a
// frozen adapter and call it a stalled player.
void pumpFor(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

struct StageLog {
    QJsonArray stages;
    bool ok = true;

    void record(const QString& name, bool passed, const QString& detail) {
        QJsonObject entry;
        entry.insert(QStringLiteral("stage"), name);
        entry.insert(QStringLiteral("passed"), passed);
        if (!detail.isEmpty())
            entry.insert(QStringLiteral("detail"), detail);
        stages.append(entry);
        if (!passed)
            ok = false;
        qInfo().noquote() << QStringLiteral("auto-edit: %1 %2%3")
                                 .arg(name, passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                                      detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail);
    }
};

void writeReport(const QString& path, const QJsonObject& report) {
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning().noquote() << QStringLiteral("auto-edit: could not write the report to %1").arg(path);
        return;
    }
    file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    if (!file.commit())
        qWarning().noquote() << QStringLiteral("auto-edit: could not commit the report to %1").arg(path);
}

} // namespace

bool AutoEditRequested(const QStringList& args) {
    return args.contains(QStringLiteral("--auto-edit"));
}

bool ParseAutoEditOptions(const QStringList& args, AutoEditOptions* options, QString* error) {
    *options = AutoEditOptions{};

    options->media_path = optionValue(args, QStringLiteral("--auto-edit-media"));
    options->report_path = optionValue(args, QStringLiteral("--auto-edit-report"));

    const QString duration = optionValue(args, QStringLiteral("--auto-edit-duration"));
    if (!duration.isEmpty()) {
        bool ok = false;
        options->media_duration_seconds = duration.toDouble(&ok);
        if (!ok || options->media_duration_seconds <= 0.0) {
            *error = QStringLiteral("--auto-edit-duration requires a positive number of seconds");
            return false;
        }
    }
    options->export_enabled = !args.contains(QStringLiteral("--auto-edit-no-export"));

    const QString trim = optionValue(args, QStringLiteral("--auto-edit-trim"));
    if (!trim.isEmpty()) {
        const QStringList parts = trim.split(QLatin1Char(','));
        bool start_ok = false;
        bool end_ok = false;
        const double start = parts.value(0).toDouble(&start_ok);
        const double end = parts.value(1).toDouble(&end_ok);
        if (parts.size() != 2 || !start_ok || !end_ok || start < 0.0 || end > 1.0 || start >= end) {
            *error = QStringLiteral("--auto-edit-trim requires start,end fractions with 0 <= start < end <= 1");
            return false;
        }
        options->trim_start_fraction = start;
        options->trim_end_fraction = end;
    }

    options->screenshot_path = optionValue(args, QStringLiteral("--auto-edit-screenshot"));
    const QString size = optionValue(args, QStringLiteral("--auto-edit-size"));
    if (!size.isEmpty()) {
        const QStringList parts = size.split(QLatin1Char('x'), Qt::SkipEmptyParts);
        bool w_ok = false;
        bool h_ok = false;
        const int width = parts.value(0).toInt(&w_ok);
        const int height = parts.value(1).toInt(&h_ok);
        if (parts.size() != 2 || !w_ok || !h_ok || width <= 0 || height <= 0) {
            *error = QStringLiteral("--auto-edit-size requires WIDTHxHEIGHT, e.g. 860x700");
            return false;
        }
        options->screenshot_width = width;
        options->screenshot_height = height;
    }
    return true;
}

int RunQuickAutoEdit(QCoreApplication& app, QuickApplication& application, QQuickWindow* window,
                     const AutoEditOptions& options) {
    Q_UNUSED(app);

    auto* session = application.editSessionAdapter();
    auto* player = application.editPlayerAdapter();
    auto* timeline = application.editTimelineAdapter();
    auto* exporter = application.editExportAdapter();
    if (session == nullptr || player == nullptr || timeline == nullptr || exporter == nullptr) {
        qCritical().noquote() << QStringLiteral("auto-edit: the Quick composition owner has no edit adapters");
        return 2;
    }

    StageLog log;
    QJsonObject report;

    // ---- open ----------------------------------------------------------
    // An explicit path opens that file through MakeMinimalEditContext, the same
    // call the "Edit" notification action uses. No path means the caller already
    // opened the editor — the --auto-record chain, where the context comes from
    // the real CompletedRecording and therefore carries duration, resolution and
    // markers the way the product does.
    if (!options.media_path.isEmpty()) {
        const QFileInfo info(options.media_path);
        if (!info.isFile()) {
            log.record(QStringLiteral("open"), false, QStringLiteral("no such file: %1").arg(options.media_path));
            writeReport(options.report_path,
                        QJsonObject{{QStringLiteral("stages"), log.stages}, {QStringLiteral("passed"), false}});
            return 2;
        }
        EditContext context = MakeMinimalEditContext(info.absoluteFilePath());
        context.duration_seconds = options.media_duration_seconds;
        session->setEditContext(context);
    } else if (!session->open()) {
        // Chained onto --auto-record: open the recording this process just made,
        // through the production handoff. Not left to the completion path's own
        // call, which is gated on the open-editor-when-finished preference — a
        // gate that passes or fails on a persisted user setting is not a gate.
        if (!application.openEditorForAutomation()) {
            log.record(QStringLiteral("open"), false, QStringLiteral("no completed recording to hand to the editor"));
            writeReport(options.report_path,
                        QJsonObject{{QStringLiteral("stages"), log.stages}, {QStringLiteral("passed"), false}});
            return 2;
        }
    }

    if (!session->open()) {
        log.record(QStringLiteral("open"), false, QStringLiteral("the edit session never opened a clip"));
        writeReport(options.report_path,
                    QJsonObject{{QStringLiteral("stages"), log.stages}, {QStringLiteral("passed"), false}});
        return 2;
    }
    report.insert(QStringLiteral("clipPath"), session->clipPath());
    log.record(QStringLiteral("open"), true, session->clipPath());

    // ---- decode --------------------------------------------------------
    // trimSnapReady is the keyframe scan's completion edge: it is set by a real
    // pass over the container's cue table, so waiting on it proves the file was
    // opened by the decoder rather than merely accepted as a string.
    const bool snap_ready =
        waitFor("the keyframe scan", kClipOpenTimeoutMs, 50, [session]() { return session->trimSnapReady(); });
    report.insert(QStringLiteral("keyframeCount"), static_cast<int>(session->keyframeTimestamps().size()));
    log.record(QStringLiteral("decode.keyframeScan"), snap_ready,
               QStringLiteral("%1 keyframes").arg(session->keyframeTimestamps().size()));

    const qint64 duration_ms = session->durationMs();
    report.insert(QStringLiteral("durationMs"), duration_ms);
    log.record(QStringLiteral("decode.duration"), duration_ms > 0, QStringLiteral("%1 ms").arg(duration_ms));

    // The timeline's audio rows come from the decoder's stream list, not from a
    // fixture, so this is where multitrack is either real or absent.
    const bool tracks_known = waitFor("the audio track list", kClipOpenTimeoutMs, 50,
                                      [timeline]() { return !timeline->audioTrackLabels().isEmpty(); });
    QJsonArray track_labels;
    for (const QString& label : timeline->audioTrackLabels())
        track_labels.append(label);
    report.insert(QStringLiteral("audioTracks"), track_labels);
    log.record(QStringLiteral("decode.audioTracks"), tracks_known,
               timeline->audioTrackLabels().join(QStringLiteral(", ")));

    const bool tiles_started = waitFor("the first timeline thumbnail", kClipOpenTimeoutMs, 50,
                                       [timeline]() { return timeline->tilesReady() > 0; });
    report.insert(QStringLiteral("timelineTilesReady"), timeline->tilesReady());
    report.insert(QStringLiteral("timelineTilesExpected"), timeline->tilesExpected());
    log.record(QStringLiteral("timeline.thumbnails"), tiles_started,
               QStringLiteral("%1/%2 tiles decoded").arg(timeline->tilesReady()).arg(timeline->tilesExpected()));

    // ---- playback ------------------------------------------------------
    const qint64 position_before_play = session->positionMs();
    player->setPlaying(true);
    const bool advanced = waitFor("playback to advance", kPlaybackObserveMs, 50, [session, position_before_play]() {
        return session->positionMs() - position_before_play >= kMinPlaybackAdvanceMs;
    });
    const qint64 position_after_play = session->positionMs();
    player->setPlaying(false);
    log.record(QStringLiteral("playback.play"), advanced,
               QStringLiteral("position %1 ms -> %2 ms").arg(position_before_play).arg(position_after_play));

    // Pause has to actually stop the clock, not merely flip a label. The worker
    // is given a moment to drain whatever it had already decoded before the
    // position is sampled, so a frame in flight is not read as a failed pause.
    pumpFor(300);
    const qint64 position_at_pause = session->positionMs();
    pumpFor(500);
    const bool stayed_put = std::llabs(session->positionMs() - position_at_pause) < kMinPlaybackAdvanceMs;
    log.record(QStringLiteral("playback.pause"), stayed_put && !player->playing(),
               QStringLiteral("position held at %1 ms").arg(session->positionMs()));

    // ---- seek ----------------------------------------------------------
    // Three targets across the clip, each verified against the position the
    // session reports back, so a seek that is accepted and then ignored fails.
    QJsonArray seeks;
    bool seeks_ok = duration_ms > 0;
    for (const double fraction : {0.8, 0.1, 0.5}) {
        const qint64 target = static_cast<qint64>(std::llround(static_cast<double>(duration_ms) * fraction));
        session->requestSeek(target);
        const bool landed = waitFor("the seek to land", 5000, 25, [session, target]() {
            // 250 ms: a seek lands on the nearest decodable frame, not on an
            // arbitrary millisecond.
            return std::llabs(session->positionMs() - target) <= 250;
        });
        QJsonObject entry;
        entry.insert(QStringLiteral("targetMs"), target);
        entry.insert(QStringLiteral("reachedMs"), session->positionMs());
        entry.insert(QStringLiteral("landed"), landed);
        seeks.append(entry);
        if (!landed)
            seeks_ok = false;
    }
    report.insert(QStringLiteral("seeks"), seeks);
    log.record(QStringLiteral("playback.seek"), seeks_ok, QStringLiteral("3 targets across the clip"));

    // ---- trim ----------------------------------------------------------
    const qint64 trim_start =
        static_cast<qint64>(std::llround(static_cast<double>(duration_ms) * options.trim_start_fraction));
    const qint64 trim_end =
        static_cast<qint64>(std::llround(static_cast<double>(duration_ms) * options.trim_end_fraction));
    session->requestTrim(trim_start, trim_end);
    const bool trimmed = session->trimmed() && session->trimEndMs() > session->trimStartMs();
    report.insert(QStringLiteral("trimStartMs"), session->trimStartMs());
    report.insert(QStringLiteral("trimEndMs"), session->trimEndMs());
    log.record(QStringLiteral("trim.apply"), trimmed,
               QStringLiteral("requested %1-%2 ms, snapped to %3-%4 ms")
                   .arg(trim_start)
                   .arg(trim_end)
                   .arg(session->trimStartMs())
                   .arg(session->trimEndMs()));

    // ---- layout evidence -----------------------------------------------
    // Captured here, with the clip loaded and the trim applied, and before the
    // export changes the panel state.
    if (!options.screenshot_path.isEmpty()) {
        if (window == nullptr) {
            log.record(QStringLiteral("layout.screenshot"), false, QStringLiteral("no root window"));
        } else {
            if (options.screenshot_width > 0 && options.screenshot_height > 0)
                window->resize(options.screenshot_width, options.screenshot_height);
            // Two pumps: one for the resize to reach the scene graph, one for the
            // timeline to relayout its tiles against the new track width.
            pumpFor(400);
            pumpFor(400);
            const bool saved = window->grabWindow().save(options.screenshot_path);
            log.record(
                QStringLiteral("layout.screenshot"), saved,
                QStringLiteral("%1x%2 -> %3").arg(window->width()).arg(window->height()).arg(options.screenshot_path));
        }
    }

    // ---- export --------------------------------------------------------
    if (options.export_enabled) {
        // "copy" rather than overwrite: an E2E that overwrites its own source
        // cannot be re-run against the same input, and destroys the evidence.
        exporter->setSaveModeKey(QStringLiteral("copy"));
        if (!exporter->canExport()) {
            log.record(QStringLiteral("export.start"), false,
                       QStringLiteral("the export panel refused: canExport is false"));
        } else {
            exporter->startExport();
            const bool finished = waitFor("the export to finish", kExportTimeoutMs, 100, [exporter]() {
                return exporter->state() == EditExportAdapter::Done || exporter->state() == EditExportAdapter::Failed;
            });
            const bool done = finished && exporter->state() == EditExportAdapter::Done;
            report.insert(QStringLiteral("exportOutputPath"), exporter->outputPath());
            report.insert(QStringLiteral("exportError"), exporter->errorText());
            log.record(QStringLiteral("export.run"), done,
                       done ? exporter->outputPath()
                            : (exporter->errorText().isEmpty() ? QStringLiteral("timed out") : exporter->errorText()));

            if (done) {
                const QFileInfo out(exporter->outputPath());
                const bool on_disk = out.isFile() && out.size() > 0;
                report.insert(QStringLiteral("exportOutputBytes"), static_cast<qint64>(out.size()));
                log.record(QStringLiteral("export.output"), on_disk, QStringLiteral("%1 bytes").arg(out.size()));
            }
        }
    }

    report.insert(QStringLiteral("stages"), log.stages);
    report.insert(QStringLiteral("passed"), log.ok);
    writeReport(options.report_path, report);
    return log.ok ? 0 : 1;
}

} // namespace exosnap::quick
