#pragma once

#include "OutputSettingsModel.h"
#include "StableDisplayId.h"
#include "VideoSettingsModel.h"
#include "WebcamSettings.h"

#include <capability/audio_ui_state.h>
#include <recorder_core/audio_track_model.h>
#include <recorder_core/recorder_session.h>

#include <string>
#include <string_view>
#include <vector>

namespace exosnap {

// ---------------------------------------------------------------------------
// Schema version — bump when the persisted format changes incompatibly.
//
// v25: the capture target's monitor identity moves from the unstable GDI device
// name to a hardware-stable StableDisplayId sub-table (device path + EDID). The
// [capture] string keys display_key/region_display_key are replaced by
// [capture.display_id]/[capture.region_display_id] sub-tables, and the region is
// stored anchor-relative (region_{x,y,w,h}_norm) instead of absolute virtual-
// screen pixels. Pre-1.0, older files simply lack the sub-tables: the field-wise
// repair leaves the identity empty ("no preference"), the saved display target is
// dropped once, and the next save writes the new stable form. Not reported as an
// error (only a real parse failure is). See ADR 0047.
//
// v24: adds audio.pcm_float (bool) -- 32-bit float PCM (A_PCM/FLOAT_IEEE),
// opt-in only when audio_codec == Pcm and audio_bit_depth == 32. Older
// presets simply default to false (no behavior change).
//
// v23: the store's persisted unit changes from "the preset list plus a
// startup-default id" to "the live configuration plus named snapshots".
// A [live] table holds the config the app is actually running (restored
// verbatim on the next launch); built-in presets are never written to disk
// (they are code-defined and reseeded on every load); default_id is gone —
// there is no more startup-default preset, only a live config and, always,
// the built-in Default preset it can fall back to. Loading no longer resets
// the whole store on a schema mismatch: every field is repaired
// individually (missing/invalid values fall back to their model default).
// A version bump alone is not reported to the user — only an actual parse
// failure or dropped item is; see PersistedPresetState::repaired. This
// subsumes the v19->v20 colour-range exception below, which is now just one
// more field-wise migration rule instead of a special case.
//
// v21: adds output.hdr_mode (Off/TonemapSdr/Hdr10).
//
// v20 (fix/color-range-signaling): default colour range flipped Full ->
// Limited. Schema-19-and-older files rewrite color_range=="full" to
// "limited", because under schema <=19 "full" was the materialized old code
// default, never an informed user choice (the ConfigPage combo had a
// hydration bug and always displayed "Full (PC)" regardless of the model, so
// a deliberate Full selection could not exist). A schema-20-and-newer file
// with explicit "full" is a deliberate post-flip opt-in and is respected.
// See ADR 0032.
// ---------------------------------------------------------------------------
inline constexpr int kPresetSchemaVersion = 25;

// Files at or below this schema get the targeted color_range full->limited
// rewrite (ADR 0032) on top of the ordinary field-wise repair.
inline constexpr int kPresetSchemaColorRangeMigratedThrough = 19;

// Default PiP inset (bottom-right corner), as a fraction of the frame edge.
inline constexpr float kDefaultPipInsetNorm = 0.03f;

// Stable ids for the four shipped read-only built-in presets.
inline constexpr std::string_view kDefaultPresetId = "preset.default";
inline constexpr std::string_view kQualityPresetId = "preset.quality";
inline constexpr std::string_view kEfficiencyPresetId = "preset.efficiency";
inline constexpr std::string_view kCompatibilityPresetId = "preset.compatibility";

// ---------------------------------------------------------------------------
// PresetCaptureKind
// ---------------------------------------------------------------------------

enum class PresetCaptureKind {
    Display,
    Window,
    Region,
};

// ---------------------------------------------------------------------------
// PresetCaptureTarget
// ---------------------------------------------------------------------------

// Stores the capture source selected at the time the preset was saved. Raw
// platform handles (HWND, HMONITOR) are never stored — monitors are identified
// by a hardware-stable StableDisplayId (device path + EDID) resolved at restore
// time by the ranked DisplayIdentityResolver; windows stay description-based
// (there is no hardware-stable window identity — out of scope). An empty()
// display_id means "no stored preference" (primary/any monitor).
//
// The region is stored ANCHOR-RELATIVE: normalized [0,1] fractions of the anchor
// display's physical rect, so a resolution change carries it proportionally.
struct PresetCaptureTarget {
    PresetCaptureKind kind = PresetCaptureKind::Display;

    StableDisplayId display_id; // Monitor identity (hardware-stable)
    std::string window_key;     // Window/app identity (description-based)

    bool has_region = false;
    StableDisplayId region_display_id; // Anchor display the region belongs to
    float region_x_norm = 0.0f;        // Region rect as [0,1] fractions of the
    float region_y_norm = 0.0f;        // anchor display's physical rcMonitor.
    float region_w_norm = 0.0f;
    float region_h_norm = 0.0f;
};

// ---------------------------------------------------------------------------
// RecordingPresetConfig
// ---------------------------------------------------------------------------

struct RecordingPresetConfig {
    PresetCaptureTarget capture;
    OutputSettingsModel output;
    VideoSettingsModel video;
    capability::AudioUiState audio;
    WebcamSettings webcam;
    int countdown_seconds = 0; // One of {0, 3, 5, 10}
};

// ---------------------------------------------------------------------------
// RecordingPreset
// ---------------------------------------------------------------------------

struct RecordingPreset {
    std::string id;   // Stable unique id, e.g. "preset.<hex16>"
    std::string name; // User-visible label; NOT the identity key

    RecordingPresetConfig config;
};

// ---------------------------------------------------------------------------
// Factory / generation
// ---------------------------------------------------------------------------

// Returns the canonical default preset (MKV + AV1 + Opus, quality=High, …).
[[nodiscard]] RecordingPreset MakeDefaultPreset();

// The four read-only shipped presets, Default first. None of them sets an
// environment field (capture / bit_depth / hdr_mode stay at model defaults).
[[nodiscard]] std::vector<RecordingPreset> MakeBuiltInPresets();

// True when `id` names one of the shipped read-only presets.
[[nodiscard]] bool IsBuiltInPresetId(std::string_view id);

// Returns a stable unique id with prefix "preset." followed by 16 hex chars.
// Never equals any built-in id (the built-in suffixes are not 16 hex chars).
[[nodiscard]] std::string GeneratePresetId();

// ---------------------------------------------------------------------------
// Codec-container reconciliation
// ---------------------------------------------------------------------------

// Reconciles the codec fields of `output` to be valid for its container.
// Rules (delegated to ContainerCompatRegistry::ReconcileCodecs, ADR 0010):
//   MP4  → H.264 (Recommended) or HEVC (Allowed via hvc1 remux, 0.7.0) kept;
//           AV1 is deferred → falls back. Audio forced to AacMf (Opus/PCM/FLAC
//           Prohibited or deferred in MP4 — ADR 0010/0030).
//   WebM → Av1Nvenc + Opus (forced); AAC/PCM/FLAC and H.264/HEVC Prohibited.
//   MKV  → AV1/H.264/HEVC + Opus/AAC/PCM/FLAC all Allowed or Recommended.
void ReconcileContainerCodecs(OutputSettingsModel& output);

// ---------------------------------------------------------------------------
// Sanitization
// ---------------------------------------------------------------------------

// Sanitizes all fields in `config`.  See implementation for per-field rules.
[[nodiscard]] RecordingPresetConfig SanitizePresetConfig(RecordingPresetConfig config);

// Trims name; assigns "Untitled preset" when empty after trim.
// Ensures a non-empty id (generates one if absent).
// Sanitizes config.
[[nodiscard]] RecordingPreset SanitizePreset(RecordingPreset preset);

// ---------------------------------------------------------------------------
// Name helpers
// ---------------------------------------------------------------------------

// Returns true iff the name is non-empty after trimming ASCII whitespace.
[[nodiscard]] bool IsValidPresetName(std::string_view name);

// Returns a trimmed copy of `name`.
[[nodiscard]] std::string NormalizePresetName(std::string_view name);

// Trim + ASCII-lowercase fold for name uniqueness ("streaming" == "Streaming ").
// Non-ASCII case folding is intentionally not attempted.
[[nodiscard]] std::string FoldPresetName(std::string_view name);

// ---------------------------------------------------------------------------
// Semantic equality (dirty-state comparison)
// ---------------------------------------------------------------------------

// Returns true when `a` and `b` are semantically identical (i.e. NOT dirty).
// Floating-point fields use tolerances (see implementation for details).
[[nodiscard]] bool NormalizedConfigEquals(const RecordingPresetConfig& a, const RecordingPresetConfig& b);

// Returns true when `a` and `b` are dirty-equivalent — i.e. the user has NOT
// made meaningful changes.  Identical to NormalizedConfigEquals EXCEPT that the
// capture sub-struct (kind, display_id, window_key, has_region, region norms,
// region_display_id) is NOT compared.  Capture identity is transient: it
// depends on device availability and on auto-resolution (the default preset
// stores an empty display_id meaning "primary/any", but once applied the live
// policy holds a concrete resolved identity).  Comparing capture would cause the
// preset to appear spuriously dirty on startup, on monitor replug, etc.
// Per spec: temporary availability changes must not make the preset dirty.
// NOTE: NormalizedConfigEquals is kept for persistence round-trip verification
// and must NOT be changed.
[[nodiscard]] bool ConfigDirtyEquivalent(const RecordingPresetConfig& a, const RecordingPresetConfig& b);

// ---------------------------------------------------------------------------
// Environment fields
// ---------------------------------------------------------------------------

// Environment fields describe the machine/display, not the user's recording
// intent: capture identity, video bit depth, HDR handling. Presets neither
// set nor override them, and they never count toward the (changed) state.

// Returns `config` with the environment fields copied from `env` — used when
// applying a preset so a switch never overrides the live environment.
[[nodiscard]] RecordingPresetConfig WithEnvironmentFields(RecordingPresetConfig config,
                                                          const RecordingPresetConfig& env);

// Returns `config` with the environment fields reset to model defaults —
// used when snapshotting the live config into a named preset.
[[nodiscard]] RecordingPresetConfig StripEnvironmentFields(RecordingPresetConfig config);

// ---------------------------------------------------------------------------
// Filename token helpers (previously in RecordingProfile.h)
// ---------------------------------------------------------------------------

// Returns a short wstring token for use in filename patterns, e.g. "mkv".
[[nodiscard]] std::wstring ContainerToken(capability::Container container);

// Returns a short wstring token for a video or audio codec, e.g. "h264".
[[nodiscard]] std::wstring CodecToken(capability::VideoCodec codec);
[[nodiscard]] std::wstring CodecToken(capability::AudioCodec codec);

} // namespace exosnap
