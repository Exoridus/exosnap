// Deterministic offscreen visual evidence for the standalone updater.
//
// The production UpdaterWindow is rendered directly at representative Windows
// scale factors. No live ExoSnap instance, pointer synthesis or desktop capture
// is involved. PNGs are evidence products; assertions also catch clipped text.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPoint>
#include <QPushButton>
#include <QRect>

#include <array>
#include <string>

#include "ElidingLabel.h"
#include "UpdaterController.h"
#include "UpdaterWindow.h"

namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "updater_visual_proof";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

QString RepositoryRelativeDirectory(const QString& relative) {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 12; ++i) {
        if (dir.exists(QStringLiteral(".git")))
            return dir.absoluteFilePath(relative);
        if (!dir.cdUp())
            break;
    }
    return QCoreApplication::applicationDirPath();
}

// Deliberately NOT named after an RC. This set is rewritten by every run of
// these tests, so a folder called `rc5-pending` would keep that name while
// holding pixels rendered from a much later candidate — and the acceptance
// campaign's whole premise is that evidence is bound to the artefact it came
// from. The RC an evidence set belongs to is recorded by the campaign that
// copies it, not by a path baked into a test.
QString EvidenceDirectory() {
    return RepositoryRelativeDirectory(QStringLiteral(".workspace/live-verify/updater-visual/current"));
}

// Separate tree from the rolling evidence above, and separate again from
// `updater/current-pre-rc/`: that folder is the frozen BEFORE record of the
// pre-0.9 visual review (long versions clipped, a large "0 percent" over an
// orphan spinner, a failure state with the target version accent-filled) and is
// deliberately never rewritten. This is the AFTER set the freeze is declared on.
QString FinalBaselineDirectory() {
    return RepositoryRelativeDirectory(QStringLiteral(".workspace/visual-reference/updater/final-v0.9"));
}

UpdaterUiState FailureState(FailureCase failure, bool verify = false) {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    controller.setVerificationReinstall(verify);

    switch (failure) {
    case FailureCase::DownloadFailed:
    case FailureCase::VerifyDownloadFailed:
    case FailureCase::VerifyReinstallMismatch:
    // A0 -- refused before the pipeline was entered, so no step is marked done.
    case FailureCase::HandoffRejected:
        break;
    case FailureCase::AppWontClose:
        controller.onStepDone(UpStep::Download);
        break;
    case FailureCase::InstallFailed:
    case FailureCase::UacDeclined:
    case FailureCase::MsiFailed:
    case FailureCase::MsiRebootRequired:
        controller.onStepDone(UpStep::Download);
        controller.onStepDone(UpStep::CloseApp);
        break;
    case FailureCase::VerifyInstallFailed:
    case FailureCase::RestoreFailed:
    case FailureCase::VerifyInstallFailedMsi:
        controller.onStepDone(UpStep::Download);
        controller.onStepDone(UpStep::CloseApp);
        controller.onStepDone(UpStep::Install);
        break;
    case FailureCase::LaunchFailed:
        controller.onStepDone(UpStep::Download);
        controller.onStepDone(UpStep::CloseApp);
        controller.onStepDone(UpStep::Install);
        controller.onStepDone(UpStep::Verify);
        break;
    }

    controller.onFailure(failure, failure == FailureCase::MsiFailed ? QStringLiteral("1603") : QString());
    return controller.state();
}

UpdaterUiState WorkingState(UpStep active) {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    for (int i = 0; i < static_cast<int>(active); ++i)
        controller.onStepDone(static_cast<UpStep>(i));
    controller.onStepStarted(active);
    if (active == UpStep::Download)
        controller.onDownloadProgress(38, 100);
    return controller.state();
}

void Settle(QWidget& widget) {
    widget.move(-20000, -20000);
    widget.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        widget.ensurePolished();
        widget.adjustSize();
    }
}

bool RenderEvidenceInto(UpdaterWindow& window, const QString& output_dir, const QString& filename, qreal dpr) {
    Settle(window);
    const QSize logical = window.size().expandedTo(QSize(1, 1));
    QImage image(QSize(qRound(logical.width() * dpr), qRound(logical.height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    window.QWidget::render(&painter);
    painter.end();

    QDir().mkpath(output_dir);
    return image.save(QDir(output_dir).absoluteFilePath(filename), "PNG");
}

bool RenderEvidence(UpdaterWindow& window, const QString& filename, qreal dpr) {
    return RenderEvidenceInto(window, EvidenceDirectory(), filename, dpr);
}

// The text-layout contract, in four parts. The previous single check
// (`width >= sizeHint`) could not see any of the four clipping failures the
// pre-RC baseline actually shows:
//
//  1. NOTHING LEAVES THE WINDOW. A label whose own layout is satisfied can
//     still be pushed past the fixed 520x680 frame by the row it sits in --
//     which is precisely what the long version pills did. A per-label size
//     comparison passes that every time, because the label did get the width it
//     asked for; the WINDOW is what cut it off.
//  2. WHAT IS PAINTED FITS ITS OWN TEXT BOX. sizeHint() answers "what would I
//     like to be", not "am I clipped" -- and for an ElidingLabel it reports the
//     full string on purpose, so comparing against it would now fail on
//     correct, deliberately shortened output.
//  3. A SHORTENED STRING IS A DELIBERATE ELISION: it carries an ellipsis and
//     keeps the full value reachable rather than ending mid-token.
//  4. WRAPPED COPY FITS VERTICALLY. wordWrap labels were skipped outright
//     before, so body text clipped at the bottom of a fixed-height card passed
//     the assertion unseen.
void ExpectNoClippedCopy(const UpdaterWindow& window) {
    const QRect frame = window.rect();
    constexpr QChar kEllipsis(0x2026);

    for (const auto* label : window.findChildren<QLabel*>()) {
        if (!label->isVisible() || label->text().isEmpty())
            continue;
        const std::string what =
            label->objectName().isEmpty() ? label->text().toStdString() : label->objectName().toStdString();

        const QRect placed(label->mapTo(&window, QPoint(0, 0)), label->size());
        EXPECT_TRUE(frame.contains(placed)) << "leaves the 520x680 window: " << what;

        if (const auto* eliding = qobject_cast<const ElidingLabel*>(label)) {
            EXPECT_LE(eliding->fontMetrics().size(Qt::TextSingleLine, eliding->text()).width(),
                      eliding->textAreaWidth())
                << "elided text still overflows its own box: " << what;
            if (eliding->isElided()) {
                EXPECT_TRUE(eliding->text().contains(kEllipsis)) << "shortened without an ellipsis: " << what;
                EXPECT_EQ(eliding->toolTip(), eliding->fullText()) << "elided label drops the full value: " << what;
            }
            continue;
        }

        if (label->wordWrap()) {
            EXPECT_LE(label->heightForWidth(label->width()), label->height())
                << "wrapped copy clipped at the bottom: " << what;
            continue;
        }

        // The wordmark is markup; its string cannot be measured with plain font
        // metrics, so it keeps the size-hint comparison.
        if (label->textFormat() == Qt::RichText) {
            EXPECT_GE(label->width(), label->sizeHint().width()) << "rich-text label clipped: " << what;
            continue;
        }

        EXPECT_LE(label->fontMetrics().size(Qt::TextSingleLine, label->text()).width(), label->width())
            << "plain label clipped: " << what;
    }

    for (const auto* button : window.findChildren<QPushButton*>()) {
        if (!button->isVisible() || button->text().isEmpty())
            continue;
        EXPECT_GE(button->width(), button->sizeHint().width()) << "button clipped: " << button->text().toStdString();
        const QRect placed(button->mapTo(&window, QPoint(0, 0)), button->size());
        EXPECT_TRUE(frame.contains(placed)) << "button leaves the window: " << button->text().toStdString();
    }
}

class UpdaterVisualProofTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
        QDir().mkpath(EvidenceDirectory());
    }
};

TEST_F(UpdaterVisualProofTest, FailureMatrixAtRepresentativeDpiScales) {
    struct Scenario {
        const char* filename;
        FailureCase failure;
        qreal dpr;
        bool verify;
    };
    constexpr std::array<Scenario, 5> scenarios = {{
        {"01-download-failed-dpr100.png", FailureCase::DownloadFailed, 1.0, false},
        {"02-uac-declined-verify-dpr125.png", FailureCase::UacDeclined, 1.25, true},
        {"03-install-failed-dpr150.png", FailureCase::InstallFailed, 1.5, false},
        {"04-verify-restored-dpr200.png", FailureCase::VerifyInstallFailed, 2.0, false},
        {"05-launch-manual-dpr200.png", FailureCase::LaunchFailed, 2.0, false},
    }};

    for (const Scenario& scenario : scenarios) {
        UpdaterWindow window;
        window.render(FailureState(scenario.failure, scenario.verify));
        ExpectNoClippedCopy(window);
        EXPECT_TRUE(RenderEvidence(window, QString::fromLatin1(scenario.filename), scenario.dpr)) << scenario.filename;
    }

    struct WorkingScenario {
        const char* filename;
        UpStep step;
        qreal dpr;
    };
    constexpr std::array<WorkingScenario, 3> working = {{
        {"06-downloading-dpr100.png", UpStep::Download, 1.0},
        {"07-installing-dpr150.png", UpStep::Install, 1.5},
        {"08-verifying-dpr200.png", UpStep::Verify, 2.0},
    }};
    for (const WorkingScenario& scenario : working) {
        UpdaterWindow window;
        window.render(WorkingState(scenario.step));
        ExpectNoClippedCopy(window);
        EXPECT_TRUE(RenderEvidence(window, QString::fromLatin1(scenario.filename), scenario.dpr)) << scenario.filename;
    }
}

// The five scenarios above are the RC5 evidence set and their filenames are
// referenced from that live-verify folder, so they are left alone. This covers
// the REST of the matrix: every remaining FailureCase and the two UpSteps the
// set above skips, so the standalone updater's visual identity is preserved in
// full before the Qt Quick cutover retires the Widgets frontend around it.
//
// FailureState() already knows how to reach all twelve cases; only the scenario
// table was partial. Rendering is deterministic and offscreen — no live updater
// window, no network, no install, nothing to coordinate with the developer.
TEST_F(UpdaterVisualProofTest, RemainingFailureMatrixForVisualBaseline) {
    struct Scenario {
        const char* filename;
        FailureCase failure;
        qreal dpr;
        bool verify;
    };
    // Both reference DPIs from the cutover plan (100% / 150%). A case is rendered
    // at one of them, not both: the two scales exercise the same layout code and
    // a second copy would add files without adding information.
    constexpr std::array<Scenario, 7> scenarios = {{
        // A2 — security stop after a failed download verification.
        {"10-verify-download-failed-dpr100.png", FailureCase::VerifyDownloadFailed, 1.0, false},
        // A3 — verification reinstall refused because the version did not match.
        {"11-verify-reinstall-mismatch-dpr150.png", FailureCase::VerifyReinstallMismatch, 1.5, true},
        // B1 — the running app would not close.
        {"12-app-wont-close-dpr100.png", FailureCase::AppWontClose, 1.0, false},
        // B3-R — restore incomplete, backup preserved.
        {"13-restore-failed-dpr150.png", FailureCase::RestoreFailed, 1.5, false},
        // B3-MSI — msiexec rolled back to the previous version.
        {"14-verify-install-failed-msi-dpr100.png", FailureCase::VerifyInstallFailedMsi, 1.0, false},
        // C2 — msiexec itself failed; the detail string carries the exit code.
        {"15-msi-failed-dpr150.png", FailureCase::MsiFailed, 1.5, false},
        // C3 — terminal SUCCESS with a pending restart, not a failure despite
        // travelling through onFailure(). The only TerminalVariant the evidence
        // set had no picture of at all.
        {"16-msi-reboot-required-dpr100.png", FailureCase::MsiRebootRequired, 1.0, false},
    }};

    for (const Scenario& scenario : scenarios) {
        UpdaterWindow window;
        window.render(FailureState(scenario.failure, scenario.verify));
        ExpectNoClippedCopy(window);
        EXPECT_TRUE(RenderEvidence(window, QString::fromLatin1(scenario.filename), scenario.dpr)) << scenario.filename;
    }

    // The two steps the working set above skips, so every UpStep has a picture.
    struct WorkingScenario {
        const char* filename;
        UpStep step;
        qreal dpr;
    };
    constexpr std::array<WorkingScenario, 2> working = {{
        {"17-closing-app-dpr100.png", UpStep::CloseApp, 1.0},
        {"18-launching-dpr150.png", UpStep::Launch, 1.5},
    }};
    for (const WorkingScenario& scenario : working) {
        UpdaterWindow window;
        window.render(WorkingState(scenario.step));
        ExpectNoClippedCopy(window);
        EXPECT_TRUE(RenderEvidence(window, QString::fromLatin1(scenario.filename), scenario.dpr)) << scenario.filename;
    }
}

// ── Final v0.9 baseline ─────────────────────────────────────────────────────
//
// The two matrices above are frozen evidence sets whose filenames are cited from
// their own live-verify folders, so they are left exactly as they are. This test
// writes ONE self-contained folder holding every visually distinct state the
// shipping UpdaterWindow can reach, so a reviewer reads one directory instead of
// reconstructing the matrix from three partial ones. It covers the states no
// other capture reaches at all — the pre-flight frame the window opens on, the
// terminal Success card, and a verification-reinstall run mid-work — plus the
// realistic long-content cases.
//
// It writes `final-v0.9/`, NOT the `current-pre-rc/` folder next to it: that one
// is the frozen BEFORE record of the pre-0.9 review and the two are meant to be
// compared, so nothing may overwrite it.
//
// Same mechanism as above: the production window, driven by the production
// controller, rendered offscreen. No network, no install, no live process.
namespace baseline {

UpdaterUiState IdleState() {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    return controller.state();
}

UpdaterUiState SuccessState() {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    controller.onAllDone();
    return controller.state();
}

UpdaterUiState ReinstallWorkingState(UpStep active) {
    UpdaterController controller(QStringLiteral("0.9.0-rc5"), QStringLiteral("0.9.0-rc5"));
    controller.setVerificationReinstall(true);
    for (int i = 0; i < static_cast<int>(active); ++i)
        controller.onStepDone(static_cast<UpStep>(i));
    controller.onStepStarted(active);
    if (active == UpStep::Download)
        controller.onDownloadProgress(38, 100);
    return controller.state();
}

// The longest input the shipping updater can actually receive: version strings
// come from UpdaterArgs (--from/--to), i.e. whatever the release manifest names,
// and they land in the pills, the headline, the status line and the safety line.
// A pre-release + build-metadata semver is the realistic maximum, not a filler
// string.
constexpr char kLongFrom[] = "0.9.0-rc4+build.20260810.a5d55f1.windows-x64";
constexpr char kLongTo[] = "0.9.0-rc5+build.20260812.eba270a.windows-x64";

UpdaterUiState LongVersionSoftSuccessState() {
    UpdaterController controller(QString::fromLatin1(kLongFrom), QString::fromLatin1(kLongTo));
    controller.onStepDone(UpStep::Download);
    controller.onStepDone(UpStep::CloseApp);
    controller.onStepDone(UpStep::Install);
    controller.onStepDone(UpStep::Verify);
    controller.onFailure(FailureCase::LaunchFailed, QString());
    return controller.state();
}

UpdaterUiState LongVersionWorkingState() {
    UpdaterController controller(QString::fromLatin1(kLongFrom), QString::fromLatin1(kLongTo));
    controller.onStepStarted(UpStep::Download);
    controller.onDownloadProgress(38, 100);
    return controller.state();
}

// C2 carries msiexec's own exit text through `detail`. A descriptive Windows
// Installer message is a real value for it, not only a bare code.
UpdaterUiState LongMsiDetailState() {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    controller.onStepDone(UpStep::Download);
    controller.onStepDone(UpStep::CloseApp);
    controller.onFailure(FailureCase::MsiFailed,
                         QStringLiteral("1603 (fatal error during installation, ERROR_INSTALL_FAILURE)"));
    return controller.state();
}

} // namespace baseline

TEST_F(UpdaterVisualProofTest, FinalV09BaselineMatrix) {
    const QString dir = FinalBaselineDirectory();
    QDir().mkpath(dir);

    struct Case {
        const char* filename;
        UpdaterUiState state;
    };
    const std::array<Case, 23> cases = {{
        // Pre-flight: what the window paints before the worker reports anything.
        {"01-idle-ready-dpr100.png", baseline::IdleState()},
        // Working steps, one per UpStep.
        {"02-downloading-dpr100.png", WorkingState(UpStep::Download)},
        {"03-closing-app-dpr100.png", WorkingState(UpStep::CloseApp)},
        {"04-installing-dpr100.png", WorkingState(UpStep::Install)},
        {"05-verifying-dpr100.png", WorkingState(UpStep::Verify)},
        {"06-launching-dpr100.png", WorkingState(UpStep::Launch)},
        // Terminal success — the state the window auto-closes out of after 1.5 s.
        {"07-success-dpr100.png", baseline::SuccessState()},
        // Amber (recoverable) terminal cases.
        {"08-amber-download-failed-dpr100.png", FailureState(FailureCase::DownloadFailed)},
        {"09-amber-app-wont-close-dpr100.png", FailureState(FailureCase::AppWontClose)},
        {"10-amber-install-failed-dpr100.png", FailureState(FailureCase::InstallFailed)},
        {"11-amber-uac-declined-dpr100.png", FailureState(FailureCase::UacDeclined)},
        // Red (security / integrity) terminal cases.
        {"12-red-verify-download-failed-dpr100.png", FailureState(FailureCase::VerifyDownloadFailed)},
        {"13-red-verify-reinstall-mismatch-dpr100.png", FailureState(FailureCase::VerifyReinstallMismatch, true)},
        {"14-red-verify-install-failed-dpr100.png", FailureState(FailureCase::VerifyInstallFailed)},
        {"15-red-restore-failed-dpr100.png", FailureState(FailureCase::RestoreFailed)},
        {"16-red-verify-install-failed-msi-dpr100.png", FailureState(FailureCase::VerifyInstallFailedMsi)},
        {"17-red-msi-failed-dpr100.png", FailureState(FailureCase::MsiFailed)},
        // Green soft success and the pending-restart terminal success.
        {"18-green-launch-failed-dpr100.png", FailureState(FailureCase::LaunchFailed)},
        {"19-reboot-required-dpr100.png", FailureState(FailureCase::MsiRebootRequired)},
        // ADR 0055 verification reinstall: same layout, different wording, and no
        // version change to announce.
        {"20-reinstall-downloading-dpr100.png", baseline::ReinstallWorkingState(UpStep::Download)},
        {"21-reinstall-installing-dpr100.png", baseline::ReinstallWorkingState(UpStep::Install)},
        // Long content: maximal real version strings, and msiexec's descriptive
        // failure text in the C2 detail slot.
        {"22-long-versions-launch-failed-dpr100.png", baseline::LongVersionSoftSuccessState()},
        {"23-long-msi-detail-dpr100.png", baseline::LongMsiDetailState()},
    }};

    for (const Case& scenario : cases) {
        UpdaterWindow window;
        window.render(scenario.state);
        ExpectNoClippedCopy(window);
        EXPECT_TRUE(RenderEvidenceInto(window, dir, QString::fromLatin1(scenario.filename), 1.0)) << scenario.filename;
    }

    // Cancel confirmation is reached by pressing the button, not by a state.
    {
        UpdaterWindow window;
        window.render(WorkingState(UpStep::Download));
        Settle(window);
        auto* cancel = window.findChild<QPushButton*>(QStringLiteral("updaterCancelButton"));
        ASSERT_NE(cancel, nullptr);
        cancel->click();
        ASSERT_TRUE(window.cancelConfirmationVisible());
        EXPECT_TRUE(RenderEvidenceInto(window, dir, QStringLiteral("24-cancel-confirm-dpr100.png"), 1.0));
    }

    // DPI ladder. The window is a fixed 520x680 logical size, so scale is the
    // only responsive axis it has; three representative states carry it rather
    // than the whole matrix being multiplied by three.
    struct ScaledCase {
        const char* filename;
        UpdaterUiState state;
        qreal dpr;
    };
    const std::array<ScaledCase, 6> scaled = {{
        {"30-downloading-dpr150.png", WorkingState(UpStep::Download), 1.5},
        {"31-downloading-dpr200.png", WorkingState(UpStep::Download), 2.0},
        {"32-amber-install-failed-dpr150.png", FailureState(FailureCase::InstallFailed), 1.5},
        {"33-amber-install-failed-dpr200.png", FailureState(FailureCase::InstallFailed), 2.0},
        {"34-long-versions-downloading-dpr150.png", baseline::LongVersionWorkingState(), 1.5},
        {"35-long-versions-downloading-dpr200.png", baseline::LongVersionWorkingState(), 2.0},
    }};
    for (const ScaledCase& scenario : scaled) {
        UpdaterWindow window;
        window.render(scenario.state);
        ExpectNoClippedCopy(window);
        EXPECT_TRUE(RenderEvidenceInto(window, dir, QString::fromLatin1(scenario.filename), scenario.dpr))
            << scenario.filename;
    }
}

TEST_F(UpdaterVisualProofTest, SafeCancelConfirmationAtRepresentativeDpi) {
    UpdaterWindow window;
    window.render(WorkingState(UpStep::Download));
    Settle(window);
    auto* cancel = window.findChild<QPushButton*>(QStringLiteral("updaterCancelButton"));
    ASSERT_NE(cancel, nullptr);
    cancel->click();
    ASSERT_TRUE(window.cancelConfirmationVisible());
    EXPECT_TRUE(RenderEvidence(window, QStringLiteral("09-cancel-confirm-dpr125.png"), 1.25));
}

} // namespace
