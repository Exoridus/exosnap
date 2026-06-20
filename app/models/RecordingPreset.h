#pragma once

#include "OutputSettingsModel.h"
#include "VideoSettingsModel.h"
#include "WebcamSettings.h"

#include <capability/audio_ui_state.h>
#include <recorder_core/audio_track_model.h>
#include <recorder_core/recorder_session.h>

#include <string>
#include <string_view>

namespace exosnap {

// ---------------------------------------------------------------------------
// Schema version — bump when the persisted format changes incompatibly.
// ---------------------------------------------------------------------------
inline constexpr int kPresetSchemaVersion = 8;

// Default PiP inset (bottom-right corner), as a fraction of the frame edge.
inline constexpr float kDefaultPipInsetNorm = 0.03f;

// Stable id for the single built-in default preset.
inline constexpr std::string_view kDefaultPresetId = "preset.default";

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

// Stores best-effort stable identities for the capture source selected at the
// time the preset was saved.  Raw platform handles (HWND, HMONITOR) are never
// stored here — the keys are description-based and matched at restore time.
// An empty key means "no stored preference".
struct PresetCaptureTarget {
    PresetCaptureKind kind = PresetCaptureKind::Display;

    std::string display_key; // Monitor identity (description-based)
    std::string window_key;  // Window/app identity (description-based)

    bool has_region = false;
    recorder_core::CaptureRegion region{}; // Virtual-screen coords
    std::string region_display_key;        // Monitor the region belongs to
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

// Returns a stable unique id with prefix "preset." followed by 16 hex chars.
// Guaranteed to never equal kDefaultPresetId.
[[nodiscard]] std::string GeneratePresetId();

// ---------------------------------------------------------------------------
// Codec-container reconciliation
// ---------------------------------------------------------------------------

// Reconciles the codec fields of `output` to be valid for its container.
// Rules:
//   MP4  → H264Nvenc + AacMf (forced).
//   WebM → Av1Nvenc  + Opus  (forced).
//   MKV  → if HevcNvenc → H264Nvenc;
//           if H264Nvenc + Opus → AacMf;
//           if Pcm → AacMf.
// MKV + AV1 + Opus is left unchanged (it is a valid combination).
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

// ---------------------------------------------------------------------------
// Semantic equality (dirty-state comparison)
// ---------------------------------------------------------------------------

// Returns true when `a` and `b` are semantically identical (i.e. NOT dirty).
// Floating-point fields use tolerances (see implementation for details).
[[nodiscard]] bool NormalizedConfigEquals(const RecordingPresetConfig& a, const RecordingPresetConfig& b);

// Returns true when `a` and `b` are dirty-equivalent — i.e. the user has NOT
// made meaningful changes.  Identical to NormalizedConfigEquals EXCEPT that the
// capture sub-struct (kind, display_key, window_key, has_region, region,
// region_display_key) is NOT compared.  Capture identity is transient: it
// depends on device availability and on auto-resolution (the default preset
// stores an empty display_key meaning "primary/any", but once applied the live
// policy holds a concrete resolved key).  Comparing capture would cause the
// preset to appear spuriously dirty on startup, on monitor replug, etc.
// Per spec: temporary availability changes must not make the preset dirty.
// NOTE: NormalizedConfigEquals is kept for persistence round-trip verification
// and must NOT be changed.
[[nodiscard]] bool ConfigDirtyEquivalent(const RecordingPresetConfig& a, const RecordingPresetConfig& b);

// ---------------------------------------------------------------------------
// Filename token helpers (previously in RecordingProfile.h)
// ---------------------------------------------------------------------------

// Returns a short wstring token for use in filename patterns, e.g. "mkv".
[[nodiscard]] std::wstring ContainerToken(capability::Container container);

// Returns a short wstring token for a video or audio codec, e.g. "h264".
[[nodiscard]] std::wstring CodecToken(capability::VideoCodec codec);
[[nodiscard]] std::wstring CodecToken(capability::AudioCodec codec);

} // namespace exosnap
