// exosnap-updater -- standalone swap-updater process entry point.
//
// Parses the command line into UpdaterArgs, shows the updater window and runs
// the real pipeline: an UpdaterWorker on a QThread drives the pure
// UpdaterController through queued signals; the window is re-rendered from the
// controller state after every event. Retry routing per the failure matrix
// re-enters the worker at RetryEntryStep(case).
//
// A dev-only `--preview-state <progress|amber|red|green|reboot>` short-circuits
// all engine work and renders a canned UpdaterUiState so the canon looks can be
// inspected (and screenshotted) without a real download/install in flight.

#include <QApplication>
#include <QScreen>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <update/install_mode_detector.h>
#include <update/update_types.h>

#include "UpdaterArgs.h"
#include "UpdaterController.h"
#include "UpdaterWindow.h"
#include "UpdaterWorker.h"

namespace {

constexpr char kPreviewFrom[] = "0.8.1";
constexpr char kPreviewTo[] = "0.9.0";

// How long the Success footer ("this window closes automatically") lingers.
constexpr int kSuccessAutoCloseMs = 1500;

// Build one of the canned preview states from the real controller so the
// preview stays faithful to the shipping state machine.
std::optional<UpdaterUiState> MakePreviewState(const QString& which) {
    UpdaterController c(QString::fromLatin1(kPreviewFrom), QString::fromLatin1(kPreviewTo));

    if (which == QStringLiteral("progress")) {
        c.onStepDone(UpStep::Download);
        c.onStepDone(UpStep::CloseApp);
        c.onStepStarted(UpStep::Install);
        UpdaterUiState s = c.state();
        s.ring = 0.72; // mid-install for a readable arc/percent
        return s;
    }
    if (which == QStringLiteral("amber")) {
        c.onStepDone(UpStep::Download);
        c.onStepDone(UpStep::CloseApp);
        c.onStepStarted(UpStep::Install);
        c.onFailure(FailureCase::InstallFailed, QString());
        return c.state();
    }
    if (which == QStringLiteral("red")) {
        c.onStepDone(UpStep::Download);
        c.onStepDone(UpStep::CloseApp);
        c.onStepDone(UpStep::Install);
        c.onStepStarted(UpStep::Verify);
        c.onFailure(FailureCase::VerifyInstallFailed, QString());
        return c.state();
    }
    if (which == QStringLiteral("reboot")) {
        c.onStepDone(UpStep::Download);
        c.onStepDone(UpStep::CloseApp);
        c.onFailure(FailureCase::MsiRebootRequired, QString());
        return c.state();
    }
    if (which == QStringLiteral("green")) {
        c.onStepDone(UpStep::Download);
        c.onStepDone(UpStep::CloseApp);
        c.onStepDone(UpStep::Install);
        c.onStepDone(UpStep::Verify);
        c.onStepStarted(UpStep::Launch);
        c.onFailure(FailureCase::LaunchFailed, QString());
        // Green is the real controller output after a failed auto-relaunch: the
        // Launch step stays Failed and the ring stays wherever onFailure left it.
        // The widget layer (not the state) is responsible for presenting that
        // failed Launch row as a "manual start" affordance rather than an error.
        return c.state();
    }
    return std::nullopt;
}

void CenterOnScreen(QWidget& w) {
    if (QScreen* screen = QApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        w.move(avail.center() - QPoint(w.width() / 2, w.height() / 2));
    }
}

// Where exosnap.exe lives for the window's "Open ..." actions. Portable: the
// --install-dir tree (on B2/B3 the previous version is back there; on B4 the
// new one is live -- either way it is the runnable install). MSI: the
// registry-stamped InstallPath, falling back to --install-dir.
std::wstring ResolveOpenDir(const UpdaterArgs& args) {
    if (args.install_mode == exosnap::update::InstallMode::Installed) {
        if (const auto path = exosnap::update::ReadInstallPath(); path.has_value() && !path->empty()) {
            return *path;
        }
    }
    return args.install_dir.toStdWString();
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const QStringList arguments = QCoreApplication::arguments();

    // Packaging-smoke short-circuit: auto-quit ~2 s after the window paints so a
    // release-pipeline launch (staged temp-dir copy per UpdaterStagingFileList)
    // proves the exe + its Qt runtime load and render without hanging the gate.
    // Only meaningful alongside --preview-state (no engine work); see
    // scripts/build-release-artifacts.ps1.
    const bool previewSmoke = arguments.contains(QStringLiteral("--preview-smoke"));
    constexpr int kPreviewSmokeCloseMs = 2000;

    // Dev preview short-circuit: render a canned state and skip engine work.
    const int previewIdx = arguments.indexOf(QStringLiteral("--preview-state"));
    if (previewIdx >= 0) {
        const QString which = previewIdx + 1 < arguments.size() ? arguments.at(previewIdx + 1) : QString();
        const std::optional<UpdaterUiState> state = MakePreviewState(which);
        if (!state.has_value()) {
            std::fprintf(stderr,
                         "exosnap-updater: invalid --preview-state '%s' "
                         "(expected progress|amber|red|green|reboot)\n",
                         qPrintable(which));
            return 2;
        }
        UpdaterWindow window;
        window.render(*state);
        window.show();
        CenterOnScreen(window);
        if (previewSmoke) {
            // QCoreApplication::quit() was found NOT to reliably return control from
            // app.exec() in this exact launch shape (confirmed with instrumentation:
            // the timer fires and quit() runs, but exec() never returns) on both this
            // dev machine and the GitHub Actions windows-2022 release-pipeline runner
            // -- root cause not pinned down (no child event loop or extra thread exists
            // on this short-circuit path to explain a stuck quit propagation). This
            // smoke's only job is proving the packaged exe loads its Qt runtime and
            // renders; std::exit() sidesteps whatever is holding exec() open instead of
            // depending on normal Qt shutdown, which is fine for this dev/CI-only path
            // that never runs for a real update.
            QTimer::singleShot(kPreviewSmokeCloseMs, &app, [] { std::exit(0); });
        }
        return app.exec();
    }

    const std::optional<UpdaterArgs> args = ParseUpdaterArgs(arguments);
    if (!args.has_value()) {
        // ParseUpdaterArgs already wrote a diagnostic line to stderr.
        return 2;
    }

    if (args->verify_reinstall) {
        // ADR 0055: a same-version reinstall over the full production path. Log it
        // so an updater run in this mode is never mistaken for a real upgrade.
        std::fprintf(stderr, "exosnap-updater: verification reinstall mode — only version \"%s\" is accepted\n",
                     qPrintable(args->current_version));
    }

    // The controller starts with a placeholder to-version (the release is not
    // resolved yet) and is rebuilt when the worker reports the real one.
    // MakeController keeps the verification-reinstall marking across the rebuilds
    // (releaseResolved / Retry) instead of silently dropping it.
    const auto MakeController = [&args](const QString& to_version) {
        auto c = std::make_unique<UpdaterController>(args->current_version, to_version);
        c->setVerificationReinstall(args->verify_reinstall);
        return c;
    };
    auto controller = MakeController(args->current_version);
    QString to_version = args->current_version;
    FailureCase last_failure = FailureCase::DownloadFailed;
    // Reentrancy guard: true while a pipeline run is queued or running. A
    // double-fired Retry (or a click landing before a terminal state) must not
    // queue a second run onto the worker. Cleared only on a terminal state.
    bool in_flight = false;

    UpdaterWindow window;

    const auto render = [&] {
        UpdaterUiState s = controller->state();
        window.render(s);
    };

    // ── Worker on its own thread; all signals cross into the GUI thread as
    //    queued connections (the window is the context object). ──────────────
    QThread worker_thread;
    worker_thread.setObjectName(QStringLiteral("exosnap-updater-worker"));
    UpdaterWorker worker(*args);
    worker.moveToThread(&worker_thread);
    worker_thread.start();

    QObject::connect(&worker, &UpdaterWorker::stepStarted, &window, [&](UpStep step) {
        controller->onStepStarted(step);
        render();
    });
    QObject::connect(&worker, &UpdaterWorker::downloadProgress, &window, [&](quint64 received, quint64 total) {
        controller->onDownloadProgress(received, total);
        render();
    });
    QObject::connect(&worker, &UpdaterWorker::stepDone, &window, [&](UpStep step) {
        controller->onStepDone(step);
        render();
    });
    QObject::connect(&worker, &UpdaterWorker::releaseResolved, &window, [&](const QString& version) {
        // Rebuild the controller with the real to-version and replay the only
        // event that can have happened by now (Download is in flight).
        to_version = version;
        controller = MakeController(to_version);
        controller->onStepStarted(UpStep::Download);
        render();
    });
    QObject::connect(&worker, &UpdaterWorker::allDone, &window, [&] {
        in_flight = false;
        controller->onAllDone();
        render();
        // Success footer: "this window closes automatically".
        QTimer::singleShot(kSuccessAutoCloseMs, &app, &QCoreApplication::quit);
    });
    QObject::connect(&worker, &UpdaterWorker::failed, &window, [&](FailureCase c, const QString& detail) {
        in_flight = false;
        last_failure = c;
        controller->onFailure(c, detail);
        // Technical diagnostics remain reproducible evidence without becoming
        // the primary UI copy (in particular raw WinHTTP and filesystem paths).
        if (!detail.isEmpty())
            std::fprintf(stderr, "exosnap-updater: failure %d: %s\n", static_cast<int>(c), qPrintable(detail));
        render();
    });

    // ── Footer actions ───────────────────────────────────────────────────────
    QObject::connect(&window, &UpdaterWindow::retryRequested, &window, [&] {
        if (in_flight) {
            return; // a run is already queued/running -- ignore the double-fire
        }
        in_flight = true;
        const UpStep entry = RetryEntryStep(last_failure);
        // Reset the UI to a clean in-progress state: steps before the re-entry
        // point are already done (their artifacts are kept by the worker).
        controller = MakeController(to_version);
        for (int i = 0; i < int(entry); ++i) {
            controller->onStepDone(static_cast<UpStep>(i));
        }
        render();
        QMetaObject::invokeMethod(&worker, [&worker, entry] { worker.run(entry); }, Qt::QueuedConnection);
    });
    QObject::connect(&window, &UpdaterWindow::closeRequested, &app, &QCoreApplication::quit);
    const auto openAndQuit = [&] {
        (void)LaunchExoSnapFrom(ResolveOpenDir(*args));
        QCoreApplication::quit();
    };
    QObject::connect(&window, &UpdaterWindow::openExoSnapRequested, &window, openAndQuit);

    // First paint mirrors the pipeline's first event so the window never shows
    // an all-queued limbo state.
    controller->onStepStarted(UpStep::Download);
    render();
    window.show();
    CenterOnScreen(window);

    in_flight = true;
    QMetaObject::invokeMethod(&worker, [&worker] { worker.run(UpStep::Download); }, Qt::QueuedConnection);

    const int exit_code = app.exec();

    // Shutdown: cancel any in-flight download and give the worker time to
    // unwind. The window disables its close X during Install/Verify/Launch, so a
    // still-busy worker here is in a cancellable or short-waiting phase. Allow a
    // generous 30 s so a 15 s WaitForInstanceMutex plus a RestoreBackup can never
    // be terminate()d mid-restore; terminate() stays only as the last resort.
    worker.requestCancel();
    worker_thread.quit();
    if (!worker_thread.wait(30000)) {
        worker_thread.terminate();
        worker_thread.wait(2000);
    }
    return exit_code;
}
