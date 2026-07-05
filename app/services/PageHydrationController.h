#pragma once

#include <QObject>
#include <QString>

#include <cstddef>
#include <functional>
#include <vector>

namespace exosnap {

// Orchestrates MainWindow's staged, post-first-paint hydration of its
// secondary pages (Settings/Config, Device, Hotkeys, Diagnostics, Logs, About,
// EditExportOverlay, Webcam, Output): one build callback per event-loop turn
// via QTimer::singleShot(0, ...), so a heavy page constructor never blocks the
// UI thread for more than a single page at a time.
//
// MainWindow still owns page construction (buildConfigPage(), buildDevicePage(),
// etc. stay MainWindow members, each with its own already-built guard for early
// navigation); this controller only owns the staging policy around those
// callbacks and the "perf" AppLog milestone bracketing.
//
// Milestone naming mirrors the historical inline hydrateSecondaryPages() chain:
//   * the first step logs "hydrate-<name>-start" / "hydrate-<name>-end" and
//     runs synchronously, before start() returns (it is expected to already be
//     running inside a deferred context, e.g. a singleShot(0) from the first
//     paintEvent — the controller does not add an extra turn for it).
//   * every later step logs "hydrate-page-start:<name>" / "hydrate-page-end:<name>"
//     and runs one singleShot(0) turn after the previous step.
//   * once the last step's end milestone is logged, "hydrate-all-end" is
//     logged once, in the same turn.
class PageHydrationController : public QObject {
    Q_OBJECT

  public:
    struct Step {
        // Page identifier used in the milestone lines, e.g. "config", "device".
        QString name;
        // The page-building callback (e.g. a lambda calling MainWindow::buildDevicePage()).
        std::function<void()> build;
    };

    explicit PageHydrationController(std::vector<Step> steps, QObject* parent = nullptr);

    // Runs the first step synchronously, then schedules the remaining steps
    // one per event-loop turn. Logs "hydrate-all-end" once the last step's end
    // milestone has been logged. Idempotent: a second call is a no-op (mirrors
    // the fact that hydration is a one-shot startup sequence).
    void start();

  private:
    void runStep(std::size_t index);
    static void logMilestone(const QString& name);

    std::vector<Step> steps_;
    bool started_ = false;
};

} // namespace exosnap
