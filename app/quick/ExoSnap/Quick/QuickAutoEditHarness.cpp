#include "QuickAutoEditHarness.h"

#include "EditExportAdapter.h"
#include "EditPlayerAdapter.h"
#include "EditPlayerWorker.h"
#include "EditSessionAdapter.h"
#include "EditTimelineAdapter.h"
#include "ExoEditPlayerItem.h"
#include "QuickApplication.h"
#include "ShellAdapter.h"

#include "models/EditContextFactory.h"
#include <exosnap/engine/edit_timeline_export.h>

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QSaveFile>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <psapi.h>
#include <windows.h>

namespace exosnap::quick {

class EditPlayerAdapterTestPeer {
  public:
    static QObject* worker(EditPlayerAdapter& player) {
        return player.worker_;
    }
};

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

// Sleeps between polls. Sleep() rounds up to the system timer tick (15.6 ms by
// default), which would quantize every request-to-mailbox sample to that tick;
// a high-resolution waitable timer keeps the poll at the requested interval.
void pollSleep(int ms) {
    static const HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due{};
    due.QuadPart = -10'000LL * ms;
    if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) &&
        WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
        return;
    QThread::msleep(static_cast<unsigned long>(ms));
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
        pollSleep(std::max(1, poll_ms / 2));
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

QJsonObject processSample() {
    QJsonObject sample;
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory))) {
        sample.insert(QStringLiteral("workingSetBytes"), static_cast<qint64>(memory.WorkingSetSize));
        sample.insert(QStringLiteral("peakWorkingSetBytes"), static_cast<qint64>(memory.PeakWorkingSetSize));
        sample.insert(QStringLiteral("privateCommitBytes"), static_cast<qint64>(memory.PrivateUsage));
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        sample.insert(QStringLiteral("handles"), static_cast<qint64>(handles));
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto ticks = [](FILETIME time) { return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime; };
        sample.insert(QStringLiteral("cpuTime100ns"), static_cast<qint64>(ticks(kernel) + ticks(user)));
    }
    return sample;
}

QJsonObject cacheSample(EditTimelineAdapter* timeline) {
    const auto stats = timeline->thumbnailBroker()->Stats();
    const auto* waveforms = qobject_cast<WaveformCacheService*>(timeline->waveformService());
    QJsonObject sample{{QStringLiteral("thumbnailMemoryHits"), static_cast<qint64>(stats.memory_hits)},
                       {QStringLiteral("thumbnailDiskHits"), static_cast<qint64>(stats.disk_hits)},
                       {QStringLiteral("thumbnailDecodes"), static_cast<qint64>(stats.decodes)}};
    if (waveforms) {
        sample.insert(QStringLiteral("waveformDecoderOpens"), static_cast<qint64>(waveforms->decoderOpensForTest()));
        sample.insert(QStringLiteral("waveformDecodeCalls"), static_cast<qint64>(waveforms->decodeCallsForTest()));
        sample.insert(QStringLiteral("waveformDecoderSeeks"), static_cast<qint64>(waveforms->decoderSeeksForTest()));
        sample.insert(QStringLiteral("waveformDecodedFrames"), static_cast<qint64>(waveforms->decodedFramesForTest()));
        sample.insert(QStringLiteral("waveformAnalysisMemoryBytes"),
                      static_cast<qint64>(waveforms->analysisMemoryBytesForTest()));
    }
    return sample;
}

QJsonObject percentiles(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    QJsonObject result{{QStringLiteral("samples"), static_cast<int>(samples.size())},
                       {QStringLiteral("unit"), QStringLiteral("ms")},
                       {QStringLiteral("method"), QStringLiteral("nearest-rank")}};
    if (!samples.empty()) {
        for (const auto p : {50, 95, 99}) {
            const auto rank = static_cast<size_t>(std::ceil(p * samples.size() / 100.0));
            result.insert(QStringLiteral("p%1").arg(p), samples.at(rank - 1));
        }
    }
    return result;
}

// A detached productive surface observes the generation-filtered decoded mailbox.
// Consuming it cannot prove upload, composition, swap completion or physical display.
int runIsolatedQualification(QuickApplication& application, const AutoEditOptions& options) {
    StageLog log;
    QJsonObject report{{QStringLiteral("qpa"), QGuiApplication::platformName()},
                       {QStringLiteral("measurementBoundary"), QStringLiteral("decoder-to-item-mailbox-observed")},
                       {QStringLiteral("pollIntervalMs"), 1}};
    auto* session = application.editSessionAdapter();
    auto* player = application.editPlayerAdapter();
    auto* shell = application.shellAdapter();
    player->setVolume(0);
    QElapsedTimer total;
    total.start();
    QJsonArray cycles;
    QJsonArray seeks;
    QJsonArray edit_samples;
    QJsonArray seek_comparisons;
    QJsonArray first_seeks;
    std::vector<double> seek_latencies;
    std::vector<double> commit_latencies;
    const auto initial = processSample();
    ExoEditPlayerItem mailbox;
    qint64 observed_time = -1;
    const auto observe = [&]() {
        const auto pending = mailbox.takePendingFrame();
        const bool decoded =
            (pending.frame && pending.frame->width > 0 && pending.frame->height > 0) ||
            (pending.timeline && !pending.timeline->layers.empty() && pending.timeline->layers.front().frame.width > 0);
        if (decoded && pending.timeline_time_us) {
            observed_time = *pending.timeline_time_us;
            return true;
        }
        return false;
    };
    const auto seek = [&](qint64 target, int cycle) {
        observed_time = -1;
        (void)mailbox.takePendingFrame();
        QElapsedTimer clock;
        clock.start();
        session->requestSeekUs(target);
        const bool arrived = waitFor("a decoded mailbox frame", 10000, 1,
                                     [&]() { return observe() && std::llabs(observed_time - target) <= 100000; });
        const double elapsed = static_cast<double>(clock.nsecsElapsed()) / 1000000.0;
        seeks.append(QJsonObject{{QStringLiteral("cycle"), cycle},
                                 {QStringLiteral("targetUs"), target},
                                 {QStringLiteral("decodedTimelineUs"), observed_time},
                                 {QStringLiteral("observedMs"), elapsed},
                                 {QStringLiteral("passed"), arrived}});
        if (arrived)
            seek_latencies.push_back(elapsed);
        return arrived;
    };
    const auto performance_snapshot = [&]() {
        struct Snapshot {
            std::atomic<bool> ready = false;
            QVariantMap values;
        };
        const auto snapshot = std::make_shared<Snapshot>();
        auto* worker = static_cast<EditPlayerWorker*>(EditPlayerAdapterTestPeer::worker(*player));
        QElapsedTimer clock;
        clock.start();
        QMetaObject::invokeMethod(
            worker,
            [worker, snapshot]() {
                snapshot->values = worker->performanceSnapshot();
                snapshot->ready.store(true);
            },
            Qt::QueuedConnection);
        const bool ready =
            waitFor("the player performance snapshot", 30000, 1, [&]() { return snapshot->ready.load(); });
        return QJsonObject{
            {QStringLiteral("passed"), ready},
            {QStringLiteral("workerSlotDrainMs"), clock.elapsed()},
            {QStringLiteral("counters"), ready ? QJsonObject::fromVariantMap(snapshot->values) : QJsonObject{}}};
    };
    const auto counter_delta = [](const QJsonObject& before, const QJsonObject& after) {
        const auto before_counters = before.value(QStringLiteral("counters")).toObject();
        const auto after_counters = after.value(QStringLiteral("counters")).toObject();
        QJsonObject delta;
        for (auto it = after_counters.begin(); it != after_counters.end(); ++it) {
            if (it.key() == QStringLiteral("timelineRevision") || it.key() == QStringLiteral("timelinePlayheadUs"))
                continue;
            if (it.value().isDouble() && before_counters.value(it.key()).isDouble())
                delta.insert(it.key(), it.value().toDouble() - before_counters.value(it.key()).toDouble());
        }
        return delta;
    };
    const auto compare_seek = [&](const QString& scenario, qint64 target, const QJsonObject& before) {
        observed_time = -1;
        (void)mailbox.takePendingFrame();
        QElapsedTimer clock;
        clock.start();
        session->requestSeekUs(target);
        const bool arrived = waitFor("a controlled decoded mailbox frame", 30000, 1,
                                     [&]() { return observe() && std::llabs(observed_time - target) <= 100000; });
        const double elapsed = static_cast<double>(clock.nsecsElapsed()) / 1000000.0;
        const qint64 decoded_time = observed_time;
        const auto after = performance_snapshot();
        const bool passed = arrived && before.value(QStringLiteral("passed")).toBool() &&
                            after.value(QStringLiteral("passed")).toBool();
        const auto delta = counter_delta(before, after);
        seek_comparisons.append(QJsonObject{{QStringLiteral("scenario"), scenario},
                                            {QStringLiteral("targetUs"), target},
                                            {QStringLiteral("decodedTimelineUs"), decoded_time},
                                            {QStringLiteral("requestToMailboxObservedMs"), elapsed},
                                            {QStringLiteral("before"), before},
                                            {QStringLiteral("after"), after},
                                            {QStringLiteral("counterDelta"), delta},
                                            {QStringLiteral("passed"), passed}});
        log.record(QStringLiteral("performance.%1").arg(scenario), passed,
                   QStringLiteral("%1 ms to decoded mailbox; worker-slot barriers do not prove presentation")
                       .arg(elapsed, 0, 'f', 3));
        return after;
    };
    for (int cycle = 0; cycle < 5; ++cycle) {
        session->close();
        pumpFor(100);
        const auto before = processSample();
        // Taken while the closed worker is idle, so the first seek's delta covers the
        // import, the session open and that seek, and nothing from the previous cycle.
        const auto cycle_counters = performance_snapshot();
        QElapsedTimer open_clock;
        open_clock.start();
        session->importMedia(QUrl::fromLocalFile(options.media_path), true, 0);
        const bool imported = waitFor("the productive media import", kClipOpenTimeoutMs, 5,
                                      [session]() { return session->open() && !session->workspace().clips().empty(); });
        const qint64 import_ms = open_clock.elapsed();
        log.record(QStringLiteral("cycle.%1.import").arg(cycle), imported, session->workspaceError());
        if (!imported)
            break;
        emit shell->navigateToPageRequested(ShellAdapter::EditPage);
        const bool visible =
            waitFor("the resident editor", 10000, 5, [shell]() { return shell->editSurfaceVisible(); });
        log.record(QStringLiteral("cycle.%1.editPage").arg(cycle), visible, QString());
        // Attach after the resident QML surface, so it cannot replace this observer.
        mailbox.setPlayerAdapter(nullptr);
        mailbox.setPlayerAdapter(player);
        const qint64 duration = session->workspace().duration();
        if (options.export_enabled && duration < 6000000) {
            log.record(QStringLiteral("cleanup.input"), false,
                       QStringLiteral("The cleanup export workflow requires at least six seconds of media."));
            break;
        }
        bool seek_ok = duration > 0;
        for (int i = 0; i < 12; ++i) {
            const qint64 target = duration * (1 + (i * 7) % 19) / 20;
            seek_ok = seek(target, cycle) && seek_ok;
            if (i == 0) {
                const auto after_first = performance_snapshot();
                first_seeks.append(
                    QJsonObject{{QStringLiteral("cycle"), cycle},
                                {QStringLiteral("requestToMailboxObservedMs"),
                                 seeks.last().toObject().value(QStringLiteral("observedMs"))},
                                {QStringLiteral("importToFirstSeekMs"), open_clock.elapsed()},
                                {QStringLiteral("counterDeltaSinceImport"), counter_delta(cycle_counters, after_first)},
                                {QStringLiteral("passed"), cycle_counters.value(QStringLiteral("passed")).toBool() &&
                                                               after_first.value(QStringLiteral("passed")).toBool()}});
            }
        }
        log.record(QStringLiteral("cycle.%1.seek").arg(cycle), seek_ok,
                   QStringLiteral("12 correlated decoded targets"));
        session->requestSeekUs(0);
        pumpFor(50);
        const qint64 before_play = session->positionUs();
        int decoded_frames = 0;
        bool monitoring_requested = false;
        player->setPlaying(true);
        const bool advanced = waitFor("muted playback and decoded frames", cycle == 0 ? 45000 : 20000, 1, [&]() {
            if (player->monitoringConfirmationRequired() && !monitoring_requested &&
                !player->monitoringTracks().empty()) {
                const auto id = player->monitoringTracks().front().toMap().value(QStringLiteral("id")).toString();
                player->confirmMonitoring({id}, true);
                monitoring_requested = true;
            }
            if (monitoring_requested && !player->monitoringConfirmationRequired() && !player->playing())
                player->setPlaying(true);
            if (observe())
                ++decoded_frames;
            return session->positionUs() - before_play >=
                       std::min<qint64>(duration / 2, cycle == 0 ? 30000000 : 8000000) &&
                   decoded_frames >= 3;
        });
        log.record(QStringLiteral("cycle.%1.playback").arg(cycle), advanced && player->volume() == 0,
                   QStringLiteral("%1 decoded mailbox observations; volume zero; open=%2; confirmation=%3; %4")
                       .arg(decoded_frames)
                       .arg(player->clipOpen())
                       .arg(player->monitoringConfirmationRequired())
                       .arg(player->placeholderText()));
        const auto clips_before = session->workspace().clips();
        const auto revision = session->workspace().revision();
        emit shell->navigateToPageRequested(ShellAdapter::RecordPage);
        const bool hidden = waitFor("Record navigation", 5000, 5, [shell, player]() {
            return shell->currentPage() == ShellAdapter::RecordPage && !player->surfaceVisible() && !player->playing();
        });
        emit shell->navigateToPageRequested(ShellAdapter::EditPage);
        const bool restored = waitFor("Edit navigation", 5000, 5, [shell, player]() {
            return shell->editSurfaceVisible() && player->surfaceVisible() && !player->playing();
        });
        log.record(QStringLiteral("cycle.%1.navigation").arg(cycle),
                   hidden && restored && session->workspace().revision() == revision &&
                       session->workspace().clips() == clips_before,
                   QStringLiteral("Record/Edit preserves edits and leaves playback paused"));
        if (cycle == 4) {
            const qint64 comparison_target = duration / 20;
            auto before_splits = compare_seek(QStringLiteral("noCuts"), comparison_target, performance_snapshot());
            const auto initial_count = session->workspace().clips().size();
            bool split_ok = true;
            for (int cut = 1; cut <= 32; ++cut) {
                const qint64 at = duration * cut / 33;
                const auto clips = session->workspace().clips();
                const auto containing = std::find_if(
                    clips.begin(), clips.end(), [at](const auto& clip) { return clip.start < at && clip.end() > at; });
                if (containing == clips.end()) {
                    split_ok = false;
                    break;
                }
                session->selectClip(containing->id);
                session->requestSeekUs(at);
                const auto old_count = session->workspace().clips().size();
                session->splitSelected();
                split_ok = session->workspace().clips().size() > old_count && split_ok;
            }
            log.record(QStringLiteral("manyClips.split"), split_ok,
                       QStringLiteral("%1 -> %2 clips").arg(initial_count).arg(session->workspace().clips().size()));
            auto after_immediate =
                compare_seek(QStringLiteral("immediatelyAfter32Cuts"), comparison_target, before_splits);
            compare_seek(QStringLiteral("afterWorkerSlotDrain"), comparison_target, after_immediate);
            const qint64 boundary = duration / 33;
            const qint64 offset = std::min<qint64>(duration / 132, 200000);
            const qint64 left_target = boundary - offset;
            const qint64 right_target = boundary + offset;
            const auto& split_clips = session->workspace().clips();
            const auto left = std::find_if(split_clips.begin(), split_clips.end(), [left_target](const auto& clip) {
                return clip.start <= left_target && clip.end() > left_target;
            });
            const auto right = std::find_if(split_clips.begin(), split_clips.end(), [right_target](const auto& clip) {
                return clip.start <= right_target && clip.end() > right_target;
            });
            const bool same_source = left != split_clips.end() && right != split_clips.end() && left->id != right->id &&
                                     left->asset == right->asset;
            log.record(QStringLiteral("performance.sameSourceSplitPair"), same_source,
                       QStringLiteral("distinct adjacent segments of one asset"));
            if (same_source && offset > 100000) {
                const auto before_crossing =
                    compare_seek(QStringLiteral("sameSourceLeftSegment"), left_target, performance_snapshot());
                compare_seek(QStringLiteral("sameSourceCrossSegment"), right_target, before_crossing);
            } else if (same_source) {
                seek_comparisons.append(
                    QJsonObject{{QStringLiteral("scenario"), QStringLiteral("sameSourceCrossSegment")},
                                {QStringLiteral("status"), QStringLiteral("SKIP")},
                                {QStringLiteral("reason"),
                                 QStringLiteral("Adjacent targets are within the mailbox matching tolerance.")}});
            }
            for (int i = 0; i < 12; ++i)
                seek_ok = seek(duration * (1 + (i * 11) % 19) / 20, cycle) && seek_ok;
            log.record(QStringLiteral("manyClips.seek"), seek_ok, QStringLiteral("12 targets after 32 linked splits"));
        }
        const auto& clips_for_edit = session->workspace().clips();
        const auto last_clip = std::max_element(clips_for_edit.begin(), clips_for_edit.end(),
                                                [](const auto& a, const auto& b) { return a.start < b.start; });
        session->selectClip(last_clip->id);
        bool edits_ok = true;
        for (int i = 0; i < 16; ++i) {
            const auto checkpoint = session->workspace().clips();
            const auto old_revision = session->workspace().revision();
            QElapsedTimer clock;
            clock.start();
            const auto copy_before = player->workspaceCopyTimeNs();
            session->nudgeSelected(1);
            const double elapsed = static_cast<double>(clock.nsecsElapsed()) / 1000000.0;
            const auto command_performance = session->commandPerformance();
            const auto copy_ns = player->workspaceCopyTimeNs() - copy_before;
            const bool committed = session->workspace().revision() != old_revision;
            if (committed)
                commit_latencies.push_back(elapsed);
            session->undo();
            edits_ok = committed && session->workspace().clips() == checkpoint && edits_ok;
            edit_samples.append(
                QJsonObject{{QStringLiteral("cycle"), cycle},
                            {QStringLiteral("observedMs"), elapsed},
                            {QStringLiteral("modelNs"), command_performance.value("modelNs").toDouble()},
                            {QStringLiteral("publicationNs"), command_performance.value("publishNs").toDouble()},
                            {QStringLiteral("workspaceCopyNs"), static_cast<double>(copy_ns)},
                            {QStringLiteral("passed"), committed}});
        }
        log.record(QStringLiteral("cycle.%1.editHistory").arg(cycle), edits_ok,
                   QStringLiteral("16 nudge/undo round trips"));
        if (cycle == 0 && options.export_enabled && duration >= 6000000) {
            const auto original = session->workspace().snapshot();
            const auto selected = session->workspace().clips().front().id;
            session->trimClip(selected, 1000, 5000);
            bool cleaned = session->workspaceError().isEmpty();
            session->moveClip(selected, 0, false);
            cleaned = cleaned && session->workspaceError().isEmpty();
            session->selectClip(selected);
            session->requestSeekUs(2000000);
            session->splitSelected();
            const auto split = session->workspace().clips();
            const auto suffix =
                std::find_if(split.begin(), split.end(), [](const auto& clip) { return clip.start == 2000000; });
            cleaned = cleaned && session->workspaceError().isEmpty() && suffix != split.end();
            if (cleaned) {
                session->selectClip(suffix->id);
                session->deleteSelected(false);
                const auto removed = session->workspace().clips();
                cleaned = cleaned && removed.size() == original->clips.size() &&
                          std::all_of(removed.begin(), removed.end(),
                                      [asset = original->clips.front().asset](const auto& clip) {
                                          return clip.asset == asset && clip.start == 0 && clip.source_in == 1000000 &&
                                                 clip.source_out == 3000000;
                                      });
                session->undo();
                cleaned = cleaned && session->workspace().clips() == split;
                session->redo();
                cleaned =
                    cleaned && session->workspace().clips() == removed && session->workspace().duration() == 2000000;
            }
            log.record(QStringLiteral("cleanup.trimSplitLiftUndoRedo"),
                       cleaned && original->clips != session->workspace().clips(),
                       QStringLiteral("source [1s,3s) remains after trim, split and removal"));
            auto* exporter = application.editExportAdapter();
            exporter->reset();
            exporter->setSaveModeKey(QStringLiteral("new"));
            exporter->setModeKey(QStringLiteral("finishedStereo"));
            exporter->setSpanKey(QStringLiteral("entire"));
            exporter->setContainerKey(QStringLiteral("mkv"));
            const auto directory = QFileInfo(options.report_path).absolutePath();
            exporter->setOutputDirectoryProvider([directory]() { return directory; });
            exporter->startExport();
            const bool prepared = waitFor("cleanup export review", kExportTimeoutMs, 5, [exporter]() {
                return exporter->state() == EditExportAdapter::Review || exporter->state() == EditExportAdapter::Failed;
            });
            if (prepared && exporter->state() == EditExportAdapter::Review) {
                report.insert(QStringLiteral("cleanupReview"), QJsonObject::fromVariantMap(exporter->reviewOutcome()));
                exporter->confirmPreparedExport();
            }
            const bool finished = waitFor("cleanup export", kExportTimeoutMs, 5, [exporter]() {
                return exporter->state() == EditExportAdapter::Done || exporter->state() == EditExportAdapter::Failed;
            });
            log.record(QStringLiteral("cleanup.export"), finished && exporter->state() == EditExportAdapter::Done,
                       exporter->errorText());
            report.insert(QStringLiteral("cleanupExportPath"), exporter->outputPath());
            report.insert(QStringLiteral("cleanupExpectedDurationUs"), 2000000);
        }
        session->close();
        mailbox.setPlayerAdapter(nullptr);
        const auto drained = std::make_shared<std::atomic<bool>>(false);
        QElapsedTimer close_clock;
        close_clock.start();
        QMetaObject::invokeMethod(
            EditPlayerAdapterTestPeer::worker(*player), [drained]() { drained->store(true); }, Qt::QueuedConnection);
        const bool worker_drained = waitFor("the player close barrier", 30000, 1, [&]() { return drained->load(); });
        const qint64 close_ms = close_clock.elapsed();
        log.record(QStringLiteral("cycle.%1.playerWorkerDrained").arg(cycle), worker_drained,
                   QStringLiteral("%1 ms through queued close and earlier work").arg(close_ms));
        pumpFor(50);
        const bool closed = !session->open() && !player->playing() && !player->clipOpen();
        log.record(QStringLiteral("cycle.%1.close").arg(cycle), closed, QString());
        cycles.append(QJsonObject{{QStringLiteral("cycle"), cycle},
                                  {QStringLiteral("elapsedMs"), open_clock.elapsed()},
                                  {QStringLiteral("importMs"), import_ms},
                                  {QStringLiteral("playerCloseDrainMs"), close_ms},
                                  {QStringLiteral("before"), before},
                                  {QStringLiteral("afterClose"), processSample()},
                                  {QStringLiteral("cacheCounters"), cacheSample(application.editTimelineAdapter())}});
    }
    const auto final_sample = processSample();
    report.insert(QStringLiteral("resourcesSource"),
                  QStringLiteral("GetProcessMemoryInfo/GetProcessTimes/GetProcessHandleCount"));
    report.insert(QStringLiteral("initial"), initial);
    report.insert(QStringLiteral("final"), final_sample);
    report.insert(QStringLiteral("elapsedMs"), total.elapsed());
    report.insert(QStringLiteral("cycles"), cycles);
    report.insert(QStringLiteral("seeks"), seeks);
    report.insert(QStringLiteral("commits"), edit_samples);
    report.insert(QStringLiteral("controlledSeekComparisons"), seek_comparisons);
    report.insert(QStringLiteral("firstSeekAfterImport"), first_seeks);
    report.insert(QStringLiteral("performanceCounterBoundary"),
                  QStringLiteral("Worker-thread cumulative counters; decoder read time includes demux, decode and "
                                 "conversion. Worker-slot drain does not drain an asynchronous decoder or prove "
                                 "Qt rendering or physical presentation."));
    report.insert(QStringLiteral("requestToDecodedMailboxObserved"), percentiles(seek_latencies));
    report.insert(QStringLiteral("synchronousAdapterCommit"), percentiles(commit_latencies));
    QJsonArray unavailable;
    for (const auto* name : {"QtTextureHandoff", "pointerInputToVisualCommit", "physicalPresentationLatency",
                             "physicalFrameDrops", "audioVideoDrift", "audioUnderruns", "gpuUtilization", "vramUsage"})
        unavailable.append(QJsonObject{{QStringLiteral("metric"), QString::fromLatin1(name)},
                                       {QStringLiteral("status"), QStringLiteral("SKIP")}});
    report.insert(QStringLiteral("unavailable"), unavailable);
    report.insert(QStringLiteral("stages"), log.stages);
    report.insert(QStringLiteral("passed"), log.ok);
    writeReport(options.report_path, report);
    return log.ok ? 0 : 1;
}

} // namespace

bool AutoEditRequested(const QStringList& args) {
    return args.contains(QStringLiteral("--auto-edit")) || args.contains(QStringLiteral("--auto-edit-qualify"));
}

bool ParseAutoEditOptions(const QStringList& args, AutoEditOptions* options, QString* error) {
    *options = AutoEditOptions{};
    options->isolated_qualification = args.contains(QStringLiteral("--auto-edit-qualify"));

    options->media_path = optionValue(args, QStringLiteral("--auto-edit-media"));
    options->additional_media_path = optionValue(args, QStringLiteral("--auto-edit-add-media"));
    options->report_path = optionValue(args, QStringLiteral("--auto-edit-report"));

    if (options->isolated_qualification && options->media_path.isEmpty()) {
        *error = QStringLiteral("--auto-edit-qualify requires --auto-edit-media");
        return false;
    }

    if (options->isolated_qualification && options->report_path.isEmpty()) {
        *error = QStringLiteral("--auto-edit-qualify requires --auto-edit-report");
        return false;
    }

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
    if (options.isolated_qualification)
        return runIsolatedQualification(application, options);

    auto* session = application.editSessionAdapter();
    auto* player = application.editPlayerAdapter();
    auto* timeline = application.editTimelineAdapter();
    auto* exporter = application.editExportAdapter();
    if (session == nullptr || player == nullptr || timeline == nullptr || exporter == nullptr) {
        qCritical().noquote() << QStringLiteral("auto-edit: the Quick composition owner has no edit adapters");
        return 2;
    }

    StageLog log;
    if (!options.report_path.isEmpty()) {
        const auto directory = QFileInfo(options.report_path).absolutePath();
        exporter->setOutputDirectoryProvider([directory]() { return directory; });
    }
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

    const qint64 first_duration_ms = session->durationMs();
    if (!options.additional_media_path.isEmpty()) {
        auto context = MakeMinimalEditContext(options.additional_media_path);
        const auto metadata =
            engine::ProbeEditMedia(std::filesystem::path(options.additional_media_path.toStdWString()));
        context.duration_seconds = static_cast<double>(metadata.duration_us) / 1000000.0;
        session->setEditContext(context);
        session->requestSeek(0);
        pumpFor(500);
    }
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

    if (!options.additional_media_path.isEmpty()) {
        session->requestSeek(std::max<qint64>(0, first_duration_ms - 500));
        pumpFor(250);
        player->setPlaying(true);
        const bool crossed = waitFor("hard cut", 5000, 25, [session, first_duration_ms]() {
            return session->positionMs() >= first_duration_ms + 500;
        });
        player->setPlaying(false);
        log.record(QStringLiteral("playback.hardCut"), crossed, QStringLiteral("crossed the source boundary"));
    }

    // ---- seek ----------------------------------------------------------
    // The scene-graph handoff must acknowledge the requested frame. Updating the
    // optimistic playhead alone does not establish decoded or rendered pixels.
    QJsonArray seeks;
    bool seeks_ok = duration_ms > 0;
    for (const double fraction : {0.8, 0.1, 0.5}) {
        const qint64 target = static_cast<qint64>(std::llround(static_cast<double>(duration_ms) * fraction));
        session->requestSeek(target);
        const bool landed = waitFor("the seek to land", 5000, 25, [player, target]() {
            const auto presented = player->presentedTimeUs();
            return player->previewState() == EditPlayerAdapter::Ready && !player->seekPending() &&
                   presented.isValid() && std::llabs(presented.toLongLong() - target * 1000) <= 250000;
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
    if (options.additional_media_path.isEmpty())
        session->requestTrim(trim_start, trim_end);
    else {
        session->requestSeek(first_duration_ms / 2);
        session->selectAdjacentClip(-1000000);
        session->splitSelected();
    }
    const bool trimmed = options.additional_media_path.isEmpty()
                             ? session->trimmed() && session->trimEndMs() > session->trimStartMs()
                             : session->workspace().clips().size() >= 6;
    report.insert(QStringLiteral("trimStartMs"), session->trimStartMs());
    report.insert(QStringLiteral("trimEndMs"), session->trimEndMs());
    log.record(options.additional_media_path.isEmpty() ? QStringLiteral("trim.apply") : QStringLiteral("split.apply"),
               trimmed,
               options.additional_media_path.isEmpty() ? QStringLiteral("requested %1-%2 ms, snapped to %3-%4 ms")
                                                             .arg(trim_start)
                                                             .arg(trim_end)
                                                             .arg(session->trimStartMs())
                                                             .arg(session->trimEndMs())
                                                       : QStringLiteral("split first source at %1 ms; linked clips: %2")
                                                             .arg(first_duration_ms / 2)
                                                             .arg(session->workspace().clips().size()));

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
        // A new output preserves the source for verification and repeated runs.
        exporter->setSaveModeKey(QStringLiteral("new"));
        if (!exporter->canExport()) {
            log.record(QStringLiteral("export.start"), false,
                       QStringLiteral("the export panel refused: canExport is false"));
        } else {
            exporter->startExport();
            const bool prepared = waitFor("export review", kExportTimeoutMs, 25, [exporter]() {
                return exporter->state() == EditExportAdapter::Review || exporter->state() == EditExportAdapter::Failed;
            });
            if (prepared && exporter->state() == EditExportAdapter::Review)
                exporter->confirmPreparedExport();
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
