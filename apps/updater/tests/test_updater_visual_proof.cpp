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
#include <QPushButton>

#include <array>

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

QString EvidenceDirectory() {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 12; ++i) {
        if (dir.exists(QStringLiteral(".git"))) {
            return dir.absoluteFilePath(QStringLiteral(".workspace/live-verify/rc5-pending/visual-after"));
        }
        if (!dir.cdUp())
            break;
    }
    return QCoreApplication::applicationDirPath();
}

UpdaterUiState FailureState(FailureCase failure, bool verify = false) {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    controller.setVerificationReinstall(verify);

    switch (failure) {
    case FailureCase::DownloadFailed:
    case FailureCase::VerifyDownloadFailed:
    case FailureCase::VerifyReinstallMismatch:
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

bool RenderEvidence(UpdaterWindow& window, const QString& filename, qreal dpr) {
    Settle(window);
    const QSize logical = window.size().expandedTo(QSize(1, 1));
    QImage image(QSize(qRound(logical.width() * dpr), qRound(logical.height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    window.QWidget::render(&painter);
    painter.end();

    const QString output_dir = EvidenceDirectory();
    QDir().mkpath(output_dir);
    return image.save(QDir(output_dir).absoluteFilePath(filename), "PNG");
}

void ExpectNoClippedCopy(const UpdaterWindow& window) {
    for (const auto* label : window.findChildren<QLabel*>()) {
        if (!label->isVisible() || label->wordWrap() || label->text().isEmpty())
            continue;
        EXPECT_GE(label->width(), label->sizeHint().width()) << "label clipped: " << label->text().toStdString();
    }
    for (const auto* button : window.findChildren<QPushButton*>()) {
        if (!button->isVisible() || button->text().isEmpty())
            continue;
        EXPECT_GE(button->width(), button->sizeHint().width()) << "button clipped: " << button->text().toStdString();
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

} // namespace
