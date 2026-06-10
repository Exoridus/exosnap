#pragma once

#include <optional>
#include <string>

#include <capability/audio_ui_state.h>

namespace exosnap {

// ---------------------------------------------------------------------------
// AudioSourcePresentationState
// ---------------------------------------------------------------------------
// Derived, immutable description of one audio source row's final UI state.
// All fields are pre-computed — no widget need perform its own derivation.

struct AudioSourcePresentationState {
    bool visible = false;          // Row is shown (target-kind policy)
    bool available = false;        // Source is present in the audio plan
    bool enabled = false;          // User's toggle state (from AudioSourceRow)
    bool controls_enabled = false; // Final: visible && available && !controls_locked
    bool separate_track = false;   // "Separate track" / !merge_with_above

    [[nodiscard]] bool operator==(const AudioSourcePresentationState& o) const noexcept {
        return visible == o.visible && available == o.available && enabled == o.enabled &&
               controls_enabled == o.controls_enabled && separate_track == o.separate_track;
    }
    [[nodiscard]] bool operator!=(const AudioSourcePresentationState& o) const noexcept {
        return !(*this == o);
    }
};

// ---------------------------------------------------------------------------
// AudioConfigurationSnapshot
// ---------------------------------------------------------------------------
// Complete structural state of the audio settings card.
// Derived from (AudioUiState + controls_locked) by PresentationStateBuilder.
// Does NOT include meter values — those are delivered via AudioMeterSnapshot.
//
// Equality comparison supports deduplication: if the snapshot has not changed
// since the last apply, the consumer can skip the widget update.

struct AudioConfigurationSnapshot {
    capability::CaptureTargetKind target_kind = capability::CaptureTargetKind::Display;
    bool controls_locked = false;

    AudioSourcePresentationState system;
    AudioSourcePresentationState app;
    AudioSourcePresentationState mic;

    // Selected mic device ID for combo restoration.
    std::optional<std::string> selected_mic_device_id;

    [[nodiscard]] bool operator==(const AudioConfigurationSnapshot& o) const noexcept {
        return target_kind == o.target_kind && controls_locked == o.controls_locked && system == o.system &&
               app == o.app && mic == o.mic && selected_mic_device_id == o.selected_mic_device_id;
    }
    [[nodiscard]] bool operator!=(const AudioConfigurationSnapshot& o) const noexcept {
        return !(*this == o);
    }
};

// ---------------------------------------------------------------------------
// AudioMeterSnapshot
// ---------------------------------------------------------------------------
// Lightweight high-cadence (~30 Hz) per-source RMS values.
// Carries NO structural state — applying a meter snapshot must never change
// widget visibility or the enabled state of any control.

struct AudioMeterSnapshot {
    float sys01 = 0.0f; // 0.0 = silence / inactive, 1.0 = peak
    float app01 = 0.0f;
    float mic01 = 0.0f;
    bool sys_active = false; // meter service is running for this source
    bool app_active = false;
    bool mic_active = false;
};

} // namespace exosnap
