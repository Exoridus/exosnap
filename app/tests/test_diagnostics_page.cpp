#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QString>
#include <QToolButton>
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

    // Loads a validated baseline + the product-default MKV/AV1/Opus selection, which the
    // baseline fully supports (Recommended path), so the active configuration has no
    // blockers AND no recommendations (AV1 is already the best GPU-supported codec, so
    // rec.profile.codec stays silent — see RecommendationEngine::checkRecommendedCodec).
    static void LoadData(DiagnosticsPage& page) {
        capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
        OutputSettingsModel output;
        output.container = capability::Container::Matroska;
        output.video_codec = capability::VideoCodec::Av1Nvenc;
        output.audio_codec = capability::AudioCodec::Opus;
        VideoSettingsModel video;
        capability::AudioUiState audio;
        page.setDiagnosticData(caps, output, video, audio, "MKV AV1 Opus", "Start/Stop: Alt+F9", "", true);
    }

    static QPushButton* FindButton(const DiagnosticsPage& page, const QString& text) {
        for (auto* button : page.findChildren<QPushButton*>())
            if (button->text() == text)
                return button;
        return nullptr;
    }

    // Reads the value label inside a named readiness tile (Readiness/Encoder/Disk/Display).
    static QString TileValue(const DiagnosticsPage& page, const QString& tile_object_name) {
        auto* tile = page.findChild<QFrame*>(tile_object_name);
        if (!tile)
            return QString();
        for (auto* lbl : tile->findChildren<QLabel*>())
            if (lbl->property("labelRole").toString() == QLatin1String("readinessTileValue"))
                return lbl->text();
        return QString();
    }

    static QString TileTone(const DiagnosticsPage& page, const QString& tile_object_name) {
        auto* tile = page.findChild<QFrame*>(tile_object_name);
        return tile ? tile->property("tileTone").toString() : QString();
    }

    static QToolButton* FindCollapseHead(const DiagnosticsPage& page, const QString& text_fragment) {
        for (auto* tb : page.findChildren<QToolButton*>())
            if (tb->property("role").toString() == QLatin1String("collapseHead") && tb->text().contains(text_fragment))
                return tb;
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
    // blockers and no diagnosed issues, so the readiness pill reports exactly READY
    // (#6: capability-matrix unavailability does not trigger amber notices).
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

// ---- Phase D: Readiness verdicts (suite-diag.jsx clear/issues/blocked) ------

TEST_F(DiagnosticsPageTest, VerdictClearShowsReadyPillAndPassTileTone) {
    // Validated baseline with a supported config → no blockers, no notices from
    // the recommendation engine → "clear" state (READY pill + pass-toned Readiness tile).
    DiagnosticsPage page;
    LoadData(page);

    QPushButton* run = FindButton(page, QStringLiteral("Run Check"));
    ASSERT_NE(run, nullptr);
    run->click();

    bool has_ready_pill = false;
    for (auto* lbl : page.findChildren<QLabel*>())
        if (lbl->property("labelRole").toString() == QLatin1String("profileStatusBadge") &&
            lbl->text().contains(QStringLiteral("READY")))
            has_ready_pill = true;
    EXPECT_TRUE(has_ready_pill) << "Expected READY in status pill for a clean baseline config";

    // The Readiness tile is pass-toned and shows an "N / M checks passed" value.
    EXPECT_EQ(TileTone(page, QStringLiteral("readinessTileReadiness")), QStringLiteral("pass"));
    EXPECT_TRUE(TileValue(page, QStringLiteral("readinessTileReadiness")).contains(QStringLiteral("/")))
        << "Readiness tile should show an N / M checks value in the clear verdict";
}

TEST_F(DiagnosticsPageTest, VerdictPipelineStagesAllPresent) {
    // Canonical six stages must be present and named correctly (suite-diag.jsx order).
    DiagnosticsPage page;
    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    ASSERT_EQ(flow->stepCount(), 6);
    // Per suite-diag.jsx: Source → Queue → Compositor → Encoder → Muxer → Disk
    EXPECT_EQ(flow->card(0)->stepName(), QStringLiteral("Source Capture"));
    EXPECT_EQ(flow->card(1)->stepName(), QStringLiteral("Frame Queue"));
    EXPECT_EQ(flow->card(2)->stepName(), QStringLiteral("Compositor"));
    EXPECT_EQ(flow->card(3)->stepName(), QStringLiteral("Encoder"));
    EXPECT_EQ(flow->card(4)->stepName(), QStringLiteral("Muxer"));
    EXPECT_EQ(flow->card(5)->stepName(), QStringLiteral("Disk"));
}

TEST_F(DiagnosticsPageTest, FourReadinessTilesPresent) {
    // suite-diag2.jsx ReadinessGrid: exactly four wide tiles — Readiness · Encoder ·
    // Disk · Display — replace the old three blocker/issue/pass stat tiles.
    DiagnosticsPage page;
    for (const char* name :
         {"readinessTileReadiness", "readinessTileEncoder", "readinessTileDisk", "readinessTileDisplay"}) {
        auto* tile = page.findChild<QFrame*>(QString::fromLatin1(name));
        EXPECT_NE(tile, nullptr) << "Missing readiness tile: " << name;
        if (tile)
            EXPECT_EQ(tile->property("panelRole").toString(), QStringLiteral("readinessTile"));
    }
    // The old three-tile labels must be gone.
    for (auto* lbl : page.findChildren<QLabel*>())
        EXPECT_NE(lbl->property("labelRole").toString(), QLatin1String("statTileLabel"))
            << "Old stat-tile labels must be removed by the redesign";
}

TEST_F(DiagnosticsPageTest, ReadinessTilesPopulateAfterData) {
    DiagnosticsPage page;
    LoadData(page);
    QPushButton* run = FindButton(page, QStringLiteral("Run Check"));
    ASSERT_NE(run, nullptr);
    run->click();

    const QString dash = QString::fromUtf8("\xe2\x80\x94");
    // Encoder tile reflects the active video codec (AV1 baseline in LoadData).
    EXPECT_TRUE(TileValue(page, QStringLiteral("readinessTileEncoder")).contains(QStringLiteral("AV1")))
        << "Encoder tile should show the active video codec";
    // Display tile shows a resolution (× glyph), never the em-dash, once a screen exists.
    EXPECT_NE(TileValue(page, QStringLiteral("readinessTileDisplay")), dash);
}

// ---- v10 verdict logic (#6): capability matrix unavailability is NOT amber ----

TEST_F(DiagnosticsPageTest, VerdictClearNoBannerAmber) {
    // Validated baseline with a supported config → no real diagnosed issues →
    // the readiness panel must NOT carry stateRole="warn" or stateRole="blocked".
    // Un-instrumented pipeline stages and unavailable-but-unselected capabilities
    // must not trigger amber coloring. (suite-diag.jsx: "calm when fine")
    DiagnosticsPage page;
    LoadData(page);

    QPushButton* run = FindButton(page, QStringLiteral("Run Check"));
    ASSERT_NE(run, nullptr);
    run->click();

    // The readiness panel must have no stateRole set (neutral) — NOT "warn" or "blocked".
    bool panel_is_amber = false;
    for (auto* frame : page.findChildren<QFrame*>()) {
        if (frame->property("panelRole").toString() == QLatin1String("readinessBanner")) {
            const QString role = frame->property("stateRole").toString();
            if (role == QLatin1String("warn") || role == QLatin1String("blocked"))
                panel_is_amber = true;
        }
    }
    EXPECT_FALSE(panel_is_amber)
        << "Readiness banner must not be amber/red in a clear all-pass verdict (#6: no false alarm)";
}

TEST_F(DiagnosticsPageTest, VerdictClearPillTextIsJustReady) {
    // In clear state the pill must read exactly "READY" with no notice suffix
    // (old code produced "READY · N NOTICES" which was wrong per v10).
    DiagnosticsPage page;
    LoadData(page);

    QPushButton* run = FindButton(page, QStringLiteral("Run Check"));
    ASSERT_NE(run, nullptr);
    run->click();

    bool pill_exact_ready = false;
    for (auto* lbl : page.findChildren<QLabel*>()) {
        if (lbl->property("labelRole").toString() == QLatin1String("profileStatusBadge") &&
            lbl->text() == QStringLiteral("READY"))
            pill_exact_ready = true;
    }
    EXPECT_TRUE(pill_exact_ready) << "Status pill must read exactly 'READY' (no notice count) in a clear verdict";
}

// ---- Simple / Expert redesign (suite-diag2.jsx) ------------------------------

TEST_F(DiagnosticsPageTest, ExpertContainerHiddenInSimpleShownInExpert) {
    // The full taxonomy (② Pre-flight → ③ Live → ④ Post-flight + elevation) lives in
    // the Expert-only container; Simple shows only verdict + tiles + worst-first cards.
    DiagnosticsPage page;
    auto* expert = page.findChild<QWidget*>(QStringLiteral("diagExpertContainer"));
    ASSERT_NE(expert, nullptr);
    EXPECT_FALSE(page.isExpertModeEnabled());
    EXPECT_TRUE(expert->isHidden()) << "Expert container must be hidden in Simple (default) view";

    page.setExpertModeEnabled(true);
    EXPECT_TRUE(page.isExpertModeEnabled());
    EXPECT_FALSE(expert->isHidden()) << "Expert container must be revealed in Expert view";

    page.setExpertModeEnabled(false);
    EXPECT_TRUE(expert->isHidden());
}

TEST_F(DiagnosticsPageTest, ExpertToggleEmitsExactlyOnUserFlip) {
    // The toolbar toggle emits expertModeChanged; the external setter (MainWindow sync)
    // must NOT re-emit, so cross-page sync can't loop.
    DiagnosticsPage page;
    int emits = 0;
    QObject::connect(&page, &DiagnosticsPage::expertModeChanged, &page, [&emits](bool) { ++emits; });

    page.setExpertModeEnabled(true); // external sync → no emit
    EXPECT_EQ(emits, 0);

    auto* toggle = page.findChild<QAbstractButton*>(QStringLiteral("diagExpertModeToggleBtn"));
    ASSERT_NE(toggle, nullptr);
    toggle->click(); // user flip (true → false) → exactly one emit
    EXPECT_EQ(emits, 1);
}

TEST_F(DiagnosticsPageTest, DeviceLinkEmitsOpenDeviceRequest) {
    // Capability facts moved to the Device page; the Environment row links there.
    DiagnosticsPage page;
    int emits = 0;
    QObject::connect(&page, &DiagnosticsPage::openDevicePageRequested, &page, [&emits]() { ++emits; });
    auto* btn = page.findChild<QPushButton*>(QStringLiteral("openDeviceBtn"));
    ASSERT_NE(btn, nullptr);
    btn->click();
    EXPECT_EQ(emits, 1);
}

TEST_F(DiagnosticsPageTest, NoCapabilityMatrixSectionRemains) {
    // The capability matrix is removed (it now lives on the Device page). No section
    // header should carry the old "CAPABILITY MATRIX" title.
    DiagnosticsPage page;
    LoadData(page);
    for (auto* lbl : page.findChildren<QLabel*>())
        EXPECT_FALSE(lbl->text().contains(QStringLiteral("CAPABILITY MATRIX"), Qt::CaseInsensitive))
            << "Capability matrix must be removed from Diagnostics";
}

TEST_F(DiagnosticsPageTest, ActiveConfigAccordionTogglesBody) {
    // makeCollapsibleSection is the reusable accordion primitive: clicking the head
    // reveals the body. Active configuration starts collapsed.
    DiagnosticsPage page;
    LoadData(page);
    auto* body = page.findChild<QWidget*>(QStringLiteral("diagActiveConfigBody"));
    ASSERT_NE(body, nullptr);
    EXPECT_TRUE(body->isHidden()) << "Active configuration must start collapsed";

    auto* head = FindCollapseHead(page, QStringLiteral("Active configuration"));
    ASSERT_NE(head, nullptr);
    head->click();
    EXPECT_FALSE(body->isHidden()) << "Clicking the accordion head must reveal the body";
    head->click();
    EXPECT_TRUE(body->isHidden()) << "Clicking again must collapse the body";
}

TEST_F(DiagnosticsPageTest, ElevationRestartStaysDisabledHonestly) {
    // The self-relaunch flow is a later slice; until then the button must be
    // disabled (planned), never a live-looking control that silently no-ops.
    DiagnosticsPage page;
    auto* btn = page.findChild<QPushButton*>(QStringLiteral("elevationRestartBtn"));
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->isEnabled());
}

TEST_F(DiagnosticsPageTest, VerdictLastCheckLabelUpdatesAfterRunCheck) {
    DiagnosticsPage page;
    LoadData(page);

    QPushButton* run = FindButton(page, QStringLiteral("Run Check"));
    ASSERT_NE(run, nullptr);

    // Before run check the label shows the em-dash placeholder.
    bool has_placeholder = false;
    for (auto* lbl : page.findChildren<QLabel*>())
        if (lbl->property("labelRole").toString() == QLatin1String("subtle") &&
            lbl->text().contains(QStringLiteral("Last check:")))
            has_placeholder = true;
    EXPECT_TRUE(has_placeholder);

    run->click();

    // After run check the label must show a real timestamp (not the em-dash).
    bool has_timestamp = false;
    for (auto* lbl : page.findChildren<QLabel*>())
        if (lbl->property("labelRole").toString() == QLatin1String("subtle") &&
            lbl->text().contains(QStringLiteral("Last check:")) &&
            !lbl->text().contains(QString::fromUtf8("\xe2\x80\x94")) &&
            !lbl->text().contains(QStringLiteral("running")))
            has_timestamp = true;
    EXPECT_TRUE(has_timestamp) << "Last check label should show a date/time after Run Check";
}

// ---- Live pipeline diagnostics (DIAGNOSTICS-LIVE-PIPELINE-R1) ----------------

namespace {

const QString kDash = QString::fromUtf8("\xE2\x80\x94");

recorder_core::RecordingDiagnosticsSnapshot MakeRecordingSnapshot() {
    using namespace recorder_core;
    RecordingDiagnosticsSnapshot s;
    s.session_generation = 1;
    s.lifecycle = DiagnosticsLifecycle::Recording;
    s.valid = true;
    s.elapsed_seconds = 12.0;
    s.capture.target_fps = 60.0;
    s.capture.actual_fps = 59.8;
    s.capture.frames_captured = 720;
    s.capture.frames_emitted = 700;
    s.capture.source_type = CaptureSourceType::Display;
    s.compositor.active = true;
    s.compositor.average_ms = 1.4;
    s.compositor.peak_ms = 2.8;
    s.video_encoder.frames_submitted = 700;
    s.video_encoder.frames_encoded = 700;
    s.video_encoder.output_fps = 60.0;
    s.video_encoder.codec = VideoCodec::Av1Nvenc;
    s.video_encoder.width = 1920;
    s.video_encoder.height = 1080;
    s.video_encoder.cfr = true;
    s.audio.active = true;
    s.audio.sample_rate = 48000;
    s.audio.channels = 2;
    s.audio.codec = AudioCodec::Opus;
    s.audio.track_count = 1;
    s.video_queue.bounded = false;
    s.audio_queue.bounded = true;
    s.audio_queue.capacity = 600;
    s.mux.availability = MetricAvailability::Available;
    s.disk.output_target = "C:";
    s.disk.latency_availability = MetricAvailability::Available;
    s.split.split_supported = true;
    s.split.current_segment = 1;
    s.bottleneck = PipelineBottleneck::None;
    s.health = PipelineHealth::Good;
    return s;
}

QString LiveValue(const DiagnosticsPage& page, const QString& key) {
    auto* label = page.findChild<QLabel*>(key);
    return label ? label->text() : QString();
}

} // namespace

TEST_F(DiagnosticsPageTest, LiveDiagnosticsIdleShowsNeutralNotStale) {
    DiagnosticsPage page;
    recorder_core::RecordingDiagnosticsSnapshot idle;
    idle.lifecycle = recorder_core::DiagnosticsLifecycle::Idle;
    idle.valid = false;
    page.applyLiveDiagnostics(idle);

    EXPECT_EQ(LiveValue(page, QStringLiteral("liveCaptureFps")), kDash);
    EXPECT_EQ(LiveValue(page, QStringLiteral("liveEncoderCounts")), kDash);
    EXPECT_EQ(LiveValue(page, QStringLiteral("livePipelineLifecycle")), QStringLiteral("No active recording"));
}

TEST_F(DiagnosticsPageTest, LiveDiagnosticsRecordingShowsLiveMetrics) {
    DiagnosticsPage page;
    page.applyLiveDiagnostics(MakeRecordingSnapshot());

    EXPECT_TRUE(LiveValue(page, QStringLiteral("liveCaptureFps")).contains(QStringLiteral("59.8")));
    EXPECT_TRUE(LiveValue(page, QStringLiteral("liveCaptureFps")).contains(QStringLiteral("60.0")));
    EXPECT_TRUE(LiveValue(page, QStringLiteral("liveEncoderCounts")).contains(QStringLiteral("submitted 700")));
    EXPECT_TRUE(LiveValue(page, QStringLiteral("livePipelineLifecycle")).contains(QStringLiteral("Recording")));
}

TEST_F(DiagnosticsPageTest, LiveDiagnosticsIdleAfterRecordingClearsStaleValues) {
    DiagnosticsPage page;
    page.applyLiveDiagnostics(MakeRecordingSnapshot());
    ASSERT_TRUE(LiveValue(page, QStringLiteral("liveCaptureFps")).contains(QStringLiteral("59.8")));

    recorder_core::RecordingDiagnosticsSnapshot idle;
    idle.lifecycle = recorder_core::DiagnosticsLifecycle::Idle;
    idle.valid = false;
    page.applyLiveDiagnostics(idle);

    // No stale active-looking value remains.
    EXPECT_EQ(LiveValue(page, QStringLiteral("liveCaptureFps")), kDash);
    EXPECT_FALSE(LiveValue(page, QStringLiteral("liveCaptureFps")).contains(QStringLiteral("59.8")));
}

TEST_F(DiagnosticsPageTest, LiveDiagnosticsPausedShowsStableState) {
    DiagnosticsPage page;
    auto s = MakeRecordingSnapshot();
    s.lifecycle = recorder_core::DiagnosticsLifecycle::Paused;
    page.applyLiveDiagnostics(s);
    EXPECT_TRUE(LiveValue(page, QStringLiteral("livePipelineLifecycle")).contains(QStringLiteral("Paused")));
}

TEST_F(DiagnosticsPageTest, LiveDiagnosticsUnavailableNotShownAsZero) {
    DiagnosticsPage page;
    auto s = MakeRecordingSnapshot();
    // MP4-style: no measurable write boundary, no reorder window / split.
    s.mux.availability = recorder_core::MetricAvailability::Unavailable;
    s.disk.latency_availability = recorder_core::MetricAvailability::Unavailable;
    s.split.split_supported = false;
    s.split.availability = recorder_core::MetricAvailability::Unavailable;
    page.applyLiveDiagnostics(s);

    EXPECT_TRUE(LiveValue(page, QStringLiteral("liveReorder")).contains(QStringLiteral("Unavailable")));
    EXPECT_TRUE(LiveValue(page, QStringLiteral("liveMuxWrite")).contains(QStringLiteral("Unavailable")));
    EXPECT_TRUE(LiveValue(page, QStringLiteral("liveSegment")).contains(QStringLiteral("Single file")));
}

TEST_F(DiagnosticsPageTest, LiveDiagnosticsBottleneckStatusRendered) {
    DiagnosticsPage page;
    auto s = MakeRecordingSnapshot();
    s.bottleneck = recorder_core::PipelineBottleneck::VideoEncoder;
    s.bottleneck_reason = "Encoder backlog rising";
    s.health = recorder_core::PipelineHealth::Warning;
    page.applyLiveDiagnostics(s);

    const QString status = LiveValue(page, QStringLiteral("livePipelineStatus"));
    EXPECT_TRUE(status.contains(QStringLiteral("VideoEncoder")));
    EXPECT_TRUE(status.contains(QStringLiteral("Warning")));
}

// ---- Present-mode provider bridge (ADR 0033) ------------------------------------

class StubPresent final : public exosnap::diagnostics::IPresentProvider {
  public:
    exosnap::diagnostics::PresentSample Sample() const override {
        exosnap::diagnostics::PresentSample s;
        s.available = true;
        s.mode = exosnap::diagnostics::PresentMode::IndependentFlip;
        s.tearing = true;
        return s;
    }
    bool IsAvailable() const override {
        return true;
    }
};

TEST_F(DiagnosticsPageTest, PresentProvider_InjectedModeAppearsInLivePanel) {
    DiagnosticsPage page;
    StubPresent stub;
    page.setPresentProvider(&stub);

    page.applyLiveDiagnostics(MakeRecordingSnapshot());

    auto* label = page.findChild<QLabel*>(QStringLiteral("liveCapturePresentMode"));
    ASSERT_NE(label, nullptr) << "liveCapturePresentMode label not found";
    EXPECT_TRUE(label->text().contains(QStringLiteral("Independent flip")))
        << "Expected 'Independent flip' in label, got: " << label->text().toStdString();
}

TEST_F(DiagnosticsPageTest, PresentProvider_NullProviderKeepsUnavailable) {
    DiagnosticsPage page;
    // No provider — present_mode_availability stays Unavailable → em-dash.
    page.applyLiveDiagnostics(MakeRecordingSnapshot());

    auto* label = page.findChild<QLabel*>(QStringLiteral("liveCapturePresentMode"));
    ASSERT_NE(label, nullptr) << "liveCapturePresentMode label not found";
    EXPECT_FALSE(label->text().contains(QStringLiteral("Independent flip")));
}

TEST_F(DiagnosticsPageTest, CaptureCardsLiveDuringRecording) {
    DiagnosticsPage page;
    LoadData(page);
    auto s = MakeRecordingSnapshot();
    s.video_encoder.average_ms = 2.1;
    s.mux.process_average_ms = 0.5;
    s.mux.process_availability = recorder_core::MetricAvailability::Available;
    s.disk.average_write_ms = 0.8;
    page.applyLiveDiagnostics(s);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    // Healthy recording → no card alarmed (no Over).
    for (int i = 0; i < flow->stepCount(); ++i)
        EXPECT_NE(flow->card(i)->status(), PipelineStepCard::Status::Over) << i;
    // Capture card shows the fps number; Encoder shows ms.
    EXPECT_TRUE(flow->card(0)->secondaryNumber().contains(QStringLiteral("fps")));
    EXPECT_TRUE(flow->card(3)->secondaryNumber().contains(QStringLiteral("ms")));
    EXPECT_EQ(flow->card(3)->resourceTag(), QStringLiteral("GPU (NVENC)"));
}

TEST_F(DiagnosticsPageTest, CaptureCardBottleneckShownAsOver) {
    DiagnosticsPage page;
    LoadData(page);
    auto s = MakeRecordingSnapshot();
    s.video_encoder.average_ms = 22.0; // way over the 16.7 ms budget
    s.video_encoder.backlog = 6;
    page.applyLiveDiagnostics(s);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    EXPECT_EQ(flow->card(3)->status(), PipelineStepCard::Status::Over);
}

TEST_F(DiagnosticsPageTest, LiveCardStatusSurvivesStaticRefresh) {
    // Regression for the static-refresh clobber: a live recording snapshot drives the
    // Encoder card to Over; a subsequent setDiagnosticData() static refresh (mirroring
    // MainWindow's QTimer::singleShot(0) → refreshDiagnosticsData() bootstrap) must NOT
    // reset the card back to its static readiness status. The live state must survive.
    DiagnosticsPage page;
    LoadData(page);
    auto s = MakeRecordingSnapshot();
    s.video_encoder.average_ms = 22.0; // way over the 16.7 ms budget → Over
    page.applyLiveDiagnostics(s);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    ASSERT_EQ(flow->card(3)->status(), PipelineStepCard::Status::Over);

    // Static refresh while the recording is still live (last_live_snapshot_ valid).
    LoadData(page);

    EXPECT_EQ(flow->card(3)->status(), PipelineStepCard::Status::Over)
        << "Static refresh must not clobber live Over status during an active recording";
}

TEST_F(DiagnosticsPageTest, EncoderHealthyBacklogDoesNotTriggerOver) {
    // Regression for FIX 1: NVENC steady-state backlog (naturally ≥2) must NOT be
    // treated as a shedding proxy. A healthy encoder with backlog=4 and latency well
    // under budget must NOT show Over on the Encoder card.
    DiagnosticsPage page;
    LoadData(page);
    auto s = MakeRecordingSnapshot();
    s.video_encoder.backlog = 4;      // steady-state NVENC pipeline depth
    s.video_encoder.average_ms = 2.0; // well under the 16.7 ms budget at 60 fps
    s.video_encoder.frames_encoded = 700;
    page.applyLiveDiagnostics(s);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    EXPECT_NE(flow->card(3)->status(), PipelineStepCard::Status::Over)
        << "Healthy encoder with steady-state NVENC backlog must not show Over (FIX 1 regression)";
}

TEST_F(DiagnosticsPageTest, EncoderSecondaryNumberDashWhenNoSampleYet) {
    // Regression for FIX 2: when frames_encoded > 0 but average_ms == 0.0 (no latency
    // sample accumulated yet), the Encoder secondary number must show the dash (kDash),
    // not "0.0 ms". enc.available is true (stage is live), but the VALUE is not ready.
    DiagnosticsPage page;
    LoadData(page);
    auto s = MakeRecordingSnapshot();
    s.video_encoder.frames_encoded = 10; // stage is alive
    s.video_encoder.average_ms = 0.0;    // no latency sample yet
    page.applyLiveDiagnostics(s);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    EXPECT_EQ(flow->card(3)->secondaryNumber(), kDash)
        << "Encoder secondary number must be dash when average_ms == 0.0 (FIX 2 honesty)";
}

TEST_F(DiagnosticsPageTest, MuxNumberDashWhenUnavailable) {
    DiagnosticsPage page;
    LoadData(page);
    auto s = MakeRecordingSnapshot();
    s.mux.process_availability = recorder_core::MetricAvailability::Unavailable;
    page.applyLiveDiagnostics(s);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    EXPECT_EQ(flow->card(4)->secondaryNumber(), kDash);
}

TEST_F(DiagnosticsPageTest, IdleAfterRecordingRestoresStaticCards) {
    DiagnosticsPage page;
    LoadData(page);
    page.applyLiveDiagnostics(MakeRecordingSnapshot());

    recorder_core::RecordingDiagnosticsSnapshot idle;
    idle.lifecycle = recorder_core::DiagnosticsLifecycle::Idle;
    idle.valid = false;
    page.applyLiveDiagnostics(idle);

    auto* flow = page.findChild<PipelineFlow*>(QStringLiteral("pipelineFlow"));
    ASSERT_NE(flow, nullptr);
    // Idle restores static readiness: probe-less cards Planned, probed cards Ok.
    EXPECT_EQ(flow->card(0)->status(), PipelineStepCard::Status::Planned);
    EXPECT_EQ(flow->card(3)->status(), PipelineStepCard::Status::Ok);
}

// ---- Tier split: verdict counts only blockers + Tier-2, never Tier-3 tips -----

TEST_F(DiagnosticsPageTest, TipsOnlyConfigStaysReadyWithTipChipAndNoCards) {
    // MKV + H.264 + AAC on the AV1-capable baseline fires exactly one Tier-3
    // optimisation (rec.profile.codec "a better codec is available") and nothing else
    // (the combo is Recommended, so no validation-warning card either). Per
    // suite-diag2.jsx scenario 'ready': tips present AND chip = READY/success — a
    // tip must never turn the verdict amber or claim issue cards that don't exist.
    DiagnosticsPage page;
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    OutputSettingsModel output;
    output.container = capability::Container::Matroska;
    output.video_codec = capability::VideoCodec::H264Nvenc; // AV1 available → tip fires
    output.audio_codec = capability::AudioCodec::AacMf;
    VideoSettingsModel video;
    capability::AudioUiState audio;
    page.setDiagnosticData(caps, output, video, audio, "MKV H264 AAC", "Start/Stop: Alt+F9", "", true);

    QPushButton* run = FindButton(page, QStringLiteral("Run Check"));
    ASSERT_NE(run, nullptr);
    run->click();

    // Verdict: READY (tips-only is the calm ready state, not ATTENTION).
    bool pill_ready = false;
    for (auto* lbl : page.findChildren<QLabel*>())
        if (lbl->property("labelRole").toString() == QLatin1String("profileStatusBadge") &&
            lbl->text() == QStringLiteral("READY"))
            pill_ready = true;
    EXPECT_TRUE(pill_ready) << "Tips-only config must stay READY (Tier-3 never alarms the verdict)";

    // Readiness tile: pass tone, not amber.
    EXPECT_EQ(TileTone(page, QStringLiteral("readinessTileReadiness")), QStringLiteral("pass"))
        << "Readiness tile must not be amber for optimisation tips";

    // The tip is bundled in the chip…
    auto* chip = page.findChild<QWidget*>(QStringLiteral("diagTipChip"));
    ASSERT_NE(chip, nullptr);
    EXPECT_FALSE(chip->isHidden()) << "Tip chip must be visible when tips exist";

    // …and there are NO issue cards ("See the cards below" over a void is the bug).
    int visible_cards = 0;
    for (auto* frame : page.findChildren<QFrame*>())
        if (frame->property("panelRole").toString() == QLatin1String("issueCard"))
            ++visible_cards;
    EXPECT_EQ(visible_cards, 0) << "Tier-3 tips must not render issue cards";
}

TEST_F(DiagnosticsPageTest, BlockerShowsCardInSimpleViewAndBlocksVerdict) {
    // MP4 + FLAC is a hard container incompatibility → Tier-1 blocker. The card must
    // be visible in the Simple (default, expert=false) view and the verdict must gate.
    DiagnosticsPage page;
    EXPECT_FALSE(page.isExpertModeEnabled());
    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    OutputSettingsModel output;
    output.container = capability::Container::Mp4;
    output.audio_codec = capability::AudioCodec::Flac; // rec.009 Blocker
    output.video_codec = capability::VideoCodec::Av1Nvenc;
    VideoSettingsModel video;
    capability::AudioUiState audio;
    page.setDiagnosticData(caps, output, video, audio, "MP4 AV1 FLAC", "Start/Stop: Alt+F9", "", true);

    QPushButton* run = FindButton(page, QStringLiteral("Run Check"));
    ASSERT_NE(run, nullptr);
    run->click();

    bool pill_blocked = false;
    for (auto* lbl : page.findChildren<QLabel*>())
        if (lbl->property("labelRole").toString() == QLatin1String("profileStatusBadge") &&
            lbl->text().contains(QStringLiteral("CAN'T RECORD")))
            pill_blocked = true;
    EXPECT_TRUE(pill_blocked) << "A blocker must gate the verdict (CAN'T RECORD)";

    bool blocker_card_visible = false;
    for (auto* frame : page.findChildren<QFrame*>())
        if (frame->property("panelRole").toString() == QLatin1String("issueCard") &&
            frame->property("issueTone").toString() == QLatin1String("blocker"))
            blocker_card_visible = true;
    EXPECT_TRUE(blocker_card_visible) << "Tier-1 blocker card must be visible in the Simple view";
}

TEST_F(DiagnosticsPageTest, MeasuredJudderNoticeIsCardNotTip) {
    // rec.001 fires only on MEASURED present jitter (Tier-2). It must render as an
    // issue card, never disappear into the Tier-3 tip chip.
    DiagnosticsPage page;
    LoadData(page);

    auto s = MakeRecordingSnapshot();
    s.capture.present_cadence_availability = recorder_core::MetricAvailability::Available;
    s.capture.source_present_jitter_ms = 11.4; // over the 8 ms judder limit
    s.video_encoder.cfr = true;
    page.applyLiveDiagnostics(s);

    QPushButton* run = FindButton(page, QStringLiteral("Run Check"));
    ASSERT_NE(run, nullptr);
    run->click();

    // The measured problem earns a Tier-2 card…
    bool judder_card = false;
    for (auto* frame : page.findChildren<QFrame*>()) {
        if (frame->property("panelRole").toString() != QLatin1String("issueCard"))
            continue;
        for (auto* lbl : frame->findChildren<QLabel*>())
            if (lbl->text().contains(QStringLiteral("judder"), Qt::CaseInsensitive))
                judder_card = true;
    }
    EXPECT_TRUE(judder_card) << "Measured judder (rec.001) must render as an issue card";

    // …and does NOT hide in the tip chip.
    auto* chip = page.findChild<QWidget*>(QStringLiteral("diagTipChip"));
    ASSERT_NE(chip, nullptr);
    bool judder_in_chip = false;
    for (auto* lbl : chip->findChildren<QLabel*>())
        if (lbl->text().contains(QStringLiteral("rec.001")))
            judder_in_chip = true;
    EXPECT_FALSE(judder_in_chip) << "A measured Tier-2 problem must never be bundled as a tip";

    // Verdict reflects the measured problem (amber), which is correct for Tier-2.
    bool pill_attention = false;
    for (auto* lbl : page.findChildren<QLabel*>())
        if (lbl->property("labelRole").toString() == QLatin1String("profileStatusBadge") &&
            lbl->text().contains(QStringLiteral("ATTENTION")))
            pill_attention = true;
    EXPECT_TRUE(pill_attention) << "A Tier-2 measured problem must turn the verdict amber";
}

} // namespace
} // namespace exosnap
