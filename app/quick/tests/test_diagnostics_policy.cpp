// Policy extracted out of the Qt Widgets DiagnosticsPage and MainWindow. These
// tests pin the behaviour that used to be buried inside widget code: the verdict
// rail, the tier -> card/tip split, the readiness tile text, the self-test rollup,
// the FixAction dispatch table, and — most importantly — the frame-drop delta
// accounting the live pipeline cards depend on.

#include "diagnostics/DiagnosticsController.h"
#include "diagnostics/DiagnosticsProbe.h"
#include "diagnostics/FixActionDispatcher.h"
#include "diagnostics/SessionLedger.h"
#include "viewmodels/RecordViewModel.h"
#include "visual_tests/DiagnosticsLiveScenario.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

using namespace exosnap;
using namespace exosnap::diagnostics;

namespace {

DiagnosticResult MakeResult(std::string id, DiagnosticTier tier, DiagnosticSeverity severity, std::string title) {
    DiagnosticResult result;
    result.id = std::move(id);
    result.tier = tier;
    result.severity = severity;
    result.title = std::move(title);
    result.summary = "summary";
    return result;
}

DiagnosticChecklist MakeChecklist(std::vector<DiagnosticResult> results) {
    DiagnosticChecklist checklist;
    checklist.results = std::move(results);
    return checklist;
}

DiagnosticResult LedgerProblem(const std::string& id, double value, double budget) {
    DiagnosticResult result =
        MakeResult(id, DiagnosticTier::MeasuredProblem, DiagnosticSeverity::Notice, id + " title");
    result.measured_value = value;
    result.budget_value = budget;
    result.value_unit = "ms";
    return result;
}

// A ledger holding `ids` as entered entries, with everything past `active_count`
// gone quiet again. Two observations per id: the entry debounce is the ledger's
// own contract and is pinned in its own test file.
SessionLedger LedgerWith(const std::vector<std::string>& ids, size_t active_count) {
    SessionLedger ledger;
    ledger.Reset(1);
    std::vector<DiagnosticResult> all;
    for (const std::string& id : ids)
        all.push_back(LedgerProblem(id, 9.0, 8.0));
    ledger.Observe(all, 1.0);
    ledger.Observe(all, 1.5);
    ledger.Observe({all.begin(), all.begin() + static_cast<ptrdiff_t>(active_count)}, 2.0);
    return ledger;
}

const LedgerEntry* FindEntry(const std::vector<LedgerEntry>& entries, const std::string& id) {
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const LedgerEntry& e) { return e.id == id; });
    return it == entries.end() ? nullptr : &*it;
}

} // namespace

// ── Verdict ─────────────────────────────────────────────────────────────────────

TEST(DiagnosticsVerdict, NoDataIsNeutralNotReady) {
    const Verdict verdict = ComputeVerdict({}, 0, false);
    EXPECT_EQ(verdict.state, VerdictState::Neutral);
    EXPECT_EQ(verdict.headline, "Not checked yet");
    EXPECT_EQ(verdict.blockers, 0);
}

TEST(DiagnosticsVerdict, CleanChecklistIsReadyAndNamesCapabilityPasses) {
    const Verdict verdict = ComputeVerdict({}, 12, true);
    EXPECT_EQ(verdict.state, VerdictState::Ready);
    EXPECT_EQ(verdict.headline, "Ready to record");
    EXPECT_NE(verdict.subline.find("12 capability checks passed"), std::string::npos);
}

TEST(DiagnosticsVerdict, BlockerWins) {
    const DiagnosticChecklist checklist = MakeChecklist({
        MakeResult("rec.001", DiagnosticTier::Blocker, DiagnosticSeverity::Blocker, "blocked"),
        MakeResult("rec.002", DiagnosticTier::MeasuredProblem, DiagnosticSeverity::Notice, "measured"),
    });
    const Verdict verdict = ComputeVerdict(checklist, 5, true);
    EXPECT_EQ(verdict.state, VerdictState::Blocked);
    EXPECT_EQ(verdict.blockers, 1);
    EXPECT_EQ(verdict.notices, 1);
    EXPECT_EQ(verdict.headline, "1 thing to fix before recording");
}

// The honesty rail: Tier-3 optimisations and Tier-4 facts must never colour the
// verdict, because they bundle into the quiet tip chip / Expert panel instead.
TEST(DiagnosticsVerdict, OptimisationsAndFactsDoNotWarn) {
    const DiagnosticChecklist checklist = MakeChecklist({
        MakeResult("rec.tip", DiagnosticTier::Optimisation, DiagnosticSeverity::Notice, "tip"),
        MakeResult("fact.elevation", DiagnosticTier::Fact, DiagnosticSeverity::Pass, "fact"),
    });
    const Verdict verdict = ComputeVerdict(checklist, 3, true);
    EXPECT_EQ(verdict.state, VerdictState::Ready);
    EXPECT_EQ(verdict.notices, 0);
}

TEST(DiagnosticsVerdict, MeasuredProblemsWarnWithPlural) {
    const DiagnosticChecklist checklist = MakeChecklist({
        MakeResult("rec.a", DiagnosticTier::MeasuredProblem, DiagnosticSeverity::Notice, "a"),
        MakeResult("rec.b", DiagnosticTier::MeasuredProblem, DiagnosticSeverity::Notice, "b"),
    });
    const Verdict verdict = ComputeVerdict(checklist, 4, true);
    EXPECT_EQ(verdict.state, VerdictState::Warn);
    EXPECT_NE(verdict.headline.find("2 things could hurt the result"), std::string::npos);
}

// ── Top issues ──────────────────────────────────────────────────────────────────

TEST(DiagnosticsTopIssues, ProfileInvalidityLeadsAndCarriesActionHint) {
    capability::ResolveResult validation;
    validation.succeeded = false;
    validation.invalidity.push_back({"video_codec", "AV1 is unavailable"});

    const TopIssues issues = BuildTopIssues(validation, {}, true, "None configured");
    ASSERT_EQ(issues.cards.size(), 1U);
    EXPECT_EQ(issues.cards[0].tone, IssueTone::Blocker);
    EXPECT_EQ(issues.cards[0].title, "Video codec is not supported");
    EXPECT_FALSE(issues.cards[0].why.empty());
}

TEST(DiagnosticsTopIssues, OptimisationsBundleIntoTipsNotCards) {
    DiagnosticResult tip = MakeResult("rec.tip", DiagnosticTier::Optimisation, DiagnosticSeverity::Notice, "Tip title");
    FixAction fix;
    fix.id = "fix.container.mkv";
    fix.label = "Switch to MKV";
    fix.safety = FixAction::Safety::Assisted;
    fix.changes_summary = "Container becomes MKV";
    tip.fix_action = fix;

    const TopIssues issues = BuildTopIssues({}, MakeChecklist({tip}), true, "None configured");
    EXPECT_TRUE(issues.cards.empty());
    ASSERT_EQ(issues.tips.size(), 1U);
    EXPECT_EQ(issues.tips[0].summary, "Tip title");
    EXPECT_TRUE(issues.tips[0].has_fix);
    EXPECT_EQ(issues.tips[0].fix_safety, FixSafetyKind::Assisted);
}

TEST(DiagnosticsTopIssues, CardListIsCappedAtSix) {
    std::vector<DiagnosticResult> results;
    for (int i = 0; i < 10; ++i) {
        results.push_back(
            MakeResult("rec.00" + std::to_string(i), DiagnosticTier::Blocker, DiagnosticSeverity::Blocker, "b"));
    }
    const TopIssues issues = BuildTopIssues({}, MakeChecklist(results), true, "None configured");
    EXPECT_EQ(static_cast<int>(issues.cards.size()), kMaxIssueCards);
}

TEST(DiagnosticsTopIssues, HotkeyNoticeOnlyWhenBindingsExist) {
    EXPECT_TRUE(BuildTopIssues({}, {}, false, "None configured").cards.empty());

    const TopIssues issues = BuildTopIssues({}, {}, false, "Ctrl+Shift+R");
    ASSERT_EQ(issues.cards.size(), 1U);
    EXPECT_EQ(issues.cards[0].title, "Global hotkeys are not active");
}

TEST(DiagnosticsTopIssues, PresentAndDpcChecksCarryTheElevationBadge) {
    const TopIssues issues = BuildTopIssues(
        {},
        MakeChecklist(
            {MakeResult("rec.present.flips", DiagnosticTier::MeasuredProblem, DiagnosticSeverity::Notice, "p"),
             MakeResult("rec.dpc.latency", DiagnosticTier::MeasuredProblem, DiagnosticSeverity::Notice, "d"),
             MakeResult("rec.005", DiagnosticTier::MeasuredProblem, DiagnosticSeverity::Notice, "o")}),
        true, "None configured");
    ASSERT_EQ(issues.cards.size(), 3U);
    EXPECT_TRUE(issues.cards[0].needs_elevation);
    EXPECT_TRUE(issues.cards[1].needs_elevation);
    EXPECT_FALSE(issues.cards[2].needs_elevation);
}

TEST(DiagnosticsTopIssues, EvidencePresenceIgnoresWhitespaceOnlyFields) {
    DiagnosticResult result = MakeResult("rec.x", DiagnosticTier::Blocker, DiagnosticSeverity::Blocker, "t");
    result.recommendation = "   ";
    result.current_value = "";
    result.detail = "";
    const TopIssues issues = BuildTopIssues({}, MakeChecklist({result}), true, "None configured");
    ASSERT_EQ(issues.cards.size(), 1U);
    EXPECT_FALSE(issues.cards[0].has_evidence());
}

// ── Readiness tiles ─────────────────────────────────────────────────────────────

// Four, always, and in one order: a tile row that reflows between recordings
// cannot be read at a glance, and a fifth tile leaves a ragged half-row.
TEST(DiagnosticsTiles, TheReadinessRowIsAlwaysFourTilesInOneOrder) {
    ReadinessTileInputs inputs;
    inputs.data_ready = true;
    const auto tiles = BuildReadinessTiles(inputs);
    ASSERT_EQ(tiles.size(), 4U);
    EXPECT_EQ(tiles[0].key, "encoder");
    EXPECT_EQ(tiles[1].key, "disk");
    EXPECT_EQ(tiles[2].key, "display");
    EXPECT_EQ(tiles[3].key, "audio");
}

// A queried zero is a FULL drive, not an unknown one: it must read "0.0 GB".
TEST(DiagnosticsTiles, ZeroFreeBytesIsAMeasurementNotAnUnknown) {
    ReadinessTileInputs inputs;
    inputs.data_ready = true;
    inputs.free_bytes = 0;
    inputs.total_bytes = 100ULL * 1024 * 1024 * 1024;
    inputs.output_drive_label = "C:";
    const auto tiles = BuildReadinessTiles(inputs);
    EXPECT_EQ(tiles[1].value, "0.0 GB");
    EXPECT_TRUE(tiles[1].has_usage_bar);
    EXPECT_EQ(tiles[1].usage_percent, 100);
}

TEST(DiagnosticsTiles, UnqueryableVolumeShowsDashAndNoBar) {
    ReadinessTileInputs inputs;
    inputs.data_ready = true;
    const auto tiles = BuildReadinessTiles(inputs);
    EXPECT_EQ(tiles[1].value, "\xe2\x80\x94");
    EXPECT_FALSE(tiles[1].has_usage_bar);
}

TEST(DiagnosticsTiles, CaptureTargetFallsBackToTheConfiguredKind) {
    ReadinessTileInputs inputs;
    inputs.data_ready = true;
    inputs.target_is_window = true;
    EXPECT_NE(BuildReadinessTiles(inputs)[2].sub.find("application window"), std::string::npos);

    inputs.target_selected = true;
    inputs.target_description = "Firefox";
    EXPECT_NE(BuildReadinessTiles(inputs)[2].sub.find("Firefox"), std::string::npos);
}

TEST(DiagnosticsTiles, AudioSublineKeepsTheRateAndUnitTogether) {
    ReadinessTileInputs inputs;
    inputs.data_ready = true;
    inputs.audio_sources = 2;
    inputs.audio_sample_rate = 48000;
    inputs.audio_channels = 2;
    const std::string sub = BuildReadinessTiles(inputs)[3].sub;
    EXPECT_NE(sub.find("2 sources"), std::string::npos);
    // Non-breaking space between number and unit, so word-wrap cannot split "48 kHz".
    EXPECT_NE(sub.find("48\xc2\xa0kHz"), std::string::npos);
    EXPECT_NE(sub.find("Stereo"), std::string::npos);
}

// ── Helpers ─────────────────────────────────────────────────────────────────────

TEST(DiagnosticsHelpers, HumanBytesPicksPrecisionByMagnitude) {
    EXPECT_EQ(HumanBytes(0), "0.0 GB");
    EXPECT_EQ(HumanBytes(2ULL * 1024 * 1024 * 1024), "2.0 GB");
    EXPECT_EQ(HumanBytes(50ULL * 1024 * 1024 * 1024), "50 GB");
    EXPECT_EQ(HumanBytes(2048ULL * 1024 * 1024 * 1024), "2.0 TB");
}

TEST(DiagnosticsHelpers, StripBackendSuffixLeavesBareCodecNames) {
    EXPECT_EQ(StripBackendSuffix("AV1 (NVENC)"), "AV1");
    EXPECT_EQ(StripBackendSuffix("Opus"), "Opus");
}

TEST(DiagnosticsHelpers, DriveLabelPrefersTheVolumeRoot) {
    EXPECT_EQ(DriveLabelForPath(std::filesystem::path("C:/Users/User/Videos")), "C:");
    EXPECT_EQ(DriveLabelForPath(std::filesystem::path("relative/path")), "relative/path");
}

// ── Self-test ───────────────────────────────────────────────────────────────────

TEST(DiagnosticsSelfTest, AllPassIsPass) {
    DiagnosticChecklist checklist;
    checklist.results.push_back(MakeResult("st.capture", DiagnosticTier::Fact, DiagnosticSeverity::Pass, "Capture"));
    const SelfTestReport report = BuildSelfTestReport(checklist);
    EXPECT_EQ(report.state, SelfTestState::Pass);
    ASSERT_EQ(report.rows.size(), 1U);
    EXPECT_FALSE(report.rows[0].not_run);
}

TEST(DiagnosticsSelfTest, FailureIsWarnNotNotRun) {
    DiagnosticChecklist checklist;
    checklist.has_notice = true;
    DiagnosticResult failed = MakeResult("st.encoder", DiagnosticTier::Fact, DiagnosticSeverity::Notice, "Encoder");
    failed.detail = "NVENC encoder library not found";
    checklist.results.push_back(failed);
    const SelfTestReport report = BuildSelfTestReport(checklist);
    EXPECT_EQ(report.state, SelfTestState::Warn);
    EXPECT_FALSE(report.rows[0].not_run);
}

// The runner's "not executed in this build" sentinel is mapped ONCE, here, onto a
// typed field. Nothing downstream of this extraction sniffs the detail string.
TEST(DiagnosticsSelfTest, CompiledOutCheckBecomesATypedNotRunRow) {
    DiagnosticChecklist checklist;
    checklist.has_notice = true;
    DiagnosticResult skipped = MakeResult("st.dpc", DiagnosticTier::Fact, DiagnosticSeverity::Notice, "DPC");
    skipped.detail = "DPC probe not executed in this build";
    checklist.results.push_back(skipped);
    const SelfTestReport report = BuildSelfTestReport(checklist);
    EXPECT_EQ(report.state, SelfTestState::NotRun);
    ASSERT_EQ(report.rows.size(), 1U);
    EXPECT_TRUE(report.rows[0].not_run);
    EXPECT_EQ(report.rows[0].status_text, "Not run");
}

// ── Environment / config tables ─────────────────────────────────────────────────

TEST(DiagnosticsEnvironment, EmptyFactsStillReportTheMeasuredElevationState) {
    const auto standard = BuildEnvironmentRows({}, false);
    ASSERT_EQ(standard.size(), 1U);
    EXPECT_EQ(standard[0].label, "Elevation");
    EXPECT_NE(standard[0].value.find("Standard"), std::string::npos);

    const auto elevated = BuildEnvironmentRows({}, true);
    EXPECT_NE(elevated[0].value.find("Elevated"), std::string::npos);
}

TEST(DiagnosticsEnvironment, FactsMapTitleAndSummary) {
    DiagnosticResult fact = MakeResult("fact.audio", DiagnosticTier::Fact, DiagnosticSeverity::Pass, "Audio format");
    fact.summary = "48 kHz stereo";
    const auto rows = BuildEnvironmentRows({fact}, false);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].label, "Audio format");
    EXPECT_EQ(rows[0].value, "48 kHz stereo");
}

TEST(DiagnosticsEnvironment, ConfigRowsMirrorTheSummaryEntries) {
    ConfigSummary summary;
    summary.entries.push_back({"Container", "MKV"});
    summary.entries.push_back({"Video codec", "AV1"});
    const auto rows = BuildConfigRows(summary);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[1].label, "Video codec");
    EXPECT_EQ(rows[1].value, "AV1");
}

// ── Pipeline: the frame-drop delta accounting ───────────────────────────────────

namespace {

exosnap::engine::RecordingDiagnosticsSnapshot MakeLiveSnapshot(uint64_t generation, uint64_t backpressure_drops) {
    exosnap::engine::RecordingDiagnosticsSnapshot snapshot;
    snapshot.valid = true;
    snapshot.lifecycle = exosnap::engine::DiagnosticsLifecycle::Recording;
    snapshot.session_generation = generation;
    snapshot.capture.target_fps = 60.0;
    snapshot.capture.actual_fps = 60.0;
    snapshot.capture.frames_dropped_backpressure = backpressure_drops;
    return snapshot;
}

} // namespace

TEST(PipelineCards, FirstSampleReportsNoRecentDrops) {
    PipelineCardBuilder builder;
    const auto stages = builder.BuildLive(MakeLiveSnapshot(1, 40));
    EXPECT_EQ(stages.size(), 6U);
    EXPECT_EQ(builder.lastRecentDropsForTesting(), 0U);
}

// The counter is session-cumulative; the capture stage needs drops SINCE THE LAST
// SAMPLE. Losing this baseline reports every drop since session start as "recent".
TEST(PipelineCards, RecentDropsAreADeltaNotACumulativeTotal) {
    PipelineCardBuilder builder;
    (void)builder.BuildLive(MakeLiveSnapshot(1, 40));
    (void)builder.BuildLive(MakeLiveSnapshot(1, 43));
    EXPECT_EQ(builder.lastRecentDropsForTesting(), 3U);
    (void)builder.BuildLive(MakeLiveSnapshot(1, 43));
    EXPECT_EQ(builder.lastRecentDropsForTesting(), 0U);
}

TEST(PipelineCards, NewSessionGenerationRebaselinesTheDelta) {
    PipelineCardBuilder builder;
    (void)builder.BuildLive(MakeLiveSnapshot(1, 900));
    (void)builder.BuildLive(MakeLiveSnapshot(2, 5));
    // Without the generation reset this would underflow or report 0 by luck; the
    // rebaseline makes the new session's first sample a clean zero.
    EXPECT_EQ(builder.lastRecentDropsForTesting(), 0U);
    (void)builder.BuildLive(MakeLiveSnapshot(2, 9));
    EXPECT_EQ(builder.lastRecentDropsForTesting(), 4U);
}

TEST(PipelineCards, ResetClearsTheBaselineWhenRecordingStops) {
    PipelineCardBuilder builder;
    (void)builder.BuildLive(MakeLiveSnapshot(1, 100));
    builder.Reset();
    (void)builder.BuildLive(MakeLiveSnapshot(1, 100));
    EXPECT_EQ(builder.lastRecentDropsForTesting(), 0U);
}

TEST(PipelineCards, StaticStagesNameTheProbeThatIsMissing) {
    const auto unprobed = PipelineCardBuilder::BuildStatic(false, false, false, false);
    ASSERT_EQ(unprobed.size(), 6U);
    EXPECT_EQ(unprobed[3].status, StageStatus::Planned);
    EXPECT_EQ(unprobed[5].tip, "Run a check to probe the output path.");

    const auto probed = PipelineCardBuilder::BuildStatic(true, true, true, false);
    EXPECT_EQ(probed[3].status, StageStatus::Ok);
    EXPECT_EQ(probed[5].status, StageStatus::Unavailable);
    EXPECT_EQ(probed[5].tip, "Output path is not writable.");
}

// ── Throttle ────────────────────────────────────────────────────────────────────

TEST(RefreshThrottleTest, FirstTickPassesThenRateLimits) {
    RefreshThrottle throttle(std::chrono::milliseconds(500));
    const auto t0 = RefreshThrottle::Clock::now();
    EXPECT_TRUE(throttle.Allow(t0));
    EXPECT_FALSE(throttle.Allow(t0 + std::chrono::milliseconds(200)));
    EXPECT_TRUE(throttle.Allow(t0 + std::chrono::milliseconds(600)));
}

TEST(RefreshThrottleTest, ResetAllowsTheNextTickImmediately) {
    RefreshThrottle throttle(std::chrono::milliseconds(500));
    const auto t0 = RefreshThrottle::Clock::now();
    EXPECT_TRUE(throttle.Allow(t0));
    throttle.Reset();
    EXPECT_TRUE(throttle.Allow(t0 + std::chrono::milliseconds(1)));
}

// ── FixAction dispatch ──────────────────────────────────────────────────────────

TEST(FixActionDispatch, UnknownIdIsReportedNotSilentlyApplied) {
    capability::CapabilitySet caps;
    OutputSettingsModel output;
    VideoSettingsModel video;
    const FixResult result = ApplyAutoFix("fix.does.not.exist", caps, output, video);
    EXPECT_FALSE(result.handled());
    EXPECT_EQ(result.outcome, FixOutcome::Unknown);
}

TEST(FixActionDispatch, FramePacingFixMutatesOnlyVideoSettings) {
    capability::CapabilitySet caps;
    OutputSettingsModel output;
    VideoSettingsModel video;
    video.frame_pacing = exosnap::engine::FramePacingMode::Newest;
    const FixResult result = ApplyAutoFix("fix.frame_pacing.smooth", caps, output, video);
    EXPECT_EQ(result.outcome, FixOutcome::SettingsChanged);
    EXPECT_EQ(video.frame_pacing, exosnap::engine::FramePacingMode::Smooth);
}

TEST(FixActionDispatch, ColorRangeFixSwitchesToLimited) {
    capability::CapabilitySet caps;
    OutputSettingsModel output;
    VideoSettingsModel video;
    output.color_range = capability::ColorRange::Full;
    EXPECT_EQ(ApplyAutoFix("fix.color.range", caps, output, video).outcome, FixOutcome::SettingsChanged);
    EXPECT_EQ(output.color_range, capability::ColorRange::Limited);
}

// The engine keys the HDR fix id off whichever codec is actually selectable, so the
// handler must apply exactly that codec rather than a blind AV1.
TEST(FixActionDispatch, HdrFixAppliesTheCodecTheActionNamed) {
    capability::CapabilitySet caps;
    OutputSettingsModel output;
    VideoSettingsModel video;
    output.video_codec = capability::VideoCodec::H264;
    (void)ApplyAutoFix("fix.hdr.codec.hevc", caps, output, video);
    EXPECT_EQ(output.video_codec, capability::VideoCodec::Hevc);

    output.video_codec = capability::VideoCodec::H264;
    (void)ApplyAutoFix("fix.hdr.codec.av1", caps, output, video);
    EXPECT_EQ(output.video_codec, capability::VideoCodec::Av1);
}

TEST(FixActionDispatch, VideoCodecDefaultFallsBackToH264WhenNothingIsSupported) {
    capability::CapabilitySet caps;
    OutputSettingsModel output;
    VideoSettingsModel video;
    output.video_codec = capability::VideoCodec::Av1;
    (void)ApplyAutoFix("fix.codec.video.default", caps, output, video);
    EXPECT_EQ(output.video_codec, capability::VideoCodec::H264);
}

TEST(FixActionDispatch, CaptureRetargetIsAHostActionNotASettingsChange) {
    capability::CapabilitySet caps;
    OutputSettingsModel output;
    VideoSettingsModel video;
    const FixResult result = ApplyAutoFix("fix.capture.monitor_instead", caps, output, video);
    EXPECT_EQ(result.outcome, FixOutcome::RetargetToHostingMonitor);
}

TEST(FixActionDispatch, AssistedFixesResolveToASettingsSection) {
    EXPECT_EQ(SettingsSectionFor(ResolveAssistedFix("fix.output.change_folder").outcome), "settings/output");
    EXPECT_EQ(SettingsSectionFor(ResolveAssistedFix("fix.output.fat32_folder").outcome), "settings/output");
    EXPECT_EQ(SettingsSectionFor(ResolveAssistedFix("fix.container.mkv").outcome), "settings/format");
    EXPECT_FALSE(ResolveAssistedFix("").handled());
}

// ── Recording verdict: the ledger, not the current checklist ────────────────────

TEST(DiagnosticsRecordingVerdict, EmptyLedgerIsReadyWhileRecording) {
    SessionLedger ledger;
    ledger.Reset(1);
    const Verdict verdict = ComputeRecordingVerdict({}, ledger);
    EXPECT_EQ(verdict.state, VerdictState::Ready);
    EXPECT_TRUE(verdict.recording);
    EXPECT_EQ(verdict.headline, "Recording \xe2\x80\x94 no problems measured");
}

TEST(DiagnosticsRecordingVerdict, LedgerEntriesWarnAndCountActive) {
    const SessionLedger ledger = LedgerWith({"rec.001", "rec.disk.writestall"}, /*active_count=*/1);
    const Verdict verdict = ComputeRecordingVerdict({}, ledger, /*now_s=*/2.0);
    EXPECT_EQ(verdict.state, VerdictState::Warn);
    EXPECT_EQ(verdict.notices, 2);
    EXPECT_EQ(verdict.headline, "Recording \xe2\x80\x94 2 problems observed, 1 active");
    // The subline names what is happening NOW, which is the one thing the count
    // in the headline cannot say.
    EXPECT_NE(verdict.subline.find("rec.001 title"), std::string::npos);
}

TEST(DiagnosticsRecordingVerdict, AQuietLedgerSaysSoAndNamesTheLastProblem) {
    const SessionLedger ledger = LedgerWith({"rec.001"}, /*active_count=*/0);
    const Verdict verdict = ComputeRecordingVerdict({}, ledger, /*now_s=*/12.0);
    EXPECT_EQ(verdict.state, VerdictState::Warn);
    EXPECT_EQ(verdict.headline, "Recording \xe2\x80\x94 1 problem observed, quiet now");
    EXPECT_NE(verdict.subline.find("rec.001 title"), std::string::npos);
    EXPECT_NE(verdict.subline.find("last seen"), std::string::npos);
}

// The headline is restricted to counts so the band does not rewrite itself twice
// a second. A subline counting up in whole seconds under it would do exactly that
// for the whole quiet stretch, so the figure is quantised.
TEST(DiagnosticsRecordingVerdict, TheLastSeenFigureIsQuantisedSoTheSublineHoldsStill) {
    const SessionLedger ledger = LedgerWith({"rec.001"}, /*active_count=*/0);
    // The entry goes quiet at 2.0 s.
    EXPECT_NE(ComputeRecordingVerdict({}, ledger, 6.0).subline.find("just now"), std::string::npos);
    EXPECT_EQ(ComputeRecordingVerdict({}, ledger, 6.0).subline, ComputeRecordingVerdict({}, ledger, 9.4).subline);
    EXPECT_NE(ComputeRecordingVerdict({}, ledger, 27.0).subline.find("20 s ago"), std::string::npos);
    EXPECT_EQ(ComputeRecordingVerdict({}, ledger, 27.0).subline, ComputeRecordingVerdict({}, ledger, 29.9).subline);
    EXPECT_NE(ComputeRecordingVerdict({}, ledger, 200.0).subline.find("3 min ago"), std::string::npos);
}

TEST(DiagnosticsRecordingVerdict, ABlockerInTheLiveChecklistWins) {
    const SessionLedger ledger = LedgerWith({"rec.001", "rec.disk.writestall"}, /*active_count=*/2);
    const DiagnosticChecklist live = MakeChecklist({
        MakeResult("rec.capture.adapter_mismatch", DiagnosticTier::Blocker, DiagnosticSeverity::Blocker, "blocked"),
    });
    const Verdict verdict = ComputeRecordingVerdict(live, ledger, /*now_s=*/2.0);
    EXPECT_EQ(verdict.state, VerdictState::Blocked);
    EXPECT_EQ(verdict.blockers, 1);
}

TEST(DiagnosticsRecordingVerdict, HeadlineOnlyChangesWithTheCounts) {
    SessionLedger mild;
    mild.Reset(1);
    mild.Observe({LedgerProblem("rec.001", 9.0, 8.0)}, 1.0);
    mild.Observe({LedgerProblem("rec.001", 9.0, 8.0)}, 1.5);

    SessionLedger severe;
    severe.Reset(1);
    severe.Observe({LedgerProblem("rec.001", 44.0, 8.0)}, 1.0);
    severe.Observe({LedgerProblem("rec.001", 91.0, 8.0)}, 1.5);

    // The band must not flicker on every measurement. Only the counts move it.
    EXPECT_EQ(ComputeRecordingVerdict({}, mild, 1.5).headline, ComputeRecordingVerdict({}, severe, 1.5).headline);
}

namespace {

DiagnosticsController::Config MinimalConfig() {
    DiagnosticsController::Config config;
    config.caps.video_codecs[capability::VideoCodec::Av1] = {capability::SupportLevel::Available, ""};
    config.caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    config.hotkeys_ok = true;
    return config;
}

exosnap::engine::RecordingDiagnosticsSnapshot JudderSnapshot(double elapsed_s) {
    exosnap::engine::RecordingDiagnosticsSnapshot snapshot =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("judder"));
    // The fixture sits at 7.9 ms, just under the 8 ms threshold: the judder check
    // has to actually fire for the ledger to have anything to record.
    snapshot.capture.source_present_jitter_ms = 9.0;
    snapshot.elapsed_seconds = elapsed_s;
    return snapshot;
}

} // namespace

TEST(DiagnosticsController, EvaluateFeedsTheLedgerOnlyWhileRecording) {
    DiagnosticsController controller;
    controller.SetConfig(MinimalConfig());

    controller.SetLiveSnapshot(JudderSnapshot(10.0));
    DiagnosticsSnapshot first = controller.Evaluate();
    // One evaluation is a spike, and a spike is not a problem.
    EXPECT_EQ(FindEntry(first.ledger, "rec.001"), nullptr);

    controller.SetLiveSnapshot(JudderSnapshot(10.5));
    DiagnosticsSnapshot second = controller.Evaluate();
    const LedgerEntry* entry = FindEntry(second.ledger, "rec.001");
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->active);
    EXPECT_GE(second.ledger_active, 1);
    EXPECT_DOUBLE_EQ(entry->first_seen_s, 10.0);

    exosnap::engine::RecordingDiagnosticsSnapshot done = JudderSnapshot(11.0);
    done.lifecycle = exosnap::engine::DiagnosticsLifecycle::Completed;
    controller.SetLiveSnapshot(done);
    DiagnosticsSnapshot after = controller.Evaluate();
    const LedgerEntry* frozen = FindEntry(after.ledger, "rec.001");
    ASSERT_NE(frozen, nullptr) << "the ledger is frozen at Stop, not emptied";
    EXPECT_FALSE(frozen->active);
    EXPECT_DOUBLE_EQ(frozen->occurrences.back().end_s, 11.0);
    EXPECT_EQ(after.ledger_active, 0);
}

// The entry rule counts consecutive MEASUREMENTS. Evaluate() is reached from
// every settings change, display change and probe result as well as from the live
// rail, so re-evaluating one snapshot must not let a single spike enter.
TEST(DiagnosticsController, ReEvaluatingOneSnapshotDoesNotSatisfyTheEntryRule) {
    DiagnosticsController controller;
    controller.SetConfig(MinimalConfig());

    controller.SetLiveSnapshot(JudderSnapshot(10.0));
    for (int pass = 0; pass < 5; ++pass) {
        const DiagnosticsSnapshot out = controller.Evaluate();
        EXPECT_EQ(FindEntry(out.ledger, "rec.001"), nullptr) << "pass " << pass;
    }

    // A second, genuinely different measurement is what earns the entry.
    controller.SetLiveSnapshot(JudderSnapshot(10.5));
    EXPECT_NE(FindEntry(controller.Evaluate().ledger, "rec.001"), nullptr);
}

TEST(DiagnosticsController, ANewSessionGenerationResetsTheLedger) {
    DiagnosticsController controller;
    controller.SetConfig(MinimalConfig());
    controller.SetLiveSnapshot(JudderSnapshot(10.0));
    const DiagnosticsSnapshot spike = controller.Evaluate();
    EXPECT_EQ(FindEntry(spike.ledger, "rec.001"), nullptr);
    controller.SetLiveSnapshot(JudderSnapshot(10.5));
    const DiagnosticsSnapshot entered = controller.Evaluate();
    ASSERT_NE(FindEntry(entered.ledger, "rec.001"), nullptr);

    exosnap::engine::RecordingDiagnosticsSnapshot next = JudderSnapshot(0.5);
    next.session_generation = 2;
    controller.SetLiveSnapshot(next);
    const DiagnosticsSnapshot fresh = controller.Evaluate();
    EXPECT_EQ(controller.ledger().generation(), 2u);
    // The new session starts from nothing, debounce included.
    EXPECT_EQ(FindEntry(fresh.ledger, "rec.001"), nullptr);
}

// ── Last session ────────────────────────────────────────────────────────────────

namespace {

UiRecordingResult MakeResultFor(uint32_t frame_rate_num) {
    UiRecordingResult result;
    result.succeeded = true;
    result.output_path = L"D:/Videos/ExoSnap_2026-09-05.mkv";
    result.output_file_bytes = 1610612736;
    result.elapsed_seconds = 184.0;
    result.media_duration_seconds = 183.5;
    result.frame_rate_num = frame_rate_num;
    result.frame_rate_den = 1;
    result.container = exosnap::engine::Container::Matroska;
    result.video_codec = exosnap::engine::VideoCodec::Av1;
    result.audio_codec = exosnap::engine::AudioCodec::Opus;
    return result;
}

std::vector<LedgerEntry> FrozenLedger() {
    SessionLedger ledger = LedgerWith({"rec.001"}, /*active_count=*/0);
    ledger.Freeze(10.0);
    return ledger.entries();
}

} // namespace

TEST(DiagnosticsLastSession, FourFactsInAFixedOrderWithTheFileNameOnly) {
    const exosnap::engine::RecordingDiagnosticsSnapshot snapshot =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("post"));
    const LastSession session = BuildLastSession(MakeResultFor(60), snapshot, {});

    EXPECT_TRUE(session.valid);
    // The name, never the path: the card is a support artefact as much as a UI.
    EXPECT_EQ(session.file_name, "ExoSnap_2026-09-05.mkv");
    // The timeline spans the session clock the marks are placed on; the media is
    // shorter by the tail between the last encoded frame and Stop.
    EXPECT_DOUBLE_EQ(session.duration_s, 184.0);
    EXPECT_DOUBLE_EQ(session.media_duration_s, 183.5);
    ASSERT_EQ(session.facts.size(), 4u);
    EXPECT_EQ(session.facts[0].key, "dropped");
    EXPECT_EQ(session.facts[1].key, "achieved");
    EXPECT_EQ(session.facts[2].key, "drift");
    EXPECT_EQ(session.facts[3].key, "file");
    EXPECT_EQ(session.problems, 0);
    EXPECT_TRUE(session.marks.empty());
}

TEST(DiagnosticsLastSession, FramesDroppedIsTheOneFactAChecksOwnsOutright) {
    exosnap::engine::RecordingDiagnosticsSnapshot clean = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("post"));
    EXPECT_EQ(BuildLastSession(MakeResultFor(60), clean, {}).facts[0].tone, ValueTone::Ok);

    exosnap::engine::RecordingDiagnosticsSnapshot lossy = clean;
    lossy.capture.frames_dropped_processing_failure = 12;
    const LastSession session = BuildLastSession(MakeResultFor(60), lossy, {});
    // A dropped frame is missing from the file. That is not a matter of degree.
    EXPECT_EQ(session.facts[0].tone, ValueTone::Critical);
    EXPECT_EQ(session.facts[0].value, "12");
}

TEST(DiagnosticsLastSession, TheFrozenLedgerBecomesTheProblemCountAndTheTimelineMarks) {
    const exosnap::engine::RecordingDiagnosticsSnapshot snapshot =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("post"));
    const std::vector<LedgerEntry> frozen = FrozenLedger();
    ASSERT_EQ(frozen.size(), 1u);
    ASSERT_EQ(frozen.front().occurrences.size(), 1u);

    const LastSession session = BuildLastSession(MakeResultFor(60), snapshot, frozen);
    EXPECT_EQ(session.problems, 1);
    EXPECT_EQ(session.ledger, frozen);
    ASSERT_EQ(session.marks.size(), 1u);
    EXPECT_EQ(session.marks.front().id, "rec.001");
    EXPECT_EQ(session.marks.front().tone, "warn");
    EXPECT_DOUBLE_EQ(session.marks.front().start_s, 1.0);
    EXPECT_DOUBLE_EQ(session.marks.front().end_s, 2.0);
}

// The engine keeps no timestamped drop history, so a drop can only be counted in
// the Frames dropped fact. A mark spanning the whole recording would read as "the
// entire run was bad" for a defect that lasted a frame.
TEST(DiagnosticsLastSession, FrameDropsAreCountedAndNeverMarkedOnTheTimeline) {
    exosnap::engine::RecordingDiagnosticsSnapshot lossy = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("post"));
    lossy.capture.frames_dropped_backpressure = 41;
    const LastSession session = BuildLastSession(MakeResultFor(60), lossy, FrozenLedger());
    EXPECT_EQ(session.facts[0].value, "41");
    ASSERT_EQ(session.marks.size(), 1u);
    EXPECT_EQ(session.marks.front().id, "rec.001");
    EXPECT_EQ(session.marks.front().tone, "warn");
}

TEST(DiagnosticsLastSession, AnUnmeasuredSessionStatesWhatItCannotSayInsteadOfZero) {
    const LastSession session = BuildLastSession(MakeResultFor(60), {}, {});
    EXPECT_EQ(session.facts[0].value, "\xe2\x80\x94");
    EXPECT_EQ(session.facts[0].tone, ValueTone::Neutral);
    EXPECT_EQ(session.facts[1].value, "\xe2\x80\x94");
    EXPECT_EQ(session.facts[2].value, "Unavailable");
    // The file itself is a fact of the result, not of the snapshot.
    EXPECT_EQ(session.facts[3].value, "Valid");
    EXPECT_EQ(session.facts[3].tone, ValueTone::Ok);
}

TEST(DiagnosticsController, TheLastSessionIsHeldUntilTheNextRecording) {
    DiagnosticsController controller;
    EXPECT_FALSE(controller.lastSession().valid);
    controller.SetLastSession(BuildLastSession(MakeResultFor(60), {}, FrozenLedger()));
    EXPECT_TRUE(controller.lastSession().valid);
    EXPECT_EQ(controller.lastSession().problems, 1);
}
