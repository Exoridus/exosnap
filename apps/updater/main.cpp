// exosnap-updater -- standalone swap-updater process entry point.
//
// Two modes, decided by the command line and nothing else:
//
//   * AppHandoff -- ExoSnap launched this process with `--apply-handoff <path>`,
//     naming ONE versioned document that carries the whole operation: the pinned
//     release, the manifest bytes and detached signature that prove it, the
//     installation it applies to, the parent pid and the transaction id. The
//     pipeline runs start to finish without asking: the user already confirmed
//     in the app. The document is untrusted input -- it is re-validated here,
//     and the manifest signature is verified again in this process, because this
//     is the process that performs the destructive action.
//   * Manual -- someone started the executable themselves. It rests at Idle,
//     works out its own context, and does nothing until asked: check, then
//     download, then apply, each a separate confirmation. This is also the only
//     way back when a failed update left the app unable to start.
//
// An UpdaterWorker on a QThread drives the pure UpdaterController through queued
// signals; the window is re-rendered from the controller state after every
// event. Retry routing per the failure matrix re-enters the worker at
// RetryEntryStep(case).
//
// A dev-only `--preview-state <download|progress|amber|red|green|reboot>`
// short-circuits all engine work and renders a canned UpdaterUiState so the
// canon looks can be inspected (and screenshotted) without a real
// download/install in flight.
//
// The process exit code is the run's outcome, not "did the event loop end" --
// see UpdaterExitCode.h.

// clang-format off
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format on

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
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

#include <control/control_server.h>
#include <control/options.h>

#include <update/install_mode_detector.h>
#include <update/swap_engine.h>
#include <update/update_flow_state.h>
#include <update/update_types.h>
#include <update_handoff/handoff.h>

#include "UpdaterArgs.h"
#include "UpdaterAutomation.h"
#include "UpdaterCommandPolicy.h"
#include "UpdaterControlDispatcher.h"
#include "UpdaterController.h"
#include "UpdaterExitCode.h"
#include "UpdaterWindow.h"
#include "UpdaterWorker.h"
#include "WindowPlacement.h"

// main() itself stays at global scope; everything it drives lives in the
// updater namespace.
using namespace exosnap::updater;

namespace {

constexpr char kPreviewFrom[] = "0.9.0-rc4";
constexpr char kPreviewTo[] = "0.9.0-rc5";

// How long the Success footer ("this window closes automatically") lingers.
constexpr int kSuccessAutoCloseMs = 1500;

// Build one of the canned preview states from the real controller so the
// preview stays faithful to the shipping state machine. The accepted values are
// PreviewStateNames() -- one list, shared with the argument parser, because two
// copies of it had already drifted apart once.
std::optional<UpdaterUiState> MakePreviewState(const QString& which) {
    UpdaterController c(QString::fromLatin1(kPreviewFrom), QString::fromLatin1(kPreviewTo));

    if (which == QStringLiteral("download")) {
        c.onStepStarted(UpStep::Download);
        c.onDownloadProgress(38, 100);
        return c.state();
    }
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

// Positions the updater window centered on the ExoSnap window it was launched
// for (--app-pid), clamped inside that window's own monitor's available
// (taskbar-excluded) work area -- see WindowPlacement.h. Always centering on
// the primary screen (the old CenterOnScreen) put the updater on the wrong
// monitor whenever ExoSnap ran on a secondary one. Falls back to
// CenterOnScreen when app_pid is 0 (every manual start), the window can't be
// found (already closed, verify-reinstall's own app pid replaced by a stale
// one, ...), or no screen contains it.
void PlaceNearAppWindow(QWidget& w, quint32 app_pid) {
    if (app_pid != 0) {
        const auto hwnd = reinterpret_cast<HWND>(exosnap::update::FindTopLevelWindowForProcess(app_pid, L"ExoSnap"));
        if (hwnd != nullptr) {
            RECT r{};
            if (::GetWindowRect(hwnd, &r)) {
                const QPoint anchor_center((r.left + r.right) / 2, (r.top + r.bottom) / 2);
                QScreen* screen = QGuiApplication::screenAt(anchor_center);
                if (screen == nullptr)
                    screen = QApplication::primaryScreen();
                if (screen != nullptr) {
                    const QRect placed = PlaceWindowNearAnchor(w.size(), anchor_center, screen->availableGeometry());
                    w.move(placed.topLeft());
                    return;
                }
            }
        }
    }
    CenterOnScreen(w);
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

// The command line minus the automation option and its value. The updater's
// argument parser rejects anything it does not know, on purpose -- and the
// automation gate is not update context: it must not turn a manual start into a
// handoff, and it must not have to be threaded through UpdaterArgs to be
// tolerated.
QStringList WithoutControlOption(const QStringList& arguments) {
    const QString option = QString::fromLatin1(exosnap::updater_control::kControlOption);
    QStringList kept;
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        if (arguments.at(i) == option) {
            ++i; // and its value
            continue;
        }
        kept.append(arguments.at(i));
    }
    return kept;
}

// What system.hello answers. Enough for a runner to refuse a process it did not
// mean to talk to.
QJsonObject BuildIdentity(const UpdaterArgs& args) {
    const bool handoff = args.mode == exosnap::update::UpdaterMode::AppHandoff;

    QJsonObject identity;
    identity.insert(QStringLiteral("product"), QStringLiteral("exosnap-updater"));
    identity.insert(QStringLiteral("executable"), QCoreApplication::applicationFilePath());
    identity.insert(QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid()));
    identity.insert(QStringLiteral("mode"), QString::fromLatin1(exosnap::update::UpdaterModeName(args.mode)));
    identity.insert(QStringLiteral("installMode"), args.install_mode == exosnap::update::InstallMode::Installed
                                                       ? QStringLiteral("installed")
                                                       : QStringLiteral("portable"));
    identity.insert(QStringLiteral("installDir"), args.install_dir);
    // A handoff run resolves no channel: its release is pinned and its manifest
    // was handed over. Reporting the default "stable" there would name a feed
    // this process never reads, so the field is null instead.
    identity.insert(QStringLiteral("channel"), handoff
                                                   ? QJsonValue(QJsonValue::Null)
                                                   : QJsonValue(args.channel == exosnap::update::UpdateChannel::Preview
                                                                    ? QStringLiteral("preview")
                                                                    : QStringLiteral("stable")));
    identity.insert(QStringLiteral("currentVersion"), args.current_version);
    identity.insert(QStringLiteral("targetVersion"), args.target_version);
    identity.insert(QStringLiteral("verifyReinstall"), args.verify_reinstall);
    // The correlation identity and the schema this process read it under. A
    // runner asserts both against what the application reported, which is what
    // makes the process transition provable rather than assumed.
    identity.insert(QStringLiteral("updateTransactionId"), args.update_transaction_id.isEmpty()
                                                               ? QJsonValue(QJsonValue::Null)
                                                               : QJsonValue(args.update_transaction_id));
    identity.insert(QStringLiteral("handoffVersion"),
                    handoff ? QJsonValue(exosnap::update_handoff::kHandoffVersion) : QJsonValue(QJsonValue::Null));
    return identity;
}

// Load, then validate, the handed-over document. Returns an empty string when
// the operation may proceed; otherwise the evidence line for the refusal.
//
// Three independent things are checked, and NONE of them is a trust decision:
// the schema (is this a document this build understands), the assets (do the
// files it points at exist) and the installation context (is the named directory
// really an ExoSnap installation running the version the document claims). The
// trust chain itself is re-established later, in the worker, over the manifest
// bytes.
QString AcceptHandoff(const QString& path, UpdaterArgs* args, const UpdaterCommandLine& command_line) {
    namespace handoff_ns = exosnap::update_handoff;

    const handoff_ns::HandoffLoadResult loaded = handoff_ns::LoadUpdateHandoff(path);
    if (!loaded.ok()) {
        // The mode is set even on a refusal: this process WAS handed off to, and
        // reporting it as a manual run would misdescribe why it exists.
        args->mode = exosnap::update::UpdaterMode::AppHandoff;
        return QStringLiteral("%1 (%2): %3")
            .arg(QString::fromLatin1(handoff_ns::HandoffRejectionName(loaded.rejection)), path, loaded.detail);
    }

    *args = ArgsFromHandoff(*loaded.handoff, command_line);

    QString detail;
    if (!handoff_ns::HandoffAssetsPresent(*loaded.handoff, &detail))
        return detail;
    if (handoff_ns::ValidateInstallContextOnDisk(*loaded.handoff, &detail) != handoff_ns::InstallContextRejection::None)
        return detail;
    return {};
}

// Everything a manual start has to work out for itself, because no launcher told
// it: which install this is, where it lives, and what version is actually there.
void FillManualContext(UpdaterArgs& args) {
    const auto registry_path = exosnap::update::ReadInstallPath();
    const ManualContext context =
        ResolveManualContext(exosnap::update::DetectInstallMode(),
                             registry_path.has_value() ? QString::fromStdWString(*registry_path) : QString(),
                             QCoreApplication::applicationDirPath());
    args.install_mode = context.install_mode;
    args.install_dir = context.install_dir;
    args.current_version = ReadInstalledVersion(context.install_dir);
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
        const std::optional<UpdaterUiState> state = IsKnownPreviewState(which) ? MakePreviewState(which) : std::nullopt;
        if (!state.has_value()) {
            std::fprintf(stderr, "exosnap-updater: invalid --preview-state '%s' (expected %s)\n", qPrintable(which),
                         qPrintable(PreviewStateNames().join(QLatin1Char('|'))));
            return static_cast<int>(UpdaterExit::UsageError);
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
        (void)app.exec();
        // A preview render has no update outcome to report. It is a rendering
        // tool, and a rendering tool that succeeded exits 0.
        return static_cast<int>(UpdaterExit::Success);
    }

    // The automation gate: one explicit argv option carrying a run id. Without
    // it there is no pipe, no thread and no log line. A malformed option is
    // fatal rather than silently ignored -- a runner that believes it armed the
    // channel and got an ordinary process would report the wrong thing.
    const exosnap::control::ControlOptions control_options =
        exosnap::control::ParseControlOptions(arguments, QString::fromLatin1(exosnap::updater_control::kControlOption));
    if (control_options.requested && !control_options.error.isEmpty()) {
        std::fprintf(stderr, "exosnap-updater: %s\n", qPrintable(control_options.error));
        return static_cast<int>(UpdaterExit::UsageError);
    }

    const std::optional<UpdaterCommandLine> command_line = ParseUpdaterCommandLine(WithoutControlOption(arguments));
    if (!command_line.has_value()) {
        // ParseUpdaterCommandLine already wrote a diagnostic line to stderr.
        return static_cast<int>(UpdaterExit::UsageError);
    }

    // Empty means the handoff was accepted. A refusal is NOT a usage error: the
    // command line was fine, the document was not -- so it becomes a truthful
    // terminal failure with an observable state and a non-zero exit code,
    // reachable over the automation endpoint like any other outcome.
    UpdaterArgs args;
    QString handoff_rejection;
    if (!command_line->handoff_path.isEmpty()) {
        handoff_rejection = AcceptHandoff(command_line->handoff_path, &args, *command_line);
        if (!handoff_rejection.isEmpty())
            std::fprintf(stderr, "exosnap-updater: refusing the update handoff — %s\n", qPrintable(handoff_rejection));
    } else {
        args = ArgsForManualStart(*command_line);
        FillManualContext(args);
    }

    const bool manual = args.mode == exosnap::update::UpdaterMode::Manual;

    if (args.verify_reinstall) {
        // ADR 0055: a same-version reinstall over the full production path. Log it
        // so an updater run in this mode is never mistaken for a real upgrade.
        std::fprintf(stderr, "exosnap-updater: verification reinstall mode — only version \"%s\" is accepted\n",
                     qPrintable(args.current_version));
    }
    if (!args.target_version.isEmpty()) {
        std::fprintf(stderr, "exosnap-updater: pinned to version \"%s\" — any other release is refused\n",
                     qPrintable(args.target_version));
    }
    if (!args.update_transaction_id.isEmpty()) {
        std::fprintf(stderr, "exosnap-updater: update transaction %s\n", qPrintable(args.update_transaction_id));
    }

    // The controller opens on the version the handoff pinned, when there is one:
    // that is what the user was offered and what this run will install or refuse.
    // Without a pin (a manual start) there is no target yet, and the window
    // shows one pill rather than an empty second one.
    QString to_version = args.target_version;
    // MakeController keeps the mode and the verification-reinstall marking across
    // the rebuilds (releaseResolved / Retry) instead of silently dropping them.
    const bool checks_enabled = UpdateChecksEnabled(args);
    const auto MakeController = [&args, manual, checks_enabled](const QString& target) {
        auto c = std::make_unique<UpdaterController>(args.current_version, target);
        c->setVerificationReinstall(args.verify_reinstall);
        c->setMode(manual ? exosnap::update::UpdaterMode::Manual : exosnap::update::UpdaterMode::AppHandoff);
        c->setContext(args.install_mode, checks_enabled);
        c->setUpdateTransactionId(args.update_transaction_id);
        return c;
    };
    auto controller = MakeController(to_version);
    FailureCase last_failure = FailureCase::DownloadFailed;
    // Reentrancy guard: true while a worker call is queued or running. A
    // double-fired Retry (or a click landing before a terminal state) must not
    // queue a second run onto the worker. Cleared only on a terminal state.
    bool in_flight = false;

    UpdaterWindow window;

    // Armed only by --automation-control. Declared here so `render` can publish
    // through it; both stay null on every ordinary launch.
    std::unique_ptr<UpdaterAutomation> automation;
    std::unique_ptr<exosnap::updater_control::UpdaterControlDispatcher> control_dispatcher;
    std::unique_ptr<exosnap::control::ControlServer> control_server;

    const auto render = [&] {
        UpdaterUiState s = controller->state();
        window.render(s);
        if (!automation)
            return;
        // The window and the channel are rendered from the SAME controller
        // event, in the same call. There is no separate automation state to fall
        // behind, and the event below fires only when the revision actually
        // advanced -- so a client waiting on it cannot be woken by a progress
        // tick, and cannot miss a phase change either.
        const exosnap::update::UpdateFlowState flow = controller->flowState();
        if (automation->Publish(flow) && control_server) {
            control_server->EmitEvent(QString::fromLatin1(exosnap::updater_control::kStateChangedEvent),
                                      exosnap::updater_control::StateToJson(flow, automation->StateRevision()));
        }
    };

    // ── Worker on its own thread; all signals cross into the GUI thread as
    //    queued connections (the window is the context object). ──────────────
    QThread worker_thread;
    worker_thread.setObjectName(QStringLiteral("exosnap-updater-worker"));
    UpdaterWorker worker(args);
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
        // With a pinned target this only ever confirms what is already on the
        // pill (anything else is refused by the target gate a moment later).
        // Without one it is the first time this process knows the version, so
        // the controller is rebuilt around it and the only event that can have
        // happened by now (Download is in flight) is replayed.
        if (version == to_version)
            return;
        to_version = version;
        controller = MakeController(to_version);
        controller->onStepStarted(UpStep::Download);
        render();
    });
    QObject::connect(&worker, &UpdaterWorker::allDone, &window, [&] {
        in_flight = false;
        // The transaction is over and it succeeded: this process consumed the
        // document, so this process disposes of it. On every other outcome the
        // directory is left exactly where it is -- it IS the evidence for what
        // was handed over, and the application prunes it when it prepares the
        // next one.
        if (!args.handoff_path.isEmpty()) {
            const QString transaction_dir = QFileInfo(args.handoff_path).absolutePath();
            if (!QDir(transaction_dir).removeRecursively())
                std::fprintf(stderr, "exosnap-updater: could not remove the consumed update transaction at %s\n",
                             qPrintable(transaction_dir));
        }
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
            std::fprintf(stderr, "exosnap-updater: failure %s: %s\n", exosnap::update::FailureCaseName(c),
                         qPrintable(detail));
        render();
    });

    QObject::connect(&worker, &UpdaterWorker::cancelled, &window, [&] {
        in_flight = false;
        controller->onCancelled();
        render();
    });

    // ── Manual-mode results ──────────────────────────────────────────────────
    QObject::connect(&worker, &UpdaterWorker::checkStarted, &window, [&] {
        controller->onCheckStarted();
        render();
    });
    QObject::connect(&worker, &UpdaterWorker::upToDate, &window, [&] {
        in_flight = false;
        to_version.clear();
        controller->onUpToDate();
        render();
    });
    QObject::connect(&worker, &UpdaterWorker::updateAvailable, &window, [&](const QString& version) {
        in_flight = false;
        to_version = version;
        controller->onUpdateAvailable(version);
        render();
    });
    QObject::connect(&worker, &UpdaterWorker::readyToApply, &window, [&] {
        in_flight = false;
        controller->onReadyToApply();
        render();
    });
    QObject::connect(&worker, &UpdaterWorker::checkBlocked, &window, [&](const QString& reason) {
        in_flight = false;
        controller->onCheckBlocked(reason);
        render();
    });

    // ── Footer actions ───────────────────────────────────────────────────────
    const auto start = [&](void (UpdaterWorker::*slot)()) {
        if (in_flight)
            return false; // a call is already queued/running -- ignore the double-fire
        in_flight = true;
        QMetaObject::invokeMethod(&worker, [&worker, slot] { (worker.*slot)(); }, Qt::QueuedConnection);
        return true;
    };
    const auto doCheck = [&] {
        if (in_flight)
            return false;
        // A re-check starts from a clean slate: the previous answer's step marks
        // and target version describe a resolution that is being replaced.
        controller = MakeController(QString());
        to_version.clear();
        render();
        return start(&UpdaterWorker::check);
    };
    QObject::connect(&window, &UpdaterWindow::checkRequested, &window, [&] { (void)doCheck(); });
    QObject::connect(&window, &UpdaterWindow::downloadRequested, &window,
                     [&] { (void)start(&UpdaterWorker::download); });
    QObject::connect(&window, &UpdaterWindow::applyRequested, &window, [&] { (void)start(&UpdaterWorker::apply); });

    const auto doRetry = [&] {
        if (in_flight) {
            return false; // a run is already queued/running -- ignore the double-fire
        }
        const UpStep entry = RetryEntryStep(last_failure);
        // Reset the UI to a clean in-progress state: steps before the re-entry
        // point are already done (their artifacts are kept by the worker).
        controller = MakeController(to_version);
        for (int i = 0; i < int(entry); ++i) {
            controller->onStepDone(static_cast<UpStep>(i));
        }
        render();
        if (manual) {
            // A manual retry must re-enter the manual flow, never the
            // straight-through pipeline: re-resolve when no release was ever
            // found, re-fetch when one was, and otherwise resume at the failed
            // apply step. run(Download) here would download AND install without
            // the confirmation this mode exists to require.
            in_flight = true;
            if (entry <= UpStep::Download) {
                const bool resolved = !to_version.isEmpty();
                QMetaObject::invokeMethod(
                    &worker, [&worker, resolved] { resolved ? worker.download() : worker.check(); },
                    Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(&worker, [&worker, entry] { worker.run(entry); }, Qt::QueuedConnection);
            }
            return true;
        }
        in_flight = true;
        QMetaObject::invokeMethod(&worker, [&worker, entry] { worker.run(entry); }, Qt::QueuedConnection);
        return true;
    };
    QObject::connect(&window, &UpdaterWindow::retryRequested, &window, [&] { (void)doRetry(); });
    QObject::connect(&window, &UpdaterWindow::closeRequested, &app, &QCoreApplication::quit);
    const auto openAndQuit = [&] {
        (void)LaunchExoSnapFrom(ResolveOpenDir(args));
        QCoreApplication::quit();
    };
    QObject::connect(&window, &UpdaterWindow::openExoSnapRequested, &window, openAndQuit);

    // ── Automation endpoint ──────────────────────────────────────────────────
    // Only when explicitly armed. The intents below route to the SAME functions
    // the footer buttons drive -- a channel that reached past them would prove
    // something a user never executes, which for an updater is the whole point.
    if (control_options.requested) {
        UpdaterAutomation::Intents intents;
        intents.check = [&](QString* error) {
            if (!doCheck()) {
                *error = QStringLiteral("Another updater operation is already running");
                return false;
            }
            return true;
        };
        intents.download = [&](QString* error) {
            if (!start(&UpdaterWorker::download)) {
                *error = QStringLiteral("Another updater operation is already running");
                return false;
            }
            return true;
        };
        intents.apply = [&](QString* error) {
            if (!start(&UpdaterWorker::apply)) {
                *error = QStringLiteral("Another updater operation is already running");
                return false;
            }
            return true;
        };
        intents.retry = [&](QString* error) {
            if (!doRetry()) {
                *error = QStringLiteral("Another updater operation is already running");
                return false;
            }
            return true;
        };
        intents.cancel = [&](QString*) {
            // Cooperative, and only meaningful where the engine observes it --
            // the policy has already refused every phase where it would not.
            worker.requestCancel();
            return true;
        };
        intents.close = [&](QString*) {
            // Accepted, then the process ends. The client sees the response and
            // then the connection closing, and THAT is the completion: there is
            // no state left to observe once the endpoint is gone. The small
            // delay exists so the response reaches the pipe before the process
            // does exit; it is not a synchronisation guarantee, which is why the
            // documented contract is "ok, then the connection drops".
            QTimer::singleShot(250, &app, &QCoreApplication::quit);
            return true;
        };

        automation = std::make_unique<UpdaterAutomation>(BuildIdentity(args), std::move(intents));
        control_dispatcher = std::make_unique<exosnap::updater_control::UpdaterControlDispatcher>(
            automation.get(), control_options.run_id);
        control_server = std::make_unique<exosnap::control::ControlServer>(
            control_dispatcher.get(), QString::fromLatin1(exosnap::updater_control::kControlRole),
            control_options.run_id, QStringLiteral("updater-control"));

        QString control_error;
        if (!control_server->Start(&control_error)) {
            // Fatal for an automated run: a runner that cannot reach the process
            // it was told to drive must not be handed a normal updater instead.
            std::fprintf(stderr, "exosnap-updater: %s\n", qPrintable(control_error));
            worker_thread.quit();
            worker_thread.wait(5000);
            return static_cast<int>(UpdaterExit::UsageError);
        }
        // Seed the source with the state the window is about to be rendered from,
        // so a client that attaches before the first action sees a real snapshot
        // rather than a default-constructed one.
        (void)automation->Publish(controller->flowState());
    }

    if (!handoff_rejection.isEmpty()) {
        // A0. The pipeline is never entered: this process refused the document
        // before doing any work, which is exactly why its "nothing was touched"
        // claim needs no further evidence. The endpoint (armed above) publishes
        // the same terminal state a runner asserts on.
        last_failure = FailureCase::HandoffRejected;
        controller->onFailure(FailureCase::HandoffRejected, handoff_rejection);
        render();
        window.show();
        CenterOnScreen(window);
    } else if (manual) {
        // The resting entry point. Nothing is fetched, nothing is contacted and
        // nothing is replaced until the primary action is pressed.
        controller->onIdle();
        render();
        window.show();
        CenterOnScreen(window);
    } else {
        // First paint mirrors the pipeline's first event so the window never
        // shows an all-queued limbo state.
        controller->onStepStarted(UpStep::Download);
        render();
        window.show();
        PlaceNearAppWindow(window, args.app_pid);

        in_flight = true;
        QMetaObject::invokeMethod(&worker, [&worker] { worker.run(UpStep::Download); }, Qt::QueuedConnection);
    }

    (void)app.exec();

    // The endpoint goes first: its worker thread hands requests to this thread,
    // and this thread is about to stop answering.
    if (control_server)
        control_server->Stop();

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

    // The outcome, not "the event loop ended". app.exec()'s own return value is
    // whatever quit() was handed, which is 0 for every path including a failed
    // update -- the exact reason this contract exists.
    return UpdaterExitCodeFor(controller->flowState());
}
