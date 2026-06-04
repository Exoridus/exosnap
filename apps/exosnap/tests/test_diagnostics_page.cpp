#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include "pages/DiagnosticsPage.h"
#include "ui/widgets/PipelineFlow.h"
#include "ui/widgets/PipelineStepCard.h"

#include "models/OutputSettingsModel.h"
#include "models/VideoSettingsModel.h"

#include <capability/audio_ui_state.h>
#include <capability/capability_builder.h>
#include <capability/capability_set.h>

namespace exosnap {
namespace {

using ui::widgets::PipelineFlow;
using ui::widgets::PipelineStepCard;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "diagnostics_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class DiagnosticsPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    // Loads a validated baseline + the default MKV/H.264/AAC selection (which the
    // baseline supports), so the active configuration has no blockers.
    static void LoadData(DiagnosticsPage& page) {
        capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
        OutputSettingsModel output;
        output.container = capability::Container::Matroska;
        output.video_codec = capability::VideoCodec::H264Nvenc;
        output.audio_codec = capability::AudioCodec::AacMf;
        VideoSettingsModel video;
        capability::AudioUiState audio;
        page.setDiagnosticData(caps, output, video, audio, "MKV H.264 AAC", "Start/Stop: Alt+F9", "", true);
    }

    static QPushButton* FindButton(const DiagnosticsPage& page, const QString& text) {
        for (auto* button : page.findChildren<QPushButton*>())
            if (button->text() == text)
                return button;
        return nullptr;
    }
};

TEST_F(DiagnosticsPageTest, ContainsPipelineFlowWithCanonicalSteps) {
    DiagnosticsPage page;
    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    ASSERT_EQ(flow->stepCount(), 6);
    EXPECT_EQ(flow->card(0)->stepName(), QStringLiteral("Source Capture"));
    EXPECT_EQ(flow->card(1)->stepName(), QStringLiteral("Frame Queue"));
    EXPECT_EQ(flow->card(2)->stepName(), QStringLiteral("Compositor"));
    EXPECT_EQ(flow->card(3)->stepName(), QStringLiteral("Encoder"));
    EXPECT_EQ(flow->card(4)->stepName(), QStringLiteral("Muxer"));
    EXPECT_EQ(flow->card(5)->stepName(), QStringLiteral("Disk"));
}

TEST_F(DiagnosticsPageTest, RealStepsResolveAfterDataAndProbelessStepsStayPlanned) {
    DiagnosticsPage page;
    LoadData(page);
    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);

    // Real static checks on the validated baseline pass.
    EXPECT_EQ(flow->card(3)->status(), PipelineStepCard::Status::Ok); // Encoder
    EXPECT_EQ(flow->card(4)->status(), PipelineStepCard::Status::Ok); // Muxer
    EXPECT_EQ(flow->card(5)->status(), PipelineStepCard::Status::Ok); // Disk

    // Internal stages without a probe stay honestly Planned.
    EXPECT_EQ(flow->card(0)->status(), PipelineStepCard::Status::Planned);
    EXPECT_EQ(flow->card(1)->status(), PipelineStepCard::Status::Planned);
    EXPECT_EQ(flow->card(2)->status(), PipelineStepCard::Status::Planned);
}

TEST_F(DiagnosticsPageTest, CapabilityRowsRenderAfterData) {
    DiagnosticsPage page;
    LoadData(page);
    const QList<QWidget*> rows = page.findChildren<QWidget*>(QStringLiteral("diagTableRow"));
    EXPECT_FALSE(rows.isEmpty());
}

TEST_F(DiagnosticsPageTest, RunCheckUpdatesStatusToReady) {
    DiagnosticsPage page;
    LoadData(page);

    QPushButton* run = FindButton(page, QStringLiteral("Run Check"));
    ASSERT_NE(run, nullptr);
    run->click();

    // With the validated baseline and a supported configuration there are no
    // blockers, so the readiness pill reports READY (possibly with notices).
    bool found_ready = false;
    for (auto* label : page.findChildren<QLabel*>())
        if (label->text().contains(QStringLiteral("READY")))
            found_ready = true;
    EXPECT_TRUE(found_ready);
}

TEST_F(DiagnosticsPageTest, ExportReportStaysDisabledHonestly) {
    DiagnosticsPage page;
    LoadData(page);
    QPushButton* export_btn = FindButton(page, QStringLiteral("Export Report"));
    ASSERT_NE(export_btn, nullptr);
    EXPECT_FALSE(export_btn->isEnabled());
}

} // namespace
} // namespace exosnap
