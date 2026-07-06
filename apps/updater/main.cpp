// exosnap-updater -- standalone swap-updater process entry point.
//
// Parses the command line into UpdaterArgs and shows the updater window. A
// dev-only `--preview-state <progress|amber|red|green>` short-circuits all
// engine work and renders a canned UpdaterUiState so the four canon looks can be
// inspected (and screenshotted) without a real download/install in flight.

#include <QApplication>
#include <QScreen>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <optional>

#include "UpdaterArgs.h"
#include "UpdaterController.h"
#include "UpdaterWindow.h"

namespace {

constexpr char kPreviewFrom[] = "0.8.1";
constexpr char kPreviewTo[] = "0.9.0";

// Build one of the four canned preview states from the real controller so the
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

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const QStringList arguments = QCoreApplication::arguments();

    // Dev preview short-circuit: render a canned state and skip engine work.
    const int previewIdx = arguments.indexOf(QStringLiteral("--preview-state"));
    if (previewIdx >= 0) {
        const QString which =
            previewIdx + 1 < arguments.size() ? arguments.at(previewIdx + 1) : QString();
        const std::optional<UpdaterUiState> state = MakePreviewState(which);
        if (!state.has_value()) {
            std::fprintf(stderr,
                         "exosnap-updater: invalid --preview-state '%s' "
                         "(expected progress|amber|red|green)\n",
                         qPrintable(which));
            return 2;
        }
        UpdaterWindow window;
        window.render(*state);
        window.show();
        CenterOnScreen(window);
        return app.exec();
    }

    const std::optional<UpdaterArgs> args = ParseUpdaterArgs(arguments);
    if (!args.has_value()) {
        // ParseUpdaterArgs already wrote a diagnostic line to stderr.
        return 2;
    }

    // No engine driver yet (arrives in a later task): show the initial window in
    // its "download starting" state so the process has a visible surface.
    UpdaterController controller(args->current_version, args->current_version);
    controller.onStepStarted(UpStep::Download);

    UpdaterWindow window;
    window.render(controller.state());
    window.show();
    CenterOnScreen(window);

    return app.exec();
}
