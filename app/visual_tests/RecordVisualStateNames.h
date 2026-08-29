#pragma once

// RecordVisualStateNames.h -- the accepted values of --record-visual-state.
//
// Deliberately tiny and dependency-free (no Qt, no engine): the shipping
// frontend's scenario branches and the visual-scenario catalogue both include
// it, and a shared name is the only thing that stops the two from disagreeing
// about what a scenario is called.
//
// That disagreement is not hypothetical. `record-recording-audio-degraded` was
// in the catalogue with an `audio_degraded_notification_count` the Widgets
// harness read out of the struct; when the Widgets shell was removed the field
// lost its only consumer, and the Quick harness -- which switches on the
// scenario STRING -- had no branch for it. The scenario stayed in the catalogue,
// stayed listed in the manifest, and rendered an ordinary recording.

namespace exosnap::visual::record_state {

inline constexpr const char* kReady = "ready";
inline constexpr const char* kRecording = "recording";
// Recording, with the standing "an audio source went silent" notification
// raised through the production notification path.
inline constexpr const char* kRecordingAudioDegraded = "recording-audio-degraded";
inline constexpr const char* kCountdown = "countdown";
inline constexpr const char* kPreparing = "preparing";
inline constexpr const char* kPaused = "paused";
inline constexpr const char* kCompleted = "completed";
inline constexpr const char* kBlocked = "blocked";
inline constexpr const char* kFailed = "failed";
inline constexpr const char* kUnavailable = "unavailable";
inline constexpr const char* kOutputUnwritable = "output-unwritable";

} // namespace exosnap::visual::record_state
