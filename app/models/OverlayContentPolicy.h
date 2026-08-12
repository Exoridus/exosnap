#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

namespace exosnap::models {

// What the on-screen recording and diagnostics overlays are allowed to show.
//
// WHY THIS IS A POLICY AND NOT QML STATE
// --------------------------------------
// Two frontends, a persisted setting and a runtime metric feed all meet here. If
// the element set were assembled in the delegate, "which tokens does Technical
// contain" would exist once per surface and drift the moment one of them gained
// a token. Everything below is pure: no Qt widgets, no adapters, no engine.
//
// THE PRODUCER RULE
// -----------------
// Every element in the two enums below names a value with a measured runtime
// producer, verified against the source rather than assumed:
//
//   Elapsed        RecordViewModel::elapsed_text        (session clock)
//   OutputSize     RecordViewModel::output_size_text    (SessionStats video+audio bytes)
//   SourceName     RecordViewModel::source_name         (resolved capture target)
//   Fps            frames_captured / elapsed_seconds    (SessionStats::video_frames_captured)
//   Drop           RecordViewModel::dropped_frames      (diagnostics capture.frames_dropped_problem)
//   Drift          RecordViewModel::av_drift_ms         (audio device clock vs QPC timeline)
//   Size           RecordViewModel::output_size_text    (as above)
//   MutedSources   RecordViewModel::audio_active_{mic,sys}
//
// An element with no producer does not belong in these enums — it would put a
// toggle in Settings that can only ever produce a placeholder. This is the
// "ACTIVE PRODUCER + REAL DATA or HONESTLY UNAVAILABLE" rule applied at the
// point where the user chooses.
//
// Availability is a SEPARATE axis and deliberately not modelled here: drift is
// only measurable once the audio clock has been read, so a configured-on drift
// token renders as an em dash until then. "Configured off" and "not measured
// yet" must stay distinguishable, which is why this policy answers only the
// first.
//
// WHAT IS NOT CONFIGURABLE
// ------------------------
// Opacity, corner radius, shadow, colour and placement are design-system values,
// not preferences — they are decided once in ExoTheme and the overlay QML. Click
// -through is not configurable either: it is a correctness property of a window
// that sits over whatever the user is recording.

// ── Recording overlay ────────────────────────────────────────────────────────

// The recording HUD's state indicator is structural — a recording indicator that
// can be switched off is just an empty pill — so it is not an element. These are
// the optional tokens beside it.
enum class RecordingOverlayElement : std::uint8_t {
    Elapsed,
    OutputSize,
    SourceName,
};

enum class RecordingOverlayPreset : std::uint8_t {
    // Dot plus elapsed time. The shipped default: it answers "am I recording,
    // and for how long" and nothing else.
    Minimal,
    Custom,
};

struct RecordingOverlayContent {
    bool elapsed = true;
    bool output_size = false;
    bool source_name = false;

    [[nodiscard]] bool operator==(const RecordingOverlayContent&) const = default;
};

// ── Diagnostics overlay ──────────────────────────────────────────────────────

enum class DiagnosticsOverlayElement : std::uint8_t {
    Fps,
    Drop,
    Drift,
    Size,
    MutedSources,
};

enum class DiagnosticsOverlayPreset : std::uint8_t {
    // Only the tokens that can report a problem. Someone watching the HUD during
    // a session wants to know whether the recording is going wrong, and fps and
    // size never answer that.
    Health,
    // Every measured token.
    Technical,
    Custom,
};

struct DiagnosticsOverlayContent {
    bool fps = false;
    bool drop = true;
    bool drift = true;
    bool size = false;
    bool muted_sources = true;

    // True when nothing at all would be drawn. The overlay hides itself rather
    // than putting an empty pill over the recorded screen.
    [[nodiscard]] bool IsEmpty() const noexcept;

    [[nodiscard]] bool operator==(const DiagnosticsOverlayContent&) const = default;
};

// ── Runtime state of the recording HUD ───────────────────────────────────────

// Three production states, and Hidden.
//
// There is deliberately no Error state. A fatal recording failure is not a HUD
// message: it routes to the recording-error surface, which is a window the user
// can read and act on. Leaving a failure pill over the recorded screen as well
// would put the same event in two places and imply the capture is still live.
enum class RecordingOverlayState : std::uint8_t {
    Hidden,
    Recording,
    Paused,
    // A measured problem during an otherwise RUNNING capture. Never a prediction
    // and never a heuristic — see ResolveRecordingOverlayState.
    Warning,
};

struct RecordingOverlayStateInputs {
    bool recording = false;
    bool paused = false;
    bool failed = false;
    // Real drops only. RecordViewModel sources this from the diagnostics
    // snapshot's frames_dropped_problem(), which already excludes deliberate CFR
    // pacing — a 144 Hz source coalesced to a 60 fps target is not a drop and
    // must not raise a warning.
    std::uint64_t dropped_frames = 0;
    bool live_stats_available = false;
};

// Precedence: failed (Hidden) > Paused > Warning > Recording.
//
// Paused deliberately outranks Warning. The one thing the HUD exists to prevent
// is a user believing they are recording when they are not; a drop count is the
// lesser message and it is still on the diagnostics HUD.
[[nodiscard]] RecordingOverlayState ResolveRecordingOverlayState(const RecordingOverlayStateInputs& inputs);

// ── Persistence ──────────────────────────────────────────────────────────────
//
// Presets persist as their lowercase token; custom sets persist as a
// comma-separated token list ("drop,drift,size"). A token list rather than one
// bool per element keeps the settings file readable and lets an element be added
// without a schema migration. Unknown tokens are ignored, so a downgrade cannot
// fail to load.

[[nodiscard]] QString TokenFor(RecordingOverlayPreset preset);
[[nodiscard]] QString TokenFor(DiagnosticsOverlayPreset preset);
[[nodiscard]] QString TokenFor(RecordingOverlayElement element);
[[nodiscard]] QString TokenFor(DiagnosticsOverlayElement element);

// Unrecognised input resolves to the shipped default rather than failing: a
// settings file is user-editable and a typo must not leave the HUD unconfigured.
[[nodiscard]] RecordingOverlayPreset RecordingOverlayPresetFromToken(const QString& token);
[[nodiscard]] DiagnosticsOverlayPreset DiagnosticsOverlayPresetFromToken(const QString& token);

[[nodiscard]] QString TokensForRecordingOverlayContent(const RecordingOverlayContent& content);
[[nodiscard]] QString TokensForDiagnosticsOverlayContent(const DiagnosticsOverlayContent& content);

// ── Resolution ───────────────────────────────────────────────────────────────
//
// `custom_tokens` is read only when the preset is Custom. A named preset always
// wins over whatever the custom list happens to hold, so switching to Technical
// and back to Custom returns the user's own set unchanged.

[[nodiscard]] RecordingOverlayContent ResolveRecordingOverlayContent(RecordingOverlayPreset preset,
                                                                     const QString& custom_tokens);
[[nodiscard]] DiagnosticsOverlayContent ResolveDiagnosticsOverlayContent(DiagnosticsOverlayPreset preset,
                                                                         const QString& custom_tokens);

} // namespace exosnap::models
