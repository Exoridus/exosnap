#include "DiagnosticsController.h"
#include <QCoreApplication>
#include <capability/translatable.h>

#include "DiagnosticsPresentation.h"

// Qt Core arrives with this header (it carries UiRecordingResult); nothing below
// may use `signals` as an identifier while its moc keyword macro is defined.
#include "../viewmodels/RecordViewModel.h"

#include <capability/support_level.h>
#include <exosnap/engine/pipeline_health.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace exosnap::diagnostics {
namespace {

constexpr const char* kDash = "\xe2\x80\x94";   // em dash
constexpr const char* kMiddot = "\xc2\xb7";     // middle dot
constexpr const char* kNarrowNbsp = "\xc2\xa0"; // non-breaking space
constexpr const char* kRightArrow = "\xe2\x86\x92";
constexpr const char* kMicro = "\xc2\xb5"; // micro sign

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
    card.summary = result.impact.empty() ? result.summary : result.impact;
    card.why = result.recommendation;
    card.measured = result.current_value;
    card.log_excerpt = result.detail;
    if (!result.compensation.empty())
        card.log_excerpt += EXOSNAP_TRANSLATABLE("Diagnostics", "\nCompensation: ") + result.compensation;
    if (!result.likely_cause.empty())
        card.log_excerpt += EXOSNAP_TRANSLATABLE("Diagnostics", "\nLikely cause: ") + result.likely_cause;
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

std::string_view ValueToneKey(ValueTone tone) noexcept {
    switch (tone) {
    case ValueTone::Neutral:
        return "neutral";
    case ValueTone::Ok:
        return "ok";
    case ValueTone::Warn:
        return "warn";
    case ValueTone::Critical:
        return "critical";
    }
    return "neutral";
}

std::string_view SelfTestStateLabel(SelfTestState state) noexcept {
    switch (state) {
    case SelfTestState::NotRun:
        return EXOSNAP_TRANSLATABLE("Diagnostics", "Not run");
    case SelfTestState::Pass:
        return "PASS";
    case SelfTestState::Warn:
        return "WARN";
    }
    return EXOSNAP_TRANSLATABLE("Diagnostics", "Not run");
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

std::string PresentModeLabel(exosnap::engine::PresentMode mode) {
    switch (mode) {
    case exosnap::engine::PresentMode::Composed:
        return "Composed";
    case exosnap::engine::PresentMode::IndependentFlip:
        return EXOSNAP_TRANSLATABLE("Diagnostics", "Independent flip");
    case exosnap::engine::PresentMode::ExclusiveFullscreen:
        return EXOSNAP_TRANSLATABLE("Diagnostics", "Exclusive fullscreen");
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

// How long ago a quiet problem was last measured, quantised. The band's headline
// is deliberately restricted to counts so it does not rewrite itself twice a
// second; a subline counting up in whole seconds under it would do exactly that
// for the whole quiet stretch.
std::string LastSeenAgo(double seconds) {
    if (seconds < 10.0)
        return "just now";
    if (seconds < 60.0)
        return Number(std::floor(seconds / 10.0) * 10.0, 0) + " s ago";
    return Number(std::floor(seconds / 60.0), 0) + " min ago";
}

std::string Join(const std::string& left, const std::string& right) {
    if (left.empty())
        return right;
    if (right.empty())
        return left;
    return left + " " + kMiddot + " " + right;
}

// The tint of one number, from the ledger entry of the check that owns it and
// from nothing else. No entry means the check has never fired this session, which
// is the only thing that earns green. `sticky` is for a small sub-value (jitter,
// p99): once the check that owns it has fired, the sub-value stays amber for the
// rest of the session even while the check is quiet, because the sub-line is where
// the session's history is read.
ValueTone OwnedTone(const SessionLedger& ledger, std::initializer_list<std::string_view> owner_ids, bool sticky) {
    bool entered = false;
    for (const LedgerEntry& entry : ledger.entries()) {
        for (const std::string_view id : owner_ids) {
            if (entry.id != id)
                continue;
            if (entry.active)
                return ValueTone::Warn;
            entered = true;
        }
    }
    if (!entered)
        return ValueTone::Ok;
    return sticky ? ValueTone::Warn : ValueTone::Ok;
}

// A Tier-1 blocker on this stage outranks any ledger entry: the engine calling the
// pipeline Critical is a statement about the recording, not about one measurement.
void EscalateOnBlocker(LiveTile& tile) {
    if (tile.tone != TileTone::Blocker)
        return;
    if (tile.value_tone != ValueTone::Neutral)
        tile.value_tone = ValueTone::Critical;
    if (!tile.sub_tinted.empty())
        tile.sub_tone = ValueTone::Critical;
}

std::string PresentSampleModeLabel(PresentMode mode) {
    switch (mode) {
    case PresentMode::Composed:
        return "Composed";
    case PresentMode::IndependentFlip:
        return EXOSNAP_TRANSLATABLE("Diagnostics", "Independent flip");
    case PresentMode::ExclusiveFullscreen:
        return EXOSNAP_TRANSLATABLE("Diagnostics", "Exclusive fullscreen");
    case PresentMode::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

double FrameBudgetMs(const exosnap::engine::RecordingDiagnosticsSnapshot& s) noexcept {
    return s.capture.target_fps > 0.0 ? 1000.0 / s.capture.target_fps : 0.0;
}

LiveTile FramePacingTile(const LiveTileInputs& in) {
    const exosnap::engine::RecordingDiagnosticsSnapshot& s = in.snapshot;
    LiveTile tile;
    tile.key = "framePacing";
    tile.title = QCoreApplication::translate("Diagnostics", "Frame pacing").toStdString();
    tile.value = Number(s.capture.actual_fps, 2) + " fps";
    tile.sub = Number(s.pacing.affected_slots) + " affected output slots";
    tile.tone =
        ToneOfStage(s, {exosnap::engine::PipelineBottleneck::Capture, exosnap::engine::PipelineBottleneck::Compositor});
    tile.value_tone = OwnedTone(in.ledger, {"rec.001"}, false);
    if (s.pacing.recent_affected_slots > 0)
        tile.tone = TileTone::Notice;
    tile.detail = tile.tone == TileTone::Neutral
                      ? QCoreApplication::translate("Diagnostics", "Healthy").toStdString()
                      : QCoreApplication::translate("Diagnostics", "Recording pacing affected").toStdString();
    if (s.elapsed_seconds > 0.0) {
        tile.session_detail =
            "session avg " + Number(static_cast<double>(s.capture.frames_emitted) / s.elapsed_seconds, 2) + " fps";
    }
    EscalateOnBlocker(tile);
    return tile;
}

LiveTile EncoderTile(const LiveTileInputs& in) {
    const exosnap::engine::RecordingDiagnosticsSnapshot& s = in.snapshot;
    LiveTile tile;
    tile.key = "encoder";
    tile.title = "Encoder";
    // What is ACTUALLY running, from the encoder's initialization record. Falls
    // back to the codec the diagnostics stream reports when no encoder has been
    // configured -- never to the configured preset, which is a request.
    tile.value = CodecName(s.encoder_init.valid ? s.encoder_init.codec : s.video_encoder.codec);

    if (s.video_encoder.frames_encoded > 0) {
        tile.sub_tinted = "p99 " + Number(s.video_encoder.p99_ms, 1) + " ms";
        tile.sub = tile.sub_tinted;
        if (s.video_timing.budget_ms > 0.0)
            tile.sub = Join(tile.sub, "budget " + Number(s.video_timing.budget_ms, 2) + " ms");
        tile.sub_tone = OwnedTone(in.ledger, {"rec.gpu.contention"}, /*sticky=*/true);
    } else {
        tile.sub = QCoreApplication::translate("Diagnostics", "No frame encoded yet").toStdString();
    }
    tile.detail =
        QCoreApplication::translate("Diagnostics", "Backlog ").toStdString() + Number(s.video_encoder.backlog);
    tile.tone = ToneOfStage(s, {exosnap::engine::PipelineBottleneck::VideoEncoder});
    // The headline is a codec name, and no check measures a codec name.
    tile.value_tone = ValueTone::Neutral;
    if (const double budget = FrameBudgetMs(s); budget > 0.0)
        tile.budget = budget;
    // The engine's encoder percentiles are rolling-window (about 2 s), so there is
    // no session-wide p99 to quote. What IS session-wide is how many frames the
    // encoder got through against the session clock.
    if (s.elapsed_seconds > 0.0) {
        tile.session_detail = "session avg " +
                              Number(static_cast<double>(s.video_encoder.frames_encoded) / s.elapsed_seconds, 2) +
                              " fps encoded";
    }
    EscalateOnBlocker(tile);
    return tile;
}

LiveTile AudioSyncTile(const LiveTileInputs& in) {
    const exosnap::engine::RecordingDiagnosticsSnapshot& s = in.snapshot;
    LiveTile tile;
    tile.key = "audioSync";
    tile.title = QCoreApplication::translate("Diagnostics", "Audio sync").toStdString();
    if (!s.audio.active) {
        tile.value = QCoreApplication::translate("Diagnostics", "No audio").toStdString();
        tile.sub = QCoreApplication::translate("Diagnostics", "This recording has no audio track").toStdString();
        tile.detail.clear();
        return tile;
    }

    const bool drift_faulted = s.av_drift_availability == exosnap::engine::MetricAvailability::Faulted;
    if (s.av_drift_availability == exosnap::engine::MetricAvailability::Available) {
        const std::string sign = s.av_drift_ms >= 0.0 ? "+" : "";
        tile.value = sign + Number(s.av_drift_ms, 1) + " ms";
        tile.value_tone = OwnedTone(in.ledger, {"rec.audio.clock_saturated"}, /*sticky=*/false);
    } else if (drift_faulted) {
        // Sampled and known-wrong. QCoreApplication::translate("Diagnostics", "Unavailable").toStdString() would send
        // the reader off to wait for a value that already arrived, and a number would be a sync claim the measurement
        // cannot carry.
        tile.value = QCoreApplication::translate("Diagnostics", "Not measurable").toStdString();
    } else {
        // A multi-source merge mixes several device clocks and does not report.
        // Zero here would claim perfect sync on a recording nobody measured.
        tile.value = QCoreApplication::translate("Diagnostics", "Unavailable").toStdString();
    }

    tile.sub = Number(static_cast<uint64_t>(s.audio.sample_rate / 1000)) +
               QCoreApplication::translate("Diagnostics", " kHz").toStdString();
    tile.sub = Join(tile.sub, s.audio.channels == 1 ? std::string("Mono") : std::string("Stereo"));

    std::string detail;
    if (s.peak_av_drift_availability == exosnap::engine::MetricAvailability::Available)
        detail =
            QCoreApplication::translate("Diagnostics", "peak ").toStdString() + Number(s.peak_av_drift_ms, 1) + " ms";
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
    // The engine latches the peak drift for the whole session and resets it per
    // recording, so this one IS the session figure.
    if (s.peak_av_drift_availability == exosnap::engine::MetricAvailability::Available)
        tile.session_detail = "session peak " + Number(s.peak_av_drift_ms, 1) + " ms";

    tile.tone = ToneOfStage(s, {exosnap::engine::PipelineBottleneck::Audio});
    // A degraded source is a MEASURED problem in its own right and is
    // reported as one even while the engine still calls the pipeline healthy --
    // the recording keeps running, which is why it never escalates past Notice.
    if (s.audio.source_degraded && tile.tone == TileTone::Neutral)
        tile.tone = TileTone::Notice;
    EscalateOnBlocker(tile);
    return tile;
}

LiveTile StorageTile(const LiveTileInputs& in) {
    const exosnap::engine::RecordingDiagnosticsSnapshot& s = in.snapshot;
    LiveTile tile;
    tile.key = "storage";
    tile.title = "Storage";
    tile.value =
        Number(s.disk.throughput_mib_s, 0) + QCoreApplication::translate("Diagnostics", " MiB/s").toStdString();
    // Throughput has no owning check: it is a property of what is being written,
    // not a budget anything is spending. Neutral ink, never a verdict colour.
    tile.value_tone = ValueTone::Neutral;
    tile.sub =
        QCoreApplication::translate("Diagnostics", "Write failures ").toStdString() + Number(s.disk.write_failures);
    if (s.disk.latency_availability == exosnap::engine::MetricAvailability::Available) {
        tile.sub_tinted = "peak write " + Number(s.disk.peak_write_ms, 0) + " ms";
        tile.sub = Join(tile.sub, tile.sub_tinted);
        tile.sub_tone = OwnedTone(in.ledger, {"rec.disk.writestall"}, /*sticky=*/true);
    }
    // Negative means the estimate could not be made (unknown throughput or no
    // free-space reading), which is a different answer from "no time left".
    tile.detail = s.disk_fill_eta_seconds >= 0.0
                      ? QCoreApplication::translate("Diagnostics", "Est. remaining ").toStdString() +
                            CoarseDuration(s.disk_fill_eta_seconds)
                      : QCoreApplication::translate("Diagnostics", "Remaining time unavailable").toStdString();
    // disk.throughput_mib_s is an interval rate between publishes; the session
    // average is the bytes actually on disk over the session clock.
    if (s.elapsed_seconds > 0.0) {
        const double mib = static_cast<double>(s.disk.bytes_written) / (1024.0 * 1024.0);
        tile.session_detail = "session avg " + Number(mib / s.elapsed_seconds, 0) +
                              QCoreApplication::translate("Diagnostics", " MiB/s").toStdString();
    }
    tile.tone = ToneOfStage(s, {exosnap::engine::PipelineBottleneck::Disk, exosnap::engine::PipelineBottleneck::Muxer});
    if (s.disk.write_failures > 0 && tile.tone == TileTone::Neutral)
        tile.tone = TileTone::Notice;
    EscalateOnBlocker(tile);
    return tile;
}

// ---- In-depth row (elevation + the present/DPC opt-in) ------------------------

// Why an in-depth tile has no number, said in the tile itself. The row is full
// whenever the switch is on, so a tile with no reading has to account for itself
// rather than leave a gap where the reader expects a measurement.
constexpr const char* kNoPresentTrace =
    EXOSNAP_TRANSLATABLE("Diagnostics", "Enhanced presentation telemetry unavailable");
constexpr const char* kNoDpcTrace = "DPC/ISR trace is not reporting";

LiveTile PresentModeTile(const LiveTileInputs& in) {
    LiveTile tile;
    tile.key = "presentMode";
    tile.title = QCoreApplication::translate("Diagnostics", "Presentation / Mode").toStdString();
    if (!in.present.has_value() || !in.present->available) {
        tile.value = kDash;
        tile.detail = kNoPresentTrace;
        return tile;
    }
    const PresentSample& p = *in.present;
    tile.value = PresentSampleModeLabel(p.mode);
    tile.sub = p.tearing ? "tearing active" : "no tearing";
    tile.detail = p.present_interval_ms > 0.0 ? "interval " + Number(p.present_interval_ms, 1) + " ms"
                                              : std::string("interval not measured yet");
    return tile;
}

LiveTile PresentHealthTile(const LiveTileInputs& in) {
    LiveTile tile;
    tile.key = "presentHealth";
    tile.title = QCoreApplication::translate("Diagnostics", "Presentation / Discards").toStdString();
    if (!in.present.has_value() || !in.present->available) {
        tile.value = kDash;
        tile.detail = kNoPresentTrace;
        return tile;
    }
    const PresentSample& p = *in.present;
    const double discarded_pct =
        p.present_count > 0 ? static_cast<double>(p.discarded_count) * 100.0 / static_cast<double>(p.present_count)
                            : 0.0;
    tile.value = Number(discarded_pct, 1) + "% discarded";
    tile.value_tone = OwnedTone(in.ledger, {"rec.present.discarded"}, /*sticky=*/false);

    tile.sub_tinted = Number(static_cast<uint64_t>(p.mode_flip_count)) + " mode flips";
    tile.sub = Join(tile.sub_tinted, Number(static_cast<uint64_t>(p.present_count)) + " presents");
    tile.sub_tone = OwnedTone(in.ledger, {"rec.present.modeflip"}, /*sticky=*/true);
    tile.detail = "coalesce " + Number(in.snapshot.capture.source_coalesce_ratio, 2) + "x";
    return tile;
}

LiveTile DpcLatencyTile(const LiveTileInputs& in) {
    LiveTile tile;
    tile.key = "dpcLatency";
    tile.title = "System latency / DPC / ISR";
    if (!in.dpc.has_value() || !in.dpc->available) {
        tile.value = kDash;
        tile.detail = kNoDpcTrace;
        return tile;
    }
    const DpcLatencyReading& d = *in.dpc;
    tile.value = Number(d.max_latency_us, 0) + " " + kMicro + "s";
    tile.value_tone = OwnedTone(in.ledger, {"rec.dpc.latency"}, /*sticky=*/false);

    tile.sub = Join("avg " + Number(d.avg_latency_us, 0) + " " + kMicro + "s", "raw peak; impact assessed separately");
    tile.detail = d.worst_driver.empty() ? std::string("no driver attributed") : "worst " + d.worst_driver;
    return tile;
}

LiveTile GpuTimeTile(const LiveTileInputs& in) {
    LiveTile tile;
    tile.key = "gpuTime";
    tile.title = QCoreApplication::translate("Diagnostics", "GPU / Recorder execution").toStdString();
    tile.value = in.gpu_exec_p99_ms > 0.0 ? Number(in.gpu_exec_p99_ms, 2) + " ms" : kDash;
    tile.value_tone = OwnedTone(in.ledger, {"rec.gpu.contention"}, /*sticky=*/false);
    const double budget = FrameBudgetMs(in.snapshot);
    if (budget > 0.0) {
        tile.budget = budget;
        tile.sub = "budget " + Number(budget, 2) + " ms";
    }
    tile.detail = "p99 of the recorder's own GPU passes";
    return tile;
}

} // namespace

std::vector<LiveTile> BuildLiveTiles(const LiveTileInputs& in) {
    const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot = in.snapshot;
    const bool live = snapshot.lifecycle == exosnap::engine::DiagnosticsLifecycle::Recording ||
                      snapshot.lifecycle == exosnap::engine::DiagnosticsLifecycle::Paused;
    if (!snapshot.valid || !live)
        return {};

    std::vector<LiveTile> tiles{FramePacingTile(in), EncoderTile(in), AudioSyncTile(in), StorageTile(in)};
    if (!in.in_depth)
        return tiles;

    const auto evidence = [&tiles](std::string key, std::string title, std::string value, std::string detail) {
        LiveTile tile;
        tile.key = std::move(key);
        tile.title = std::move(title);
        tile.value = std::move(value);
        tile.detail = std::move(detail);
        tiles.push_back(std::move(tile));
    };
    const auto timing = [&evidence](const char* key, const std::string& title,
                                    const exosnap::engine::TimingDistribution& d) {
        evidence(key, title, d.samples ? Number(d.p95_ms, 2) + " ms p95" : kDash,
                 d.samples ? "p50 " + Number(d.p50_ms, 2) + " / p99 " + Number(d.p99_ms, 2) + " ms"
                           : QCoreApplication::translate("Diagnostics", "Not measured").toStdString());
    };
    timing("selectionResidual", QCoreApplication::translate("Diagnostics", "Timing / Selection residual").toStdString(),
           snapshot.pacing.absolute_residual);
    timing("selectedAge", QCoreApplication::translate("Diagnostics", "Timing / Selected frame age").toStdString(),
           snapshot.pacing.selected_frame_age);
    timing("scheduler", QCoreApplication::translate("Diagnostics", "Timing / Worker lateness").toStdString(),
           snapshot.pacing.worker_lateness);
    evidence("sourceTiming", QCoreApplication::translate("Diagnostics", "Timing / Source").toStdString(),
             snapshot.capture.present_cadence_availability == exosnap::engine::MetricAvailability::Available
                 ? Number(snapshot.capture.source_present_jitter_ms, 2) + " ms variation"
                 : kDash,
             QCoreApplication::translate("Diagnostics", "Raw source variation is not output loss").toStdString());
    evidence("ring", QCoreApplication::translate("Diagnostics", "Timing / Selection ring").toStdString(),
             snapshot.pacing.selection_samples ? Number(snapshot.pacing.ring_occupancy) + " frames" : kDash,
             snapshot.pacing.selection_samples
                 ? Number(snapshot.pacing.ring_misses) + " misses; longest duplicate run " +
                       Number(snapshot.pacing.longest_duplicate_run)
                 : QCoreApplication::translate("Diagnostics", "Present-time selection unavailable").toStdString());
    tiles.push_back(GpuTimeTile(in));
    const auto percent = [](const std::optional<double>& v) { return v ? Number(*v, 0) + "%" : std::string(kDash); };
    evidence(
        "gpuUtilization", QCoreApplication::translate("Diagnostics", "GPU / Device utilization").toStdString(),
        percent(in.gpu.utilization_percent),
        QCoreApplication::translate("Diagnostics", "Device-wide; high usage alone is not a problem").toStdString());
    evidence("encoderUtilization",
             QCoreApplication::translate("Diagnostics", "GPU / Encoder utilization").toStdString(),
             percent(in.gpu.encoder_utilization_percent), in.gpu.metadata.source);
    evidence("gpuTemperature", QCoreApplication::translate("Diagnostics", "GPU / Temperature").toStdString(),
             in.gpu.temperature_celsius ? Number(static_cast<uint64_t>(*in.gpu.temperature_celsius)) + " C" : kDash,
             in.gpu.graphics_clock_mhz ? Number(static_cast<uint64_t>(*in.gpu.graphics_clock_mhz)) +
                                             QCoreApplication::translate("Diagnostics", " MHz").toStdString()
                                       : QCoreApplication::translate("Diagnostics", "Clock unavailable").toStdString());
    evidence("videoMemory", QCoreApplication::translate("Diagnostics", "GPU / Process local memory").toStdString(),
             in.video_memory.local ? Number(in.video_memory.local->current_usage_bytes / (1024 * 1024)) +
                                         QCoreApplication::translate("Diagnostics", " MiB").toStdString()
                                   : kDash,
             in.video_memory.local ? QCoreApplication::translate("Diagnostics", "Budget ").toStdString() +
                                         Number(in.video_memory.local->budget_bytes / (1024 * 1024)) +
                                         QCoreApplication::translate("Diagnostics", " MiB; headroom ").toStdString() +
                                         Number(in.video_memory.local->headroom_bytes() / (1024 * 1024)) +
                                         QCoreApplication::translate("Diagnostics", " MiB").toStdString()
                                   : "DXGI budget unavailable");
    std::string ownership;
    uint64_t logical_bytes = 0;
    for (const auto& surface : in.video_memory.logical_surfaces) {
        if (!surface.owner || surface.surfaces == 0)
            continue;
        logical_bytes += surface.bytes;
        if (!ownership.empty())
            ownership += "; ";
        ownership += std::string(surface.owner) + " " + Number(surface.bytes / (1024 * 1024)) +
                     QCoreApplication::translate("Diagnostics", " MiB").toStdString();
    }
    evidence(
        "gpuSurfaceOwnership", QCoreApplication::translate("Diagnostics", "GPU / Tracked surface texels").toStdString(),
        in.video_memory.logical_surfaces_sampled
            ? Number(logical_bytes / (1024 * 1024)) + QCoreApplication::translate("Diagnostics", " MiB").toStdString()
            : kDash,
        QCoreApplication::translate("Diagnostics", "All process adapters; excludes driver storage and external pools. ")
                .toStdString() +
            ownership);
    evidence("encoderDeviceStats",
             QCoreApplication::translate("Diagnostics", "GPU / Device encoder sessions").toStdString(),
             in.gpu.encoder_sessions ? Number(static_cast<uint64_t>(*in.gpu.encoder_sessions)) : kDash,
             in.gpu.encoder_average_latency_us
                 ? QCoreApplication::translate("Diagnostics", "Average latency ").toStdString() +
                       Number(static_cast<uint64_t>(*in.gpu.encoder_average_latency_us)) + " us (all encoder sessions)"
                 : QCoreApplication::translate("Diagnostics", "Session latency unavailable").toStdString());
    evidence("gpuPowerState", QCoreApplication::translate("Diagnostics", "GPU / Performance state").toStdString(),
             in.gpu.performance_state ? "P" + Number(static_cast<uint64_t>(*in.gpu.performance_state)) : kDash,
             QCoreApplication::translate("Diagnostics",
                                         "Read-only device performance state; no thermal or power cause inferred")
                 .toStdString());
    tiles.push_back(PresentModeTile(in));
    tiles.push_back(PresentHealthTile(in));
    tiles.push_back(DpcLatencyTile(in));
    evidence("storageLatency", QCoreApplication::translate("Diagnostics", "Storage / Write latency").toStdString(),
             snapshot.disk.latency_availability == exosnap::engine::MetricAvailability::Available
                 ? Number(snapshot.disk.peak_write_ms, 1) + " ms peak"
                 : kDash,
             snapshot.bottleneck == exosnap::engine::PipelineBottleneck::Disk
                 ? QCoreApplication::translate("Diagnostics", "Storage pressure affected the pipeline").toStdString()
                 : QCoreApplication::translate("Diagnostics", "No measured storage backpressure").toStdString());
    timing("producerWait", QCoreApplication::translate("Diagnostics", "Storage / Producer wait").toStdString(),
           snapshot.video_queue.producer_wait);
    for (const auto& fact : in.ledger.compensated())
        evidence("compensated/" + fact.id,
                 QCoreApplication::translate("Diagnostics", "Compensated / ").toStdString() + fact.title,
                 Number(static_cast<uint64_t>(fact.count)) + " observed periods", fact.compensation);
    return tiles;
}

std::vector<LiveTile> BuildLiveTiles(const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot) {
    static const SessionLedger kNoLedger;
    return BuildLiveTiles(LiveTileInputs{snapshot, kNoLedger});
}

bool NeedsElevation(std::string_view id) noexcept {
    return id.rfind("rec.present.", 0) == 0 || id.rfind("rec.dpc.", 0) == 0;
}

std::string TrimVendorPrefix(std::string adapter_name) {
    // Longest first: "Intel(R) " must win over "Intel ".
    for (const std::string_view prefix :
         {"NVIDIA ", EXOSNAP_TRANSLATABLE("Diagnostics", "Intel(R) "), EXOSNAP_TRANSLATABLE("Diagnostics", "Intel "),
          "AMD ", EXOSNAP_TRANSLATABLE("Diagnostics", "Advanced Micro Devices, Inc. ")}) {
        if (adapter_name.rfind(prefix, 0) == 0) {
            adapter_name.erase(0, prefix.size());
            break;
        }
    }
    return adapter_name;
}

std::string VendorDriverVersion(uint32_t vendor_id, const std::string& wddm_version) {
    constexpr uint32_t kNvidiaVendorId = 0x10DEu;
    if (vendor_id != kNvidiaVendorId || wddm_version.empty())
        return wddm_version;

    // "A.B.C.D" exactly: four dot-separated decimal fields. Anything else is a
    // shape this translation was not derived from, and guessing at it would
    // invent a version number.
    std::array<std::string, 4> fields;
    size_t field = 0;
    for (const char c : wddm_version) {
        if (c == '.') {
            if (++field >= fields.size())
                return wddm_version;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) == 0)
            return wddm_version;
        fields[field].push_back(c);
    }
    if (field != 3)
        return wddm_version;
    // The minor field is written with four digits and the branch contributes its
    // last one; a driver that reports fewer is not the numbering this decodes.
    if (fields[2].empty() || fields[3].size() != 4)
        return wddm_version;

    const std::string digits = fields[2].substr(fields[2].size() - 1) + fields[3];
    return digits.substr(0, digits.size() - 2) + "." + digits.substr(digits.size() - 2);
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
        verdict.headline = QCoreApplication::translate("Diagnostics", "Not checked yet").toStdString();
        verdict.subline = QCoreApplication::translate(
                              "Diagnostics", "Run a check to see whether this machine is set up to record well.")
                              .toStdString();
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
        verdict.headline =
            verdict.blockers == 1
                ? QCoreApplication::translate("Diagnostics", "1 thing to fix before recording").toStdString()
                : QCoreApplication::translate("Diagnostics", "%1 things to fix before recording")
                      .arg(verdict.blockers)
                      .toStdString();
        verdict.subline =
            (verdict.blockers == 1
                 ? QCoreApplication::translate("Diagnostics",
                                               "1 blocker must be resolved before recording. See the cards below.")
                 : QCoreApplication::translate("Diagnostics",
                                               "%1 blockers must be resolved before recording. See the cards below.")
                       .arg(verdict.blockers))
                .toStdString();
        return verdict;
    }

    if (verdict.notices > 0) {
        verdict.state = VerdictState::Warn;
        verdict.headline =
            verdict.notices == 1
                ? std::string(QCoreApplication::translate("Diagnostics", "Recording works ").toStdString()) + kDash +
                      QCoreApplication::translate("Diagnostics", " 1 thing could hurt the result").toStdString()
                : std::string(QCoreApplication::translate("Diagnostics", "Recording works ").toStdString()) + kDash +
                      " " + std::to_string(verdict.notices) +
                      QCoreApplication::translate("Diagnostics", " things could hurt the result").toStdString();
        verdict.subline =
            (verdict.notices == 1
                 ? QCoreApplication::translate(
                       "Diagnostics", "You can record, but 1 issue could affect the result. See the cards below.")
                 : QCoreApplication::translate(
                       "Diagnostics", "You can record, but %1 issues could affect the result. See the cards below.")
                       .arg(verdict.notices))
                .toStdString();
        return verdict;
    }

    verdict.state = VerdictState::Ready;
    verdict.headline = QCoreApplication::translate("Diagnostics", "Ready to record").toStdString();
    verdict.subline =
        std::string(QCoreApplication::translate("Diagnostics", "Everything checks out ").toStdString()) + kDash + " " +
        QCoreApplication::translate("Diagnostics", "%1 capability checks passed.").arg(cap_passes).toStdString();
    return verdict;
}

Verdict ComputeRecordingVerdict(const DiagnosticChecklist& live_results, const SessionLedger& ledger, double now_s) {
    Verdict verdict;
    verdict.recording = true;

    for (const auto& result : live_results.results) {
        if (result.tier == DiagnosticTier::Blocker)
            ++verdict.blockers;
    }
    if (verdict.blockers > 0) {
        verdict.state = VerdictState::Blocked;
        verdict.headline = verdict.blockers == 1
                               ? std::string(QCoreApplication::translate("Diagnostics", "Recording ").toStdString()) +
                                     kDash +
                                     QCoreApplication::translate("Diagnostics", " 1 blocking problem").toStdString()
                               : std::string(QCoreApplication::translate("Diagnostics", "Recording ").toStdString()) +
                                     kDash + " " + std::to_string(verdict.blockers) +
                                     QCoreApplication::translate("Diagnostics", " blocking problems").toStdString();
        verdict.subline = QCoreApplication::translate("Diagnostics", "See the cards below.").toStdString();
        return verdict;
    }

    const std::vector<LedgerEntry>& entries = ledger.entries();
    verdict.notices = static_cast<int>(entries.size());
    if (entries.empty()) {
        verdict.state = VerdictState::Ready;
        verdict.headline = QCoreApplication::translate("Diagnostics", "RECORDING HEALTHY").toStdString();
        verdict.subline =
            QCoreApplication::translate("Diagnostics", "No recording-impacting problems detected.").toStdString();
        return verdict;
    }

    const int active = ledger.activeCount();
    verdict.state = VerdictState::Warn;
    // Counts only. A headline that moved with the measurement would rewrite
    // itself twice a second on a band the reader is trying to read.
    verdict.headline =
        std::string(QCoreApplication::translate("Diagnostics", "RECORDING DEGRADED ").toStdString()) + kDash + " " +
        std::to_string(verdict.notices) +
        (verdict.notices == 1 ? QCoreApplication::translate("Diagnostics", " problem observed").toStdString()
                              : QCoreApplication::translate("Diagnostics", " problems observed").toStdString()) +
        (active > 0
             ? ", " + std::to_string(active) + QCoreApplication::translate("Diagnostics", " active").toStdString()
             : std::string(QCoreApplication::translate("Diagnostics", ", quiet now").toStdString()));

    std::string subline;
    for (const LedgerEntry& entry : entries) {
        if (entry.active)
            subline = Join(subline, entry.title);
    }
    if (subline.empty()) {
        const auto last =
            std::max_element(entries.begin(), entries.end(),
                             [](const LedgerEntry& a, const LedgerEntry& b) { return a.last_seen_s < b.last_seen_s; });
        subline = last->title;
        if (now_s > last->last_seen_s)
            subline += QCoreApplication::translate("Diagnostics", ", last seen ").toStdString() +
                       LastSeenAgo(now_s - last->last_seen_s);
    }
    verdict.subline = std::move(subline);
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
        card.title = QCoreApplication::translate("Diagnostics", "%1 is not supported")
                         .arg(QString::fromStdString(InvalidFieldDisplayName(invalid.field)))
                         .toStdString();
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
        card.title = QCoreApplication::translate("Diagnostics", "Configuration needs validation").toStdString();
        card.summary = warning.message;
        card.why =
            QCoreApplication::translate("Diagnostics", "Run a short recording to validate quality on this machine.")
                .toStdString();
        card.log_excerpt = QCoreApplication::translate("Diagnostics", "Code: ").toStdString() + warning.code;
        PushCard(issues.cards, std::move(card));
    }

    if (!hotkeys_ok && hotkeys_summary != QCoreApplication::translate("Diagnostics", "None configured").toStdString()) {
        IssueCard card;
        card.tone = IssueTone::Notice;
        card.title = QCoreApplication::translate("Diagnostics", "Global hotkeys are not active").toStdString();
        card.summary =
            QCoreApplication::translate("Diagnostics", "Hotkeys are configured but not currently registered.")
                .toStdString();
        card.why = QCoreApplication::translate(
                       "Diagnostics", "Open the Hotkeys page and reapply the binding if shortcuts do not trigger.")
                       .toStdString();
        card.log_excerpt = QCoreApplication::translate(
                               "Diagnostics", "If the app just launched, this can clear once startup completes.")
                               .toStdString();
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

// ── Last session ────────────────────────────────────────────────────────────────

LastSession BuildLastSession(const UiRecordingResult& result, const exosnap::engine::RecordingDiagnosticsSnapshot& s,
                             const std::vector<LedgerEntry>& frozen_ledger) {
    LastSession session;
    session.valid = true;
    session.file_name = std::filesystem::path(result.output_path).filename().string();
    // The timeline is drawn on the SESSION clock, which is the clock the marks
    // carry; the media is shorter by the tail between the last encoded frame and
    // Stop, and is what a click has to be clamped to.
    session.media_duration_s = ResultDurationSeconds(result);
    session.duration_s = result.elapsed_seconds > 0.0 ? result.elapsed_seconds : session.media_duration_s;
    session.ledger = frozen_ledger;
    session.problems = static_cast<int>(frozen_ledger.size());

    const bool measured = s.valid;
    const uint64_t drops = measured ? s.real_frame_loss() : 0;
    const bool degraded = drops > 0 || s.pacing.affected_slots > 0 || s.audio.source_degraded_occurred ||
                          s.audio.discontinuities > 0 || !frozen_ledger.empty();
    session.outcome = !result.succeeded ? QCoreApplication::translate("Diagnostics", "Failed").toStdString()
                      : !measured ? QCoreApplication::translate("Diagnostics", "Outcome unavailable").toStdString()
                      : degraded  ? QCoreApplication::translate("Diagnostics", "Degraded").toStdString()
                                  : QCoreApplication::translate("Diagnostics", "Healthy").toStdString();

    // The four facts, in a fixed order. A card whose rows move between recordings
    // cannot be read at a glance, and these are read at a glance or not at all.
    {
        LastSessionFact fact;
        fact.key = "dropped";
        fact.label = QCoreApplication::translate("Diagnostics", "Real frame loss").toStdString();
        fact.value = measured ? Number(drops) : std::string(kDash);
        if (measured)
            fact.sub = QCoreApplication::translate("Diagnostics", "of ").toStdString() +
                       Number(s.capture.frames_captured) +
                       QCoreApplication::translate("Diagnostics", " captured").toStdString();
        // The one fact of the four that a check owns outright: a dropped frame is
        // missing from the file, which is not a matter of degree.
        if (measured)
            fact.tone = drops > 0 ? ValueTone::Critical : ValueTone::Ok;
        session.facts.push_back(std::move(fact));
    }
    {
        LastSessionFact fact;
        fact.key = "achieved";
        fact.label = QCoreApplication::translate("Diagnostics", "Achieved fps").toStdString();
        const double target = static_cast<double>(result.frame_rate_num) /
                              (result.frame_rate_den > 0 ? static_cast<double>(result.frame_rate_den) : 1.0);
        if (measured && s.elapsed_seconds > 0.0) {
            fact.value = Number(static_cast<double>(s.capture.frames_emitted) / s.elapsed_seconds, 2) + " fps";
        } else {
            fact.value = kDash;
        }
        fact.sub = QCoreApplication::translate("Diagnostics", "target ").toStdString() + Number(target, 0) + " fps";
        session.facts.push_back(std::move(fact));
    }
    {
        LastSessionFact fact;
        fact.key = "drift";
        fact.label = QCoreApplication::translate("Diagnostics", "Peak residual drift").toStdString();
        // The device-clock residual after correction, not the file's alignment.
        // Nothing here claims the recording is out of sync, so nothing tints it.
        if (measured && s.peak_av_drift_availability == exosnap::engine::MetricAvailability::Available) {
            fact.value = QCoreApplication::translate("Diagnostics", "peak ").toStdString() +
                         Number(s.peak_av_drift_ms, 1) + " ms";
            fact.sub = QCoreApplication::translate("Diagnostics", "device clock residual").toStdString();
        } else {
            fact.value = QCoreApplication::translate("Diagnostics", "Unavailable").toStdString();
            fact.sub =
                QCoreApplication::translate("Diagnostics", "no single audio clock to measure against").toStdString();
        }
        session.facts.push_back(std::move(fact));
    }
    {
        LastSessionFact fact;
        fact.key = "file";
        fact.label = QCoreApplication::translate("Diagnostics", "File").toStdString();
        fact.value = result.succeeded
                         ? QCoreApplication::translate("Diagnostics", "Finalized successfully").toStdString()
                         : QCoreApplication::translate("Diagnostics", "Failed").toStdString();
        fact.sub = Join(HumanBytes(result.output_file_bytes), CodecName(result.video_codec));
        fact.tone = result.succeeded ? ValueTone::Ok : ValueTone::Critical;
        session.facts.push_back(std::move(fact));
    }

    LastSessionFact pacing;
    pacing.key = "pacing";
    pacing.label = QCoreApplication::translate("Diagnostics", "Affected pacing slots").toStdString();
    pacing.value = measured && s.pacing.output_slots > 0 ? Number(s.pacing.affected_slots) : kDash;
    pacing.tone = measured && s.pacing.affected_slots > 0 ? ValueTone::Warn : ValueTone::Neutral;
    session.facts.push_back(std::move(pacing));
    LastSessionFact interruptions;
    interruptions.key = "interruptions";
    interruptions.label = QCoreApplication::translate("Diagnostics", "Audio interruptions").toStdString();
    interruptions.value = measured && s.audio.active &&
                                  s.audio.discontinuity_availability == exosnap::engine::MetricAvailability::Available
                              ? Number(s.audio.discontinuities)
                              : kDash;
    interruptions.sub = s.audio.source_degraded_occurred
                            ? QCoreApplication::translate("Diagnostics", "Audio source loss occurred").toStdString()
                            : "";
    session.facts.push_back(std::move(interruptions));

    for (const LedgerEntry& entry : frozen_ledger) {
        for (const LedgerOccurrence& occurrence : entry.occurrences) {
            TimelineMark mark;
            mark.start_s = occurrence.start_s;
            mark.end_s = occurrence.end_s;
            mark.id = entry.id;
            mark.title = entry.title;
            mark.worst = occurrence.worst;
            mark.tone = "warn";
            session.marks.push_back(std::move(mark));
        }
    }
    // Ledger occurrences only. Frame drops are counted in the Frames dropped fact
    // and nowhere else: the engine keeps no timestamped drop history, so a mark
    // could only span the whole recording, which would read as "the entire run was
    // bad" for a defect that lasted a frame.
    return session;
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
            {QCoreApplication::translate("Diagnostics", "Elevation").toStdString(),
             elevated
                 ? std::string(QCoreApplication::translate("Diagnostics", "Elevated ").toStdString()) + kDash +
                       QCoreApplication::translate("Diagnostics", " optional kernel traces may start").toStdString()
                 : std::string(QCoreApplication::translate("Diagnostics", "Standard ").toStdString()) + kDash +
                       QCoreApplication::translate("Diagnostics", " core recording health available ").toStdString() +
                       kMiddot +
                       QCoreApplication::translate("Diagnostics", " optional traces depend on the token's rights")
                           .toStdString()});
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
        row.status_text =
            row.not_run ? QCoreApplication::translate("Diagnostics", "Not run").toStdString() : result.summary;
        row.tone = row.not_run ? IssueTone::Pass : ToneOf(result.severity);
        report.rows.push_back(std::move(row));
    }

    return report;
}

// ── Readiness tiles ─────────────────────────────────────────────────────────────

std::vector<ReadinessTile> BuildReadinessTiles(const ReadinessTileInputs& in) {
    // Four, always, so the row is never ragged: what will encode this, where it
    // is going, what is being captured at, and what will be heard. The readiness
    // rollup is the verdict band's job and the finished recording is the Last
    // session card's, so neither takes a tile here.
    std::vector<ReadinessTile> tiles;
    tiles.reserve(4);

    // Tile 1 -- Encoder: the GPU carrying the encode, the backend and its driver
    // underneath, and the codec row below that. The vendor prefix goes from the
    // GPU name because the backend line already says whose encoder this is.
    {
        ReadinessTile tile;
        tile.key = "encoder";
        tile.title = "Encoder";
        if (in.data_ready) {
            std::string gpu = in.gpu_adapter_name;
            while (!gpu.empty() && std::isspace(static_cast<unsigned char>(gpu.back())) != 0)
                gpu.pop_back();
            gpu = TrimVendorPrefix(std::move(gpu));
            const std::string codec = StripBackendSuffix(VideoCodecDisplayName(in.video_codec));
            tile.value = gpu.empty() ? codec : gpu;
            // No head badge: the backend belongs in the sub-line next to the
            // driver it comes with, and a badge repeating what the line below
            // already says is chrome, not information.
            //
            // Literal while NVENC is the only encode backend that ships. AMF and
            // QSV replace it here when those backends exist -- the name then
            // comes from the encoder that was selected, not from this file.
            tile.sub = "NVENC";
            // The container is a muxer fact and belongs to the pipeline card,
            // not to the tile that answers "what encodes this".
            if (!in.driver_version.empty())
                tile.sub = Join(tile.sub, QCoreApplication::translate("Diagnostics", "driver %1")
                                              .arg(QString::fromStdString(in.driver_version))
                                              .toStdString());
            if (in.caps != nullptr) {
                // Canon order, not capability order: the row is a fixed reference
                // the user learns the position of, so a codec never moves because
                // this GPU cannot encode it.
                bool any_encodable = false;
                for (const capability::VideoCodec codec_id :
                     {capability::VideoCodec::H264, capability::VideoCodec::Hevc, capability::VideoCodec::Av1}) {
                    CodecChip chip;
                    chip.label = StripBackendSuffix(VideoCodecDisplayName(codec_id));
                    chip.selected = codec_id == in.video_codec;
                    chip.available = capability::IsSelectable(in.caps->QueryVideoCodec(codec_id).level);
                    any_encodable = any_encodable || chip.available;
                    tile.chips.push_back(std::move(chip));
                }
                // A cross on one chip is a 9 px cue for the one thing on this page
                // that stops a recording from happening at all. The tile carries
                // the severity: the selected codec cannot be encoded here, or
                // nothing can be.
                const bool selected_encodable =
                    capability::IsSelectable(in.caps->QueryVideoCodec(in.video_codec).level);
                if (!selected_encodable || !any_encodable)
                    tile.tone = TileTone::Blocker;
            }
        } else {
            tile.value = kDash;
            tile.sub = QCoreApplication::translate("Diagnostics", "active encoder").toStdString();
        }
        tiles.push_back(std::move(tile));
    }

    // Tile 2 — Disk. A queried zero is a FULL drive and must read "0.0 GB", not
    // blank; only an unqueryable volume shows the dash.
    {
        ReadinessTile tile;
        tile.key = "disk";
        tile.title = QCoreApplication::translate("Diagnostics", "Disk").toStdString();
        if (in.data_ready && in.free_bytes.has_value()) {
            tile.value = HumanBytes(*in.free_bytes);
            if (in.total_bytes > 0) {
                const double used = 1.0 - static_cast<double>(*in.free_bytes) / static_cast<double>(in.total_bytes);
                tile.has_usage_bar = true;
                tile.usage_percent = std::clamp(static_cast<int>(used * 100.0 + 0.5), 0, 100);
            }
            tile.sub = in.output_drive_label.empty()
                           ? std::string(QCoreApplication::translate("Diagnostics", "free ").toStdString()) + kMiddot +
                                 QCoreApplication::translate("Diagnostics", " output drive").toStdString()
                           : std::string(QCoreApplication::translate("Diagnostics", "free ").toStdString()) + kMiddot +
                                 " " + in.output_drive_label;
        } else {
            tile.value = kDash;
            tile.sub = QCoreApplication::translate("Diagnostics", "output drive").toStdString();
        }
        tiles.push_back(std::move(tile));
    }

    // Tile 3 — Display: the primary screen's mode, and what is being captured
    // from it. The two used to be separate tiles; a mode and the target it will
    // be recorded from are one answer to one question.
    {
        ReadinessTile tile;
        tile.key = "display";
        tile.title = QCoreApplication::translate("Diagnostics", "Display").toStdString();
        std::string target = in.target_is_window
                                 ? QCoreApplication::translate("Diagnostics", "application window").toStdString()
                                 : QCoreApplication::translate("Diagnostics", "full display").toStdString();
        if (in.target_selected && !BlankOrWhitespace(in.target_description))
            target = in.target_description;
        if (in.display_width > 0 && in.display_height > 0) {
            tile.value = std::to_string(in.display_width) + " \xc3\x97 " + std::to_string(in.display_height);
            tile.sub = Join(std::to_string(in.display_refresh_hz) +
                                QCoreApplication::translate("Diagnostics", " Hz").toStdString(),
                            target);
        } else {
            tile.value = kDash;
            tile.sub = target;
        }
        tiles.push_back(std::move(tile));
    }

    // Tile 4 — Audio. A plain capability readout, never coloured as a problem.
    {
        ReadinessTile tile;
        tile.key = "audio";
        tile.title = "Audio";
        if (in.data_ready) {
            const std::string codec = StripBackendSuffix(AudioCodecDisplayName(in.audio_codec));
            tile.value = codec.empty() ? std::string(kDash) : codec;
            if (in.audio_sources == 0) {
                tile.sub = std::string(QCoreApplication::translate("Diagnostics", "no sources ").toStdString()) +
                           kMiddot + QCoreApplication::translate("Diagnostics", " silent").toStdString();
            } else {
                // Non-breaking space between the number and its unit: word-wrap must
                // never split "48 kHz" across two lines inside the tile subline.
                const std::string rate = Number(static_cast<double>(in.audio_sample_rate) / 1000.0, 1);
                std::string trimmed = rate;
                if (trimmed.size() > 2 && trimmed.compare(trimmed.size() - 2, 2, ".0") == 0)
                    trimmed.resize(trimmed.size() - 2);
                const std::string channels = in.audio_channels <= 1 ? "Mono" : "Stereo";
                tile.sub =
                    std::to_string(in.audio_sources) +
                    (in.audio_sources == 1 ? QCoreApplication::translate("Diagnostics", " source ").toStdString()
                                           : QCoreApplication::translate("Diagnostics", " sources ").toStdString()) +
                    kMiddot + " " + trimmed + kNarrowNbsp +
                    QCoreApplication::translate("Diagnostics", "kHz ").toStdString() + kMiddot + " " + channels;
            }
        } else {
            tile.value = kDash;
            tile.sub = QCoreApplication::translate("Diagnostics", "audio sources").toStdString();
        }
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

    const auto planned = [&](const char* key, const std::string& title, const std::string& tip) {
        PipelineStage stage;
        stage.key = key;
        stage.title = title;
        stage.lane = kDash;
        stage.value = kDash;
        stage.tip = tip;
        stage.status = StageStatus::Planned;
        stages.push_back(std::move(stage));
    };

    planned("capture", QCoreApplication::translate("Diagnostics", "Source capture").toStdString(),
            QCoreApplication::translate("Diagnostics", "Live during recording.").toStdString());
    planned("queue", QCoreApplication::translate("Diagnostics", "Frame queue").toStdString(),
            QCoreApplication::translate("Diagnostics", "Live during recording.").toStdString());
    planned("compositor", "Compositor",
            QCoreApplication::translate("Diagnostics", "Live during recording.").toStdString());

    if (!data_ready) {
        planned("encoder", "Encoder",
                QCoreApplication::translate("Diagnostics", "Run a check to probe the encoder.").toStdString());
        planned("muxer", "Muxer",
                QCoreApplication::translate("Diagnostics", "Run a check to probe the muxer.").toStdString());
        planned("disk", QCoreApplication::translate("Diagnostics", "Disk").toStdString(),
                QCoreApplication::translate("Diagnostics", "Run a check to probe the output path.").toStdString());
        return stages;
    }

    const auto probed = [&](const char* key, const std::string& title, bool ok, const std::string& ok_tip,
                            const std::string& bad_tip) {
        PipelineStage stage;
        stage.key = key;
        stage.title = title;
        stage.lane = kDash;
        stage.value = kDash;
        stage.tip = ok ? ok_tip : bad_tip;
        stage.status = ok ? StageStatus::Ok : StageStatus::Unavailable;
        stages.push_back(std::move(stage));
    };

    probed("encoder", "Encoder", encoder_ok,
           QCoreApplication::translate("Diagnostics",
                                       "Selected video encoder is available. Live encoder load is not measured.")
               .toStdString(),
           QCoreApplication::translate("Diagnostics", "Selected video codec is not available on this system.")
               .toStdString());
    probed("muxer", "Muxer", muxer_ok,
           QCoreApplication::translate("Diagnostics",
                                       "Selected container muxer is available. Write throughput is not measured.")
               .toStdString(),
           QCoreApplication::translate("Diagnostics", "Selected container is not available on this system.")
               .toStdString());
    probed("disk", QCoreApplication::translate("Diagnostics", "Disk").toStdString(), disk_ok,
           QCoreApplication::translate("Diagnostics", "Output path is writable. Live disk throughput is not measured.")
               .toStdString(),
           QCoreApplication::translate("Diagnostics", "Output path is not writable.").toStdString());
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
    const uint64_t problem_drops = s.real_frame_loss();
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

    const StageSignals stage_signals[] = {capture, queue, comp, enc, mux, disk};
    const exosnap::engine::PipelineHealthVerdict verdict =
        exosnap::engine::ResolvePipelineHealth(stage_signals, budget_ms);

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
        stage.title = QCoreApplication::translate("Diagnostics", "Source capture").toStdString();
        stage.lane = "CPU";
        stage.status = StatusOf(health_of(StageId::SourceCapture));
        stage.value = s.capture.target_fps > 0.0
                          ? Number(s.capture.actual_fps, 1) + " / " + Number(s.capture.target_fps, 1) + " fps"
                          : std::string(kDash);
        stage.tip = (s.capture.acquire_availability == MetricAvailability::Available)
                        ? QCoreApplication::translate("Diagnostics", "Acquire ").toStdString() +
                              Number(s.capture.acquire_average_ms, 2) + " ms (CPU)"
                        : QCoreApplication::translate("Diagnostics", "Acquire timing unavailable for this capture mode")
                              .toStdString();
        stages.push_back(std::move(stage));
    }

    {
        PipelineStage stage;
        stage.key = "queue";
        stage.title = QCoreApplication::translate("Diagnostics", "Frame queue").toStdString();
        stage.lane = kDash;
        stage.status = StatusOf(health_of(StageId::FrameQueue));
        stage.value = s.video_queue.bounded && s.video_queue.capacity > 0
                          ? Number(s.video_queue.current_depth) + " / " + Number(s.video_queue.capacity)
                          : Number(s.video_queue.current_depth);
        stage.tip =
            QCoreApplication::translate("Diagnostics", "Frames waiting between encode and mux (peak ").toStdString() +
            Number(s.video_queue.peak_depth) + ")";
        stages.push_back(std::move(stage));
    }

    {
        PipelineStage stage;
        stage.key = "compositor";
        stage.title = "Compositor";
        stage.lane = "GPU";
        stage.status = StatusOf(health_of(StageId::Compositor));
        stage.value = ms(s.compositor.average_ms, s.compositor.average_ms > 0.0);
        stage.tip = QCoreApplication::translate("Diagnostics",
                                                "CPU submit (GPU execution time not measured in this view). VPBlt ")
                        .toStdString() +
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
        stage.tip = std::string(QCoreApplication::translate("Diagnostics", "CPU submit").toStdString()) + kRightArrow +
                    QCoreApplication::translate("Diagnostics", "ready latency (peak ").toStdString() +
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
        stage.tip = QCoreApplication::translate("Diagnostics", "Mux drain processing (peak ").toStdString() +
                    Number(s.mux.process_peak_ms, 2) + " ms)";
        stages.push_back(std::move(stage));
    }

    {
        PipelineStage stage;
        stage.key = "disk";
        stage.title = QCoreApplication::translate("Diagnostics", "Disk").toStdString();
        stage.lane = "CPU";
        stage.status = StatusOf(health_of(StageId::Disk));
        stage.value = ms(s.disk.average_write_ms, disk.available);
        stage.tip = QCoreApplication::translate("Diagnostics", "Filesystem write-call latency (peak ").toStdString() +
                    Number(s.disk.peak_write_ms, 1) + " ms)";
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
    if (probe.session_generation != live_.session_generation) {
        probe.gpu = {};
        probe.video_memory = {};
    }
    probe_ = std::move(probe);
    if (probe_.self_test_valid)
        self_test_ = BuildSelfTestReport(probe_.self_test);
}

void DiagnosticsController::SetDisplayFacts(DisplayFacts facts) noexcept {
    display_ = facts;
}

void DiagnosticsController::SetCaptureTargetAdapter(RecommendationEngine::CaptureTargetAdapterFacts facts) {
    capture_target_adapter_ = std::move(facts);
}

void DiagnosticsController::SetPresentAttributionPid(unsigned long pid) {
    present_attribution_pid_ = pid;
}

void DiagnosticsController::SetSelectedCaptureTarget(std::optional<exosnap::engine::CaptureTarget> target,
                                                     std::string presented_label) {
    selected_target_ = std::move(target);
    selected_target_label_ = std::move(presented_label);
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
    const bool recording = liveRecording();
    if (!recording)
        pipeline_.Reset();
    // Freeze on the lifecycle edge rather than on the next Evaluate(): the final
    // snapshot carries the elapsed time the last occurrence must be closed at, and
    // a consumer reading the frozen ledger after Stop may never call Evaluate().
    if (was_recording_ && !recording)
        ledger_.Freeze(live_.elapsed_seconds);
    was_recording_ = recording;
}

const SessionLedger& DiagnosticsController::ledger() const noexcept {
    return ledger_;
}

void DiagnosticsController::FreezeLedger() {
    ledger_.Freeze(live_.elapsed_seconds);
}

const std::vector<LedgerEntry>& DiagnosticsController::frozenLedger() const noexcept {
    return ledger_.entries();
}

void DiagnosticsController::SetLastSession(LastSession session) {
    last_session_ = std::move(session);
}

const LastSession& DiagnosticsController::lastSession() const noexcept {
    return last_session_;
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
        out.tiles = BuildReadinessTiles(tile_inputs);
        return out;
    }

    const exosnap::engine::RecordingDiagnosticsSnapshot* live = live_.valid ? &live_ : nullptr;
    // Present samples are attributed to a process only for a window target; for a
    // display or region the accumulator saw every process on the machine.
    PresentSample present_sample;
    const PresentSample* present = nullptr;
    if (present_.has_value() && present_->available) {
        present_sample = *present_;
        // Attributed when the accumulator filtered to a process: the captured
        // window's, or for a display the process presenting on that display.
        present_sample.attributed = present_attribution_pid_ != 0;
        present = &present_sample;
    }

    RecommendationEngine engine(config_.caps, config_.user_config, probe_.free_bytes,
                                config_.profile_validation.succeeded, probe_.filesystem_name, live, present);
    if (dpc_.has_value())
        engine.SetDpcLatency(*dpc_);
    engine.SetOutputPathWritable(probe_.output_path_writable);
    engine.SetElevated(elevated_);
    engine.SetCaptureTargetHdrActive(capture_target_hdr_active_);
    if (probe_.session_generation == live_.session_generation) {
        const auto now = std::chrono::steady_clock::now();
        engine.SetGpuEvidence(
            probe_.gpu.metadata.FreshForAdapter(live_.encoder_adapter_luid, now) ? probe_.gpu : GpuTelemetryReading{},
            probe_.video_memory.metadata.FreshForAdapter(live_.encoder_adapter_luid, now) ? probe_.video_memory
                                                                                          : VideoMemoryReading{});
    }
    engine.SetCaptureTargetAdapter(capture_target_adapter_);
    engine.SetOutputDriveKind(probe_.drive_kind);
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

    if (liveRecording()) {
        if (live_.session_generation != ledger_.generation())
            ledger_.Reset(live_.session_generation);
        // The entry rule counts consecutive MEASUREMENTS, and Evaluate() is reached
        // from far more than the live rail -- a settings change, a display change or
        // a probe result re-evaluates the same snapshot. Observing it again would
        // let a single spike satisfy the two-evaluation rule on its own, so the pair
        // that identifies a snapshot gates the call.
        const SnapshotStamp stamp{live_.session_generation, live_.elapsed_seconds};
        if (!last_observed_.has_value() || *last_observed_ != stamp) {
            last_observed_ = stamp;
            ledger_.Observe(recommendations.results, live_.elapsed_seconds);
        }
        out.verdict = ComputeRecordingVerdict(recommendations, ledger_, live_.elapsed_seconds);
        out.verdict.cap_passes = cap_passes;
    } else {
        // After Stop the band returns to readiness and says nothing about the
        // session; the frozen ledger below is where the session's story lives.
        out.verdict = ComputeVerdict(recommendations, cap_passes, true);
    }
    out.ledger = ledger_.entries();
    out.ledger_active = ledger_.activeCount();

    ReadinessTileInputs tile_inputs;
    tile_inputs.data_ready = true;
    tile_inputs.gpu_adapter_name = config_.caps.gpu_adapter_name;
    tile_inputs.caps = &config_.caps;
    tile_inputs.driver_version =
        VendorDriverVersion(config_.caps.runtime.adapter.vendor_id, config_.caps.runtime.adapter.driver_version);
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
        // The presented label when the caller has one -- the raw description is a
        // device path, which is not what the rest of the product calls this target.
        tile_inputs.target_description =
            selected_target_label_.empty() ? selected_target_->description : selected_target_label_;
    } else {
        tile_inputs.target_is_window = config_.audio.target_kind == capability::CaptureTargetKind::Window;
    }
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
