#include "DiagnosticsController.h"

#include "DiagnosticsPresentation.h"

#include <capability/support_level.h>
#include <exosnap/engine/pipeline_health.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace exosnap::diagnostics {
namespace {

constexpr const char* kDash = "\xe2\x80\x94";   // em dash
constexpr const char* kMiddot = "\xc2\xb7";     // middle dot
constexpr const char* kNarrowNbsp = "\xc2\xa0"; // non-breaking space
constexpr const char* kRightArrow = "\xe2\x86\x92";

// The SelfTestRunner sentinel for a check that was compiled out. Deliberately the
// ONLY place this string appears on the presentation side: everything downstream
// reads SelfTestRow::not_run instead.
constexpr std::string_view kNotExecutedSentinel = "not executed in this build";

std::string Number(double value, int precision) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

std::string Number(uint64_t value) {
    return std::to_string(value);
}

IssueTone ToneOf(DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::Pass:
        return IssueTone::Pass;
    case DiagnosticSeverity::Notice:
        return IssueTone::Notice;
    case DiagnosticSeverity::Blocker:
        return IssueTone::Blocker;
    }
    return IssueTone::Pass;
}

// Appends one card unless the calm cap has already been reached.
void PushCard(std::vector<IssueCard>& cards, IssueCard card) {
    if (static_cast<int>(cards.size()) >= kMaxIssueCards)
        return;
    cards.push_back(std::move(card));
}

IssueCard CardFromResult(const DiagnosticResult& result) {
    IssueCard card;
    card.id = result.id;
    card.tone = ToneOf(result.severity);
    card.title = result.title;
    card.summary = result.summary;
    card.why = result.recommendation;
    card.measured = result.current_value;
    card.log_excerpt = result.detail;
    card.needs_elevation = NeedsElevation(result.id);
    if (result.fix_action.has_value()) {
        const FixAction& fix = *result.fix_action;
        card.has_fix = true;
        card.fix_id = fix.id;
        card.fix_label = fix.label;
        card.fix_changes_summary = fix.changes_summary;
        card.fix_safety = FixKindOf(fix);
    }
    return card;
}

bool BlankOrWhitespace(const std::string& text) noexcept {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

StageStatus StatusOf(exosnap::engine::StageHealth health) noexcept {
    switch (health) {
    case exosnap::engine::StageHealth::Healthy:
        return StageStatus::Ok;
    case exosnap::engine::StageHealth::Busy:
        return StageStatus::Hotspot;
    case exosnap::engine::StageHealth::Bottleneck:
        return StageStatus::Over;
    }
    return StageStatus::Ok;
}

} // namespace

// ── Enum keys ───────────────────────────────────────────────────────────────────

std::string_view VerdictStateKey(VerdictState state) noexcept {
    switch (state) {
    case VerdictState::Neutral:
        return "neutral";
    case VerdictState::Checking:
        return "checking";
    case VerdictState::Ready:
        return "ready";
    case VerdictState::Warn:
        return "warn";
    case VerdictState::Blocked:
        return "blocked";
    }
    return "neutral";
}

std::string_view IssueToneKey(IssueTone tone) noexcept {
    switch (tone) {
    case IssueTone::Pass:
        return "pass";
    case IssueTone::Notice:
        return "notice";
    case IssueTone::Blocker:
        return "blocker";
    }
    return "pass";
}

std::string_view TileToneKey(TileTone tone) noexcept {
    switch (tone) {
    case TileTone::Neutral:
        return "neutral";
    case TileTone::Notice:
        return "notice";
    case TileTone::Blocker:
        return "blocker";
    }
    return "neutral";
}

std::string_view SelfTestStateLabel(SelfTestState state) noexcept {
    switch (state) {
    case SelfTestState::NotRun:
        return "Not run";
    case SelfTestState::Pass:
        return "PASS";
    case SelfTestState::Warn:
        return "WARN";
    }
    return "Not run";
}

std::string_view StageStatusKey(StageStatus status) noexcept {
    switch (status) {
    case StageStatus::Planned:
        return "planned";
    case StageStatus::Ok:
        return "ok";
    case StageStatus::Hotspot:
        return "hotspot";
    case StageStatus::Over:
        return "over";
    case StageStatus::Unavailable:
        return "unavailable";
    }
    return "planned";
}

FixSafetyKind FixKindOf(const FixAction& fix) noexcept {
    switch (fix.safety) {
    case FixAction::Safety::Auto:
        return FixSafetyKind::Auto;
    case FixAction::Safety::Assisted:
        return FixSafetyKind::Assisted;
    case FixAction::Safety::External:
        return FixSafetyKind::External;
    }
    return FixSafetyKind::Assisted;
}

bool IssueCard::has_evidence() const noexcept {
    return !BlankOrWhitespace(measured) || !BlankOrWhitespace(why) || !BlankOrWhitespace(log_excerpt);
}

// ── Pure helpers ────────────────────────────────────────────────────────────────

std::string HumanBytes(uint64_t bytes) {
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1024.0)
        return Number(gb / 1024.0, 1) + " TB";
    if (gb >= 10.0)
        return Number(gb, 0) + " GB";
    return Number(gb, 1) + " GB";
}

namespace {

// The engine's health verdict, rendered. Not a re-classification: this maps one
// enumerator onto one tone and nothing else decides a colour anywhere below.
TileTone ToneOfHealth(exosnap::engine::PipelineHealth health) noexcept {
    switch (health) {
    case exosnap::engine::PipelineHealth::Critical:
        return TileTone::Blocker;
    case exosnap::engine::PipelineHealth::Warning:
        return TileTone::Notice;
    case exosnap::engine::PipelineHealth::Good:
    case exosnap::engine::PipelineHealth::Idle:
    case exosnap::engine::PipelineHealth::Unavailable:
        return TileTone::Neutral;
    }
    return TileTone::Neutral;
}

// A per-tile tone follows the engine's ATTRIBUTION: a tile turns amber only when
// the engine named its stage as the bottleneck AND said the pipeline is unwell.
// Without the second half every tile would light up the moment a bottleneck was
// merely identified in a healthy pipeline.
TileTone ToneOfStage(const exosnap::engine::RecordingDiagnosticsSnapshot& s,
                     std::initializer_list<exosnap::engine::PipelineBottleneck> stages) noexcept {
    if (s.health != exosnap::engine::PipelineHealth::Warning && s.health != exosnap::engine::PipelineHealth::Critical)
        return TileTone::Neutral;
    for (const exosnap::engine::PipelineBottleneck stage : stages) {
        if (s.bottleneck == stage)
            return ToneOfHealth(s.health);
    }
    return TileTone::Neutral;
}

std::string BottleneckLabel(exosnap::engine::PipelineBottleneck bottleneck) {
    switch (bottleneck) {
    case exosnap::engine::PipelineBottleneck::None:
        return "No sustained bottleneck";
    case exosnap::engine::PipelineBottleneck::Capture:
        return "Capture";
    case exosnap::engine::PipelineBottleneck::Compositor:
        return "Compositor";
    case exosnap::engine::PipelineBottleneck::VideoEncoder:
        return "Video encoder";
    case exosnap::engine::PipelineBottleneck::Audio:
        return "Audio";
    case exosnap::engine::PipelineBottleneck::Muxer:
        return "Muxer";
    case exosnap::engine::PipelineBottleneck::Disk:
        return "Disk";
    case exosnap::engine::PipelineBottleneck::Unknown:
        return "Not enough evidence yet";
    }
    return "Not enough evidence yet";
}

std::string HealthLabel(exosnap::engine::PipelineHealth health) {
    switch (health) {
    case exosnap::engine::PipelineHealth::Good:
        return "Good";
    case exosnap::engine::PipelineHealth::Warning:
        return "Warning";
    case exosnap::engine::PipelineHealth::Critical:
        return "Critical";
    case exosnap::engine::PipelineHealth::Idle:
        return "Idle";
    case exosnap::engine::PipelineHealth::Unavailable:
        return "Unavailable";
    }
    return "Unavailable";
}

std::string CodecName(exosnap::engine::VideoCodec codec) {
    switch (codec) {
    case exosnap::engine::VideoCodec::Av1:
        return "AV1";
    case exosnap::engine::VideoCodec::Hevc:
        return "HEVC";
    case exosnap::engine::VideoCodec::H264:
        return "H.264";
    }
    return "AV1";
}

std::string PresetName(exosnap::engine::NvencPreset preset) {
    switch (preset) {
    case exosnap::engine::NvencPreset::P1:
        return "P1";
    case exosnap::engine::NvencPreset::P2:
        return "P2";
    case exosnap::engine::NvencPreset::P3:
        return "P3";
    case exosnap::engine::NvencPreset::P4:
        return "P4";
    case exosnap::engine::NvencPreset::P5:
        return "P5";
    case exosnap::engine::NvencPreset::P6:
        return "P6";
    case exosnap::engine::NvencPreset::P7:
        return "P7";
    }
    return "P4";
}

std::string PresentModeLabel(exosnap::engine::PresentMode mode) {
    switch (mode) {
    case exosnap::engine::PresentMode::Composed:
        return "Composed";
    case exosnap::engine::PresentMode::IndependentFlip:
        return "Independent flip";
    case exosnap::engine::PresentMode::ExclusiveFullscreen:
        return "Exclusive fullscreen";
    case exosnap::engine::PresentMode::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

// Coarse on purpose. A disk-fill estimate is a projection from the current
// sustained throughput, and quoting it to the minute would claim a precision the
// measurement does not have.
std::string CoarseDuration(double seconds) {
    if (seconds >= 6.0 * 3600.0)
        return std::string("> 6") + kNarrowNbsp + "h";
    if (seconds >= 3600.0)
        return Number(seconds / 3600.0, 1) + kNarrowNbsp + "h";
    if (seconds >= 60.0)
        return Number(seconds / 60.0, 0) + kNarrowNbsp + "min";
    return Number(seconds, 0) + kNarrowNbsp + "s";
}

std::string Join(const std::string& left, const std::string& right) {
    if (left.empty())
        return right;
    if (right.empty())
        return left;
    return left + " " + kMiddot + " " + right;
}

LiveTile PipelineHealthTile(const exosnap::engine::RecordingDiagnosticsSnapshot& s) {
    LiveTile tile;
    tile.key = "pipelineHealth";
    tile.title = "Pipeline health";
    tile.value = HealthLabel(s.health);
    tile.sub = BottleneckLabel(s.bottleneck);
    // The engine's own sentence when it has one; the drop count otherwise. Never
    // a sentence written here about a problem the engine did not report.
    tile.detail = s.bottleneck_reason.empty() ? "Problem drops " + Number(s.capture.frames_dropped_problem())
                                              : s.bottleneck_reason;
    tile.tone = ToneOfHealth(s.health);
    return tile;
}

LiveTile FramePacingTile(const exosnap::engine::RecordingDiagnosticsSnapshot& s) {
    LiveTile tile;
    tile.key = "framePacing";
    tile.title = "Frame pacing";
    tile.value = Number(s.capture.actual_fps, 2) + " fps";
    tile.sub = "Target " + Number(s.capture.target_fps, 0) + " fps";
    if (s.capture.present_cadence_availability == exosnap::engine::MetricAvailability::Available)
        tile.sub = Join(tile.sub, "jitter " + Number(s.capture.source_present_jitter_ms, 1) + " ms");

    if (s.capture.present_mode_availability == exosnap::engine::MetricAvailability::Available) {
        tile.detail = Join(PresentModeLabel(s.capture.source_present_mode),
                           s.capture.source_tearing ? std::string("tearing active") : std::string("no tearing"));
    } else {
        // Named, not blank: "unavailable" without a cause reads as a defect, and
        // the cause is an opt-in the user controls.
        tile.detail = "Present diagnostics unavailable (elevation + opt-in)";
    }
    tile.tone =
        ToneOfStage(s, {exosnap::engine::PipelineBottleneck::Capture, exosnap::engine::PipelineBottleneck::Compositor});
    return tile;
}

LiveTile EncoderTile(const exosnap::engine::RecordingDiagnosticsSnapshot& s) {
    LiveTile tile;
    tile.key = "encoder";
    tile.title = "Encoder";
    // What is ACTUALLY running, from the encoder's initialization record. Falls
    // back to the codec the diagnostics stream reports when no encoder has been
    // configured -- never to the configured preset, which is a request.
    if (s.encoder_init.valid) {
        tile.value = CodecName(s.encoder_init.codec) + " " + kMiddot + " " + PresetName(s.encoder_init.preset);
        if (s.encoder_init.rc_mode == exosnap::engine::RateControlMode::ConstantQuality)
            tile.value += std::string(" ") + kMiddot + " CQ " + Number(static_cast<uint64_t>(s.encoder_init.cq));
    } else {
        tile.value = CodecName(s.video_encoder.codec);
    }

    if (s.video_encoder.frames_encoded > 0) {
        tile.sub = "p99 " + Number(s.video_encoder.p99_ms, 1) + " ms";
        if (s.video_timing.budget_ms > 0.0)
            tile.sub = Join(tile.sub, "budget " + Number(s.video_timing.budget_ms, 2) + " ms");
    } else {
        tile.sub = "No frame encoded yet";
    }
    tile.detail = "Backlog " + Number(s.video_encoder.backlog);
    tile.tone = ToneOfStage(s, {exosnap::engine::PipelineBottleneck::VideoEncoder});
    return tile;
}

LiveTile AudioSyncTile(const exosnap::engine::RecordingDiagnosticsSnapshot& s) {
    LiveTile tile;
    tile.key = "audioSync";
    tile.title = "Audio sync";
    if (!s.audio.active) {
        tile.value = "No audio";
        tile.sub = "This recording has no audio track";
        tile.detail.clear();
        return tile;
    }

    const bool drift_faulted = s.av_drift_availability == exosnap::engine::MetricAvailability::Faulted;
    if (s.av_drift_availability == exosnap::engine::MetricAvailability::Available) {
        const std::string sign = s.av_drift_ms >= 0.0 ? "+" : "";
        tile.value = sign + Number(s.av_drift_ms, 1) + " ms";
    } else if (drift_faulted) {
        // Sampled and known-wrong. "Unavailable" would send the reader off to
        // wait for a value that already arrived, and a number would be a sync
        // claim the measurement cannot carry.
        tile.value = "Not measurable";
    } else {
        // A multi-source merge mixes several device clocks and does not report.
        // Zero here would claim perfect sync on a recording nobody measured.
        tile.value = "Unavailable";
    }

    tile.sub = Number(static_cast<uint64_t>(s.audio.sample_rate / 1000)) + " kHz";
    tile.sub = Join(tile.sub, s.audio.channels == 1 ? std::string("Mono") : std::string("Stereo"));

    std::string detail;
    if (s.peak_av_drift_availability == exosnap::engine::MetricAvailability::Available)
        detail = "peak " + Number(s.peak_av_drift_ms, 1) + " ms";
    if (s.clock_slaving_active)
        detail = Join(detail, "correcting " + Number(s.clock_slaving_ppm, 0) + " ppm");
    if (s.audio.source_degraded) {
        detail = Join(detail, Number(static_cast<uint64_t>(s.audio.degraded_sources)) + " source(s) silent");
    }
    if (drift_faulted) {
        // States what happened to the measurement, not what it might mean for
        // the file: the recorded timeline is held by the wall clock either way,
        // and an alarm about an unmeasured defect would be an invention.
        detail = Join(detail, "audio device stopped reporting its clock");
    }
    tile.detail = detail;

    tile.tone = ToneOfStage(s, {exosnap::engine::PipelineBottleneck::Audio});
    // A degraded source is a MEASURED problem in its own right (ADR 0046) and is
    // reported as one even while the engine still calls the pipeline healthy --
    // the recording keeps running, which is why it never escalates past Notice.
    if (s.audio.source_degraded && tile.tone == TileTone::Neutral)
        tile.tone = TileTone::Notice;
    return tile;
}

LiveTile StorageTile(const exosnap::engine::RecordingDiagnosticsSnapshot& s) {
    LiveTile tile;
    tile.key = "storage";
    tile.title = "Storage";
    tile.value = Number(s.disk.throughput_mib_s, 0) + " MiB/s";
    tile.sub = "Write failures " + Number(s.disk.write_failures);
    // Negative means the estimate could not be made (unknown throughput or no
    // free-space reading), which is a different answer from "no time left".
    tile.detail = s.disk_fill_eta_seconds >= 0.0 ? "Est. remaining " + CoarseDuration(s.disk_fill_eta_seconds)
                                                 : "Remaining time unavailable";
    tile.tone = ToneOfStage(s, {exosnap::engine::PipelineBottleneck::Disk, exosnap::engine::PipelineBottleneck::Muxer});
    if (s.disk.write_failures > 0 && tile.tone == TileTone::Neutral)
        tile.tone = TileTone::Notice;
    return tile;
}

} // namespace

std::vector<LiveTile> BuildLiveTiles(const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot) {
    const bool live = snapshot.lifecycle == exosnap::engine::DiagnosticsLifecycle::Recording ||
                      snapshot.lifecycle == exosnap::engine::DiagnosticsLifecycle::Paused;
    if (!snapshot.valid || !live)
        return {};
    return {PipelineHealthTile(snapshot), FramePacingTile(snapshot), EncoderTile(snapshot), AudioSyncTile(snapshot),
            StorageTile(snapshot)};
}

bool NeedsElevation(std::string_view id) noexcept {
    return id.rfind("rec.present.", 0) == 0 || id.rfind("rec.dpc.", 0) == 0;
}

std::string StripBackendSuffix(std::string codec) {
    if (const size_t paren = codec.find(" ("); paren != std::string::npos && paren > 0)
        codec.resize(paren);
    return codec;
}

int CountAvailableCapabilities(const CapabilitySummary& summary) noexcept {
    int passes = 0;
    for (const auto& entry : summary.entries) {
        if (entry.available)
            ++passes;
    }
    return passes;
}

Verdict ComputeVerdict(const DiagnosticChecklist& recommendations, int cap_passes, bool data_ready) {
    Verdict verdict;
    verdict.cap_passes = cap_passes;

    if (!data_ready) {
        verdict.state = VerdictState::Neutral;
        verdict.headline = "Not checked yet";
        verdict.subline = "Run a check to see whether this machine is set up to record well.";
        return verdict;
    }

    // The honesty rail: only Tier-1 blockers and Tier-2 measured problems steer the
    // verdict. Tier-3 optimisations bundle into the quiet tip chip and Tier-4 facts
    // are neutral environment data, so neither may colour the headline.
    for (const auto& result : recommendations.results) {
        switch (result.tier) {
        case DiagnosticTier::Blocker:
            ++verdict.blockers;
            break;
        case DiagnosticTier::MeasuredProblem:
            ++verdict.notices;
            break;
        case DiagnosticTier::Optimisation:
        case DiagnosticTier::Fact:
            break;
        }
    }

    if (verdict.blockers > 0) {
        verdict.state = VerdictState::Blocked;
        verdict.headline = verdict.blockers == 1 ? std::string("1 thing to fix before recording")
                                                 : std::to_string(verdict.blockers) + " things to fix before recording";
        verdict.subline = std::to_string(verdict.blockers) + (verdict.blockers == 1 ? " blocker" : " blockers") +
                          " must be resolved before recording. See the cards below.";
        return verdict;
    }

    if (verdict.notices > 0) {
        verdict.state = VerdictState::Warn;
        verdict.headline = verdict.notices == 1
                               ? std::string("Recording works ") + kDash + " 1 thing could hurt the result"
                               : std::string("Recording works ") + kDash + " " + std::to_string(verdict.notices) +
                                     " things could hurt the result";
        verdict.subline = "You can record, but " + std::to_string(verdict.notices) +
                          (verdict.notices == 1 ? " issue" : " issues") +
                          " could affect the result. See the cards below.";
        return verdict;
    }

    verdict.state = VerdictState::Ready;
    verdict.headline = "Ready to record";
    verdict.subline =
        std::string("Everything checks out ") + kDash + " " + std::to_string(cap_passes) + " capability checks passed.";
    return verdict;
}

TopIssues BuildTopIssues(const capability::ResolveResult& profile_validation,
                         const DiagnosticChecklist& recommendations, bool hotkeys_ok,
                         const std::string& hotkeys_summary) {
    TopIssues issues;

    // Tier-1 first: profile invalidity is a config-level blocker that precedes any
    // engine result, because an unsupported profile invalidates everything below it.
    for (const auto& invalid : profile_validation.invalidity) {
        IssueCard card;
        card.tone = IssueTone::Blocker;
        card.title = InvalidFieldDisplayName(invalid.field) + " is not supported";
        card.summary = invalid.message;
        card.why = InvalidFieldActionHint(invalid.field);
        PushCard(issues.cards, std::move(card));
    }

    const bool has_profile_invalidity = !profile_validation.invalidity.empty();
    const std::vector<DiagnosticResult> ordered = BuildTopIssueRecommendations(recommendations, has_profile_invalidity);

    for (const auto& result : ordered) {
        if (result.tier == DiagnosticTier::Blocker)
            PushCard(issues.cards, CardFromResult(result));
    }

    for (const auto& warning : profile_validation.warnings) {
        IssueCard card;
        card.tone = IssueTone::Notice;
        card.title = "Configuration needs validation";
        card.summary = warning.message;
        card.why = "Run a short recording to validate quality on this machine.";
        card.log_excerpt = "Code: " + warning.code;
        PushCard(issues.cards, std::move(card));
    }

    if (!hotkeys_ok && hotkeys_summary != "None configured") {
        IssueCard card;
        card.tone = IssueTone::Notice;
        card.title = "Global hotkeys are not active";
        card.summary = "Hotkeys are configured but not currently registered.";
        card.why = "Open the Hotkeys page and reapply the binding if shortcuts do not trigger.";
        card.log_excerpt = "If the app just launched, this can clear once startup completes.";
        PushCard(issues.cards, std::move(card));
    }

    // Tier-2 measured problems become cards; Tier-3 optimisations bundle into the
    // quiet tip chip. The tier is read straight from each result, never re-derived.
    for (const auto& result : ordered) {
        if (result.tier == DiagnosticTier::MeasuredProblem) {
            PushCard(issues.cards, CardFromResult(result));
        } else if (BundlesIntoTipChip(result.tier)) {
            TipEntry tip;
            tip.id = result.id;
            tip.summary = result.title;
            if (result.fix_action.has_value()) {
                const FixAction& fix = *result.fix_action;
                tip.has_fix = true;
                tip.fix_id = fix.id;
                tip.fix_label = fix.label;
                tip.changes = fix.changes_summary;
                tip.fix_safety = FixKindOf(fix);
            }
            issues.tips.push_back(std::move(tip));
        }
    }

    return issues;
}

std::vector<KeyValueRow> BuildEnvironmentRows(const std::vector<DiagnosticResult>& facts, bool elevated) {
    std::vector<KeyValueRow> rows;
    rows.reserve(facts.size());
    for (const auto& fact : facts)
        rows.push_back({fact.title, fact.summary});

    // Never leave Expert blank. GenerateEnvironmentFacts always emits fact.elevation
    // today, so this is a defensive fallback that still mirrors the measured state
    // rather than a fixed string.
    if (rows.empty()) {
        rows.push_back(
            {"Elevation", elevated ? std::string("Elevated ") + kDash + " PresentMon ETW present diagnostics available"
                                   : std::string("Standard ") + kDash + " DXGI / NVAPI baseline " + kMiddot +
                                         " present diagnostics need elevation"});
    }
    return rows;
}

std::vector<KeyValueRow> BuildConfigRows(const ConfigSummary& summary) {
    std::vector<KeyValueRow> rows;
    rows.reserve(summary.entries.size());
    for (const auto& entry : summary.entries)
        rows.push_back({entry.label, entry.value});
    return rows;
}

SelfTestReport BuildSelfTestReport(const DiagnosticChecklist& self_test) {
    SelfTestReport report;
    report.rows.reserve(self_test.results.size());

    bool all_not_executed = true;
    for (const auto& result : self_test.results) {
        if (result.severity != DiagnosticSeverity::Pass &&
            result.detail.find(kNotExecutedSentinel) == std::string::npos) {
            all_not_executed = false;
            break;
        }
    }

    if (self_test.worst_severity() == DiagnosticSeverity::Pass) {
        report.state = SelfTestState::Pass;
    } else if (all_not_executed) {
        report.state = SelfTestState::NotRun;
    } else if (self_test.has_notice) {
        report.state = SelfTestState::Warn;
    }

    for (const auto& result : self_test.results) {
        SelfTestRow row;
        row.not_run = result.severity != DiagnosticSeverity::Pass &&
                      result.detail.find(kNotExecutedSentinel) != std::string::npos;
        row.title = result.title;
        row.detail = result.detail;
        row.status_text = row.not_run ? "Not run" : result.summary;
        row.tone = row.not_run ? IssueTone::Pass : ToneOf(result.severity);
        report.rows.push_back(std::move(row));
    }

    return report;
}

// ── Readiness tiles ─────────────────────────────────────────────────────────────

std::vector<ReadinessTile> BuildReadinessTiles(const ReadinessTileInputs& in) {
    std::vector<ReadinessTile> tiles;
    tiles.reserve(7);

    // Tile 1 — Readiness.
    {
        ReadinessTile tile;
        tile.key = "readiness";
        tile.title = "Readiness";
        const int total = in.cap_passes + in.blockers + in.notices;
        if (in.blockers > 0) {
            tile.value = "Action needed";
            tile.sub = in.blockers == 1
                           ? std::string("1 blocker ") + kMiddot + " recording is blocked"
                           : std::to_string(in.blockers) + " blockers " + kMiddot + " recording is blocked";
            tile.tone = TileTone::Blocker;
        } else if (in.notices > 0) {
            tile.value = std::to_string(in.cap_passes) + " / " + std::to_string(total);
            tile.sub = in.notices == 1
                           ? std::string("checks pass ") + kMiddot + " 1 issue"
                           : std::string("checks pass ") + kMiddot + " " + std::to_string(in.notices) + " issues";
            tile.tone = TileTone::Notice;
        } else if (in.data_ready) {
            tile.value = std::to_string(in.cap_passes) + " / " + std::to_string(total);
            tile.sub = "checks passed";
            tile.show_ok_glyph = true;
        } else {
            tile.value = kDash;
            tile.sub = "run a check";
        }
        tiles.push_back(std::move(tile));
    }

    // Tile 2 — Encoder: the GPU carrying the encode, codec as the detail line.
    {
        ReadinessTile tile;
        tile.key = "encoder";
        tile.title = "Encoder";
        if (in.data_ready) {
            std::string gpu = in.gpu_adapter_name;
            while (!gpu.empty() && std::isspace(static_cast<unsigned char>(gpu.back())) != 0)
                gpu.pop_back();
            const std::string codec = StripBackendSuffix(VideoCodecDisplayName(in.video_codec));
            tile.value = gpu.empty() ? codec : gpu;
            tile.sub = gpu.empty() ? ContainerDisplayName(in.container) : codec + " " + kMiddot + " NVENC";
        } else {
            tile.value = kDash;
            tile.sub = "active encoder";
        }
        tiles.push_back(std::move(tile));
    }

    // Tile 3 — Disk. A queried zero is a FULL drive and must read "0.0 GB", not
    // blank; only an unqueryable volume shows the dash.
    {
        ReadinessTile tile;
        tile.key = "disk";
        tile.title = "Disk";
        if (in.data_ready && in.free_bytes.has_value()) {
            tile.value = HumanBytes(*in.free_bytes);
            if (in.total_bytes > 0) {
                const double used = 1.0 - static_cast<double>(*in.free_bytes) / static_cast<double>(in.total_bytes);
                tile.has_usage_bar = true;
                tile.usage_percent = std::clamp(static_cast<int>(used * 100.0 + 0.5), 0, 100);
            }
            tile.sub = in.output_drive_label.empty() ? std::string("free ") + kMiddot + " output drive"
                                                     : std::string("free ") + kMiddot + " " + in.output_drive_label;
        } else {
            tile.value = kDash;
            tile.sub = "output drive";
        }
        tiles.push_back(std::move(tile));
    }

    // Tile 4 — Display (an honest static fact about the primary screen).
    {
        ReadinessTile tile;
        tile.key = "display";
        tile.title = "Display";
        if (in.display_width > 0 && in.display_height > 0) {
            tile.value = std::to_string(in.display_width) + " \xc3\x97 " + std::to_string(in.display_height);
            tile.sub = std::to_string(in.display_refresh_hz) + " Hz " + kMiddot + " primary display";
        } else {
            tile.value = kDash;
            tile.sub = "display";
        }
        tiles.push_back(std::move(tile));
    }

    // Tile 5 — Audio. A plain capability readout, never coloured as a problem.
    {
        ReadinessTile tile;
        tile.key = "audio";
        tile.title = "Audio";
        if (in.data_ready) {
            const std::string codec = StripBackendSuffix(AudioCodecDisplayName(in.audio_codec));
            tile.value = codec.empty() ? std::string(kDash) : codec;
            if (in.audio_sources == 0) {
                tile.sub = std::string("no sources ") + kMiddot + " silent";
            } else {
                // Non-breaking space between the number and its unit: word-wrap must
                // never split "48 kHz" across two lines inside the tile subline.
                const std::string rate = Number(static_cast<double>(in.audio_sample_rate) / 1000.0, 1);
                std::string trimmed = rate;
                if (trimmed.size() > 2 && trimmed.compare(trimmed.size() - 2, 2, ".0") == 0)
                    trimmed.resize(trimmed.size() - 2);
                const std::string channels = in.audio_channels <= 1 ? "Mono" : "Stereo";
                tile.sub = std::to_string(in.audio_sources) + (in.audio_sources == 1 ? " source " : " sources ") +
                           kMiddot + " " + trimmed + kNarrowNbsp + "kHz " + kMiddot + " " + channels;
            }
        } else {
            tile.value = kDash;
            tile.sub = "audio sources";
        }
        tiles.push_back(std::move(tile));
    }

    // Tile 6 — Capture target. Prefers the concrete selection, then the configured
    // kind, so the tile still reads honestly before a target is picked.
    {
        ReadinessTile tile;
        tile.key = "target";
        tile.title = "Capture target";
        if (in.target_selected) {
            tile.value = in.target_is_window ? "Window" : "Screen";
            tile.sub = in.target_description;
            if (BlankOrWhitespace(tile.sub))
                tile.sub = in.target_is_window ? "application window" : "full display";
        } else if (in.data_ready) {
            tile.value = in.target_is_window ? "Window" : "Screen";
            tile.sub = in.target_is_window ? "application window" : "full display";
        } else {
            tile.value = kDash;
            tile.sub = "capture target";
        }
        tiles.push_back(std::move(tile));
    }

    // Tile 7 — Last session. Only earns a slot once a completed recording exists;
    // it is a calm signpost to the Edit overlay's Review step, never a metric.
    if (in.has_last_recording) {
        ReadinessTile tile;
        tile.key = "session";
        tile.title = "Last session";
        tile.value = "Recorded";
        tile.sub = std::string("report in Edit ") + kMiddot + " Review";
        tiles.push_back(std::move(tile));
    }

    return tiles;
}

// ── PipelineCardBuilder ─────────────────────────────────────────────────────────

void PipelineCardBuilder::Reset() noexcept {
    last_generation_ = 0;
    last_problem_drops_ = 0;
    last_recent_drops_ = 0;
    seeded_ = false;
}

uint32_t PipelineCardBuilder::lastRecentDropsForTesting() const noexcept {
    return last_recent_drops_;
}

std::vector<PipelineStage> PipelineCardBuilder::BuildStatic(bool data_ready, bool encoder_ok, bool muxer_ok,
                                                            bool disk_ok) {
    std::vector<PipelineStage> stages;
    stages.reserve(6);

    const auto planned = [&](const char* key, const char* title, const char* tip) {
        PipelineStage stage;
        stage.key = key;
        stage.title = title;
        stage.lane = kDash;
        stage.value = kDash;
        stage.tip = tip;
        stage.status = StageStatus::Planned;
        stages.push_back(std::move(stage));
    };

    planned("capture", "Source capture", "Live during recording.");
    planned("queue", "Frame queue", "Live during recording.");
    planned("compositor", "Compositor", "Live during recording.");

    if (!data_ready) {
        planned("encoder", "Encoder", "Run a check to probe the encoder.");
        planned("muxer", "Muxer", "Run a check to probe the muxer.");
        planned("disk", "Disk", "Run a check to probe the output path.");
        return stages;
    }

    const auto probed = [&](const char* key, const char* title, bool ok, const char* ok_tip, const char* bad_tip) {
        PipelineStage stage;
        stage.key = key;
        stage.title = title;
        stage.lane = kDash;
        stage.value = kDash;
        stage.tip = ok ? ok_tip : bad_tip;
        stage.status = ok ? StageStatus::Ok : StageStatus::Unavailable;
        stages.push_back(std::move(stage));
    };

    probed("encoder", "Encoder", encoder_ok, "Selected video encoder is available. Live encoder load is not measured.",
           "Selected video codec is not available on this system.");
    probed("muxer", "Muxer", muxer_ok, "Selected container muxer is available. Write throughput is not measured.",
           "Selected container is not available on this system.");
    probed("disk", "Disk", disk_ok, "Output path is writable. Live disk throughput is not measured.",
           "Output path is not writable.");
    return stages;
}

std::vector<PipelineStage> PipelineCardBuilder::BuildLive(const exosnap::engine::RecordingDiagnosticsSnapshot& s) {
    using exosnap::engine::MetricAvailability;
    using exosnap::engine::StageId;
    using exosnap::engine::StageSignals;

    const double budget_ms = (s.capture.target_fps > 0.0) ? 1000.0 / s.capture.target_fps : (1000.0 / 60.0);

    // Frame-drop DELTA accounting. frames_dropped_problem() is cumulative for the
    // session; the capture stage's health verdict needs drops since the previous
    // sample. The baseline resets whenever session_generation changes, because a new
    // recording restarts the counter from zero.
    const uint64_t problem_drops = s.capture.frames_dropped_problem();
    if (!seeded_ || s.session_generation != last_generation_) {
        seeded_ = true;
        last_generation_ = s.session_generation;
        last_problem_drops_ = problem_drops;
    }
    const uint32_t capture_recent_drops =
        (problem_drops > last_problem_drops_) ? static_cast<uint32_t>(problem_drops - last_problem_drops_) : 0;
    last_problem_drops_ = problem_drops;
    last_recent_drops_ = capture_recent_drops;

    constexpr uint32_t kQueueBusyDepth = 8;
    constexpr double kDiskBudgetMs = 8.0;

    StageSignals capture{};
    capture.id = StageId::SourceCapture;
    capture.available = s.capture.target_fps > 0.0 && s.capture.actual_fps > 0.0;
    capture.is_duration_stage = false;
    capture.can_bottleneck = true;
    capture.fps_ratio = (s.capture.target_fps > 0.0) ? s.capture.actual_fps / s.capture.target_fps : 1.0;
    capture.recent_drops = capture_recent_drops;

    StageSignals queue{};
    queue.id = StageId::FrameQueue;
    queue.available = true;
    queue.is_duration_stage = false;
    queue.can_bottleneck = false;
    queue.queue_depth = s.video_queue.current_depth;
    queue.queue_busy_threshold = kQueueBusyDepth;

    StageSignals comp{};
    comp.id = StageId::Compositor;
    comp.available = s.compositor.active;
    comp.is_duration_stage = true;
    comp.can_bottleneck = true;
    comp.avg_ms = s.compositor.average_ms;

    StageSignals enc{};
    enc.id = StageId::Encoder;
    enc.available = s.video_encoder.average_ms > 0.0 || s.video_encoder.frames_encoded > 0;
    enc.is_duration_stage = true;
    enc.can_bottleneck = true;
    enc.avg_ms = s.video_encoder.average_ms;

    StageSignals mux{};
    mux.id = StageId::Muxer;
    mux.available = s.mux.process_availability == MetricAvailability::Available;
    mux.is_duration_stage = true;
    mux.can_bottleneck = true;
    mux.avg_ms = s.mux.process_average_ms;

    StageSignals disk{};
    disk.id = StageId::Disk;
    disk.available = s.disk.latency_availability == MetricAvailability::Available;
    disk.is_duration_stage = true;
    disk.can_bottleneck = true;
    disk.avg_ms = s.disk.average_write_ms;
    disk.budget_ms = kDiskBudgetMs;

    const StageSignals signals[] = {capture, queue, comp, enc, mux, disk};
    const exosnap::engine::PipelineHealthVerdict verdict = exosnap::engine::ResolvePipelineHealth(signals, budget_ms);

    const auto health_of = [&](StageId id) {
        for (const auto& sv : verdict.per_stage) {
            if (sv.id == id)
                return sv.health;
        }
        return exosnap::engine::StageHealth::Healthy;
    };
    const auto ms = [&](double value, bool available) {
        return available ? Number(value, 1) + " ms" : std::string(kDash);
    };

    std::vector<PipelineStage> stages;
    stages.reserve(6);

    {
        PipelineStage stage;
        stage.key = "capture";
        stage.title = "Source capture";
        stage.lane = "CPU";
        stage.status = StatusOf(health_of(StageId::SourceCapture));
        stage.value = s.capture.target_fps > 0.0
                          ? Number(s.capture.actual_fps, 1) + " / " + Number(s.capture.target_fps, 1) + " fps"
                          : std::string(kDash);
        stage.tip = (s.capture.acquire_availability == MetricAvailability::Available)
                        ? "Acquire " + Number(s.capture.acquire_average_ms, 2) + " ms (CPU)"
                        : "Acquire timing unavailable for this capture mode";
        stages.push_back(std::move(stage));
    }

    {
        PipelineStage stage;
        stage.key = "queue";
        stage.title = "Frame queue";
        stage.lane = kDash;
        stage.status = StatusOf(health_of(StageId::FrameQueue));
        stage.value = s.video_queue.bounded && s.video_queue.capacity > 0
                          ? Number(s.video_queue.current_depth) + " / " + Number(s.video_queue.capacity)
                          : Number(s.video_queue.current_depth);
        stage.tip = "Frames waiting between encode and mux (peak " + Number(s.video_queue.peak_depth) + ")";
        stages.push_back(std::move(stage));
    }

    {
        PipelineStage stage;
        stage.key = "compositor";
        stage.title = "Compositor";
        stage.lane = "GPU";
        stage.status = StatusOf(health_of(StageId::Compositor));
        stage.value = ms(s.compositor.average_ms, s.compositor.average_ms > 0.0);
        stage.tip = "CPU submit (GPU execution time not measured in this view). VPBlt " +
                    ((s.compositor.vpblt_availability == MetricAvailability::Available)
                         ? Number(s.compositor.vpblt_average_ms, 2) + " ms"
                         : std::string(kDash));
        stages.push_back(std::move(stage));
    }

    {
        PipelineStage stage;
        stage.key = "encoder";
        stage.title = "Encoder";
        stage.lane = "GPU (NVENC)";
        stage.status = StatusOf(health_of(StageId::Encoder));
        stage.value = ms(s.video_encoder.average_ms, s.video_encoder.average_ms > 0.0);
        stage.tip = std::string("CPU submit") + kRightArrow + "ready latency (peak " +
                    Number(s.video_encoder.peak_ms, 1) + " ms)";
        stages.push_back(std::move(stage));
    }

    {
        PipelineStage stage;
        stage.key = "muxer";
        stage.title = "Muxer";
        stage.lane = "CPU";
        stage.status = StatusOf(health_of(StageId::Muxer));
        stage.value = ms(s.mux.process_average_ms, mux.available);
        stage.tip = "Mux drain processing (peak " + Number(s.mux.process_peak_ms, 2) + " ms)";
        stages.push_back(std::move(stage));
    }

    {
        PipelineStage stage;
        stage.key = "disk";
        stage.title = "Disk";
        stage.lane = "CPU";
        stage.status = StatusOf(health_of(StageId::Disk));
        stage.value = ms(s.disk.average_write_ms, disk.available);
        stage.tip = "Filesystem write-call latency (peak " + Number(s.disk.peak_write_ms, 1) + " ms)";
        stages.push_back(std::move(stage));
    }

    return stages;
}

// ── RefreshThrottle ─────────────────────────────────────────────────────────────

RefreshThrottle::RefreshThrottle(std::chrono::milliseconds interval) noexcept : interval_(interval) {
}

bool RefreshThrottle::Allow(Clock::time_point now) {
    if (last_ != Clock::time_point{} && (now - last_) < interval_)
        return false;
    last_ = now;
    return true;
}

void RefreshThrottle::Reset() noexcept {
    last_ = {};
}

// ── DiagnosticsController ───────────────────────────────────────────────────────

void DiagnosticsController::SetConfig(Config config) {
    config_ = std::move(config);
    data_ready_ = true;
    config_rows_ = BuildConfigRows(config_.config_summary);
}

void DiagnosticsController::SetProbeResult(ProbeResult probe) {
    probe_ = std::move(probe);
    if (probe_.self_test_valid)
        self_test_ = BuildSelfTestReport(probe_.self_test);
}

void DiagnosticsController::SetDisplayFacts(DisplayFacts facts) noexcept {
    display_ = facts;
}

void DiagnosticsController::SetSelectedCaptureTarget(std::optional<exosnap::engine::CaptureTarget> target) {
    selected_target_ = std::move(target);
}

void DiagnosticsController::SetCaptureWindowEvidence(std::optional<WindowTargetFacts> facts,
                                                     const WindowHubEvidence& hub) {
    capture_window_facts_ = std::move(facts);
    capture_window_hub_ = hub;
}

void DiagnosticsController::SetSavedDisplayUnresolved(bool unresolved, std::string label) {
    saved_display_unresolved_ = unresolved;
    saved_display_label_ = std::move(label);
}

void DiagnosticsController::SetElevated(bool elevated) noexcept {
    elevated_ = elevated;
}

void DiagnosticsController::SetHasLastRecording(bool has_last_recording) noexcept {
    has_last_recording_ = has_last_recording;
}

void DiagnosticsController::SetCaptureTargetHdrActive(bool active) noexcept {
    capture_target_hdr_active_ = active;
}

void DiagnosticsController::SetDpcLatency(std::optional<DpcLatencyReading> reading) {
    dpc_ = std::move(reading);
}

void DiagnosticsController::SetPresentSample(std::optional<PresentSample> sample) {
    present_ = std::move(sample);
}

void DiagnosticsController::SetLiveSnapshot(const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot) {
    live_ = snapshot;
    if (!liveRecording())
        pipeline_.Reset();
}

bool DiagnosticsController::dataReady() const noexcept {
    return data_ready_;
}

bool DiagnosticsController::hasLastRecording() const noexcept {
    return has_last_recording_;
}

bool DiagnosticsController::elevated() const noexcept {
    return elevated_;
}

const std::string& DiagnosticsController::outputFolder() const noexcept {
    return config_.output_folder;
}

const SelfTestReport& DiagnosticsController::selfTest() const noexcept {
    return self_test_;
}

const std::vector<KeyValueRow>& DiagnosticsController::configRows() const noexcept {
    return config_rows_;
}

bool DiagnosticsController::liveRecording() const noexcept {
    return live_.valid && (live_.lifecycle == exosnap::engine::DiagnosticsLifecycle::Recording ||
                           live_.lifecycle == exosnap::engine::DiagnosticsLifecycle::Paused);
}

const DiagnosticChecklist& DiagnosticsController::lastChecklist() const noexcept {
    return last_checklist_;
}

const std::vector<DiagnosticResult>& DiagnosticsController::lastEnvironmentFacts() const noexcept {
    return last_facts_;
}

const DiagnosticChecklist& DiagnosticsController::selfTestChecklist() const noexcept {
    return probe_.self_test;
}

bool DiagnosticsController::selfTestValid() const noexcept {
    return probe_.self_test_valid;
}

const exosnap::engine::RecordingDiagnosticsSnapshot& DiagnosticsController::liveSnapshot() const noexcept {
    return live_;
}

DiagnosticsSnapshot DiagnosticsController::Evaluate() {
    DiagnosticsSnapshot out;

    if (!data_ready_) {
        last_checklist_ = {};
        last_facts_.clear();
        out.verdict = ComputeVerdict({}, 0, false);
        ReadinessTileInputs tile_inputs;
        tile_inputs.display_width = display_.width;
        tile_inputs.display_height = display_.height;
        tile_inputs.display_refresh_hz = display_.refresh_hz;
        tile_inputs.has_last_recording = has_last_recording_;
        out.tiles = BuildReadinessTiles(tile_inputs);
        return out;
    }

    constexpr uint32_t kMonitorRefreshUnknown = 0;
    const exosnap::engine::RecordingDiagnosticsSnapshot* live = live_.valid ? &live_ : nullptr;
    const PresentSample* present = (present_.has_value() && present_->available) ? &present_.value() : nullptr;

    RecommendationEngine engine(config_.caps, config_.user_config, kMonitorRefreshUnknown, probe_.free_bytes,
                                config_.profile_validation.succeeded, probe_.filesystem_name, live, present);
    if (dpc_.has_value())
        engine.SetDpcLatency(*dpc_);
    engine.SetOutputPathWritable(probe_.output_path_writable);
    engine.SetElevated(elevated_);
    engine.SetCaptureTargetHdrActive(capture_target_hdr_active_);
    engine.SetSavedDisplayUnresolved(saved_display_unresolved_, saved_display_label_);
    engine.SetCaptureWindowEvidence(capture_window_facts_, capture_window_hub_);

    const DiagnosticChecklist recommendations = engine.Generate();
    const std::vector<DiagnosticResult> facts = engine.GenerateEnvironmentFacts();
    // Retained for the structured surface, so `diagnostics.results` and the
    // Diagnostics page are two renderings of ONE evaluation rather than two
    // evaluations that happen to usually agree.
    last_checklist_ = recommendations;
    last_facts_ = facts;
    const int cap_passes = CountAvailableCapabilities(config_.cap_summary);

    out.verdict = ComputeVerdict(recommendations, cap_passes, true);

    ReadinessTileInputs tile_inputs;
    tile_inputs.data_ready = true;
    tile_inputs.blockers = out.verdict.blockers;
    tile_inputs.notices = out.verdict.notices;
    tile_inputs.cap_passes = cap_passes;
    tile_inputs.gpu_adapter_name = config_.caps.gpu_adapter_name;
    tile_inputs.video_codec = config_.user_config.video_codec;
    tile_inputs.audio_codec = config_.user_config.audio_codec;
    tile_inputs.container = config_.user_config.container;
    tile_inputs.free_bytes = probe_.free_bytes;
    tile_inputs.total_bytes = probe_.total_bytes;
    tile_inputs.output_drive_label = probe_.drive_label;
    tile_inputs.display_width = display_.width;
    tile_inputs.display_height = display_.height;
    tile_inputs.display_refresh_hz = display_.refresh_hz;
    tile_inputs.audio_sources = static_cast<int>(config_.audio.IsAppEnabled()) +
                                static_cast<int>(config_.audio.IsSysEnabled()) +
                                static_cast<int>(config_.audio.IsMicEnabled());
    tile_inputs.audio_sample_rate = config_.audio.audio_sample_rate;
    tile_inputs.audio_channels = config_.audio.audio_channels;
    if (selected_target_.has_value()) {
        tile_inputs.target_selected = true;
        tile_inputs.target_is_window = selected_target_->kind == exosnap::engine::CaptureTarget::Kind::Window;
        tile_inputs.target_description = selected_target_->description;
    } else {
        tile_inputs.target_is_window = config_.audio.target_kind == capability::CaptureTargetKind::Window;
    }
    tile_inputs.has_last_recording = has_last_recording_;
    out.tiles = BuildReadinessTiles(tile_inputs);

    TopIssues issues =
        BuildTopIssues(config_.profile_validation, recommendations, config_.hotkeys_ok, config_.hotkeys_summary);
    out.cards = std::move(issues.cards);
    out.tips = std::move(issues.tips);
    out.environment_rows = BuildEnvironmentRows(facts, elevated_);
    return out;
}

std::vector<PipelineStage> DiagnosticsController::BuildPipelineStages() {
    if (liveRecording())
        return pipeline_.BuildLive(live_);

    const bool encoder_ok =
        data_ready_ && capability::IsSelectable(config_.caps.QueryVideoCodec(config_.user_config.video_codec).level);
    const bool muxer_ok =
        data_ready_ && capability::IsSelectable(config_.caps.QueryContainer(config_.user_config.container).level);
    return PipelineCardBuilder::BuildStatic(data_ready_, encoder_ok, muxer_ok, probe_.output_path_writable);
}

} // namespace exosnap::diagnostics
