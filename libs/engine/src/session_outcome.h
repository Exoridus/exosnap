#pragma once

// Outcome attribution for a session that never captured a video frame --
// extracted out of RecorderSession::Record() so it can be unit tested without
// capture hardware, the same pattern session_stop_reset.h uses for the
// fresh-session stop reset.
//
// Such a session leaves no failure recorded: every worker stops on request
// without complaint, and the one that DOES miss something it needed (the mux,
// waiting for codec headers it will never get) is a consumer of the capture,
// not its cause. Before this rule, the first of those consumers latched the
// session outcome and a recording that captured nothing was reported as a mux
// problem -- pointing the reader at the wrong end of the pipeline.
//
// The two causes are told apart because they ask different things of the user:
// a stop that arrived before capture started is the user's own action and
// nothing is wrong with the machine, while a capture that opened and then
// delivered nothing is a real capture-side fault worth reporting as one.

#include <exosnap/engine/error_types.h>
#include <exosnap/engine/recorder_session.h>

#include <winerror.h> // E_ABORT / E_FAIL

#include <cstdint>
#include <string>

namespace exosnap::engine {

enum class MissingCaptureCause {
    None,                 // Frames were captured, or the session already knows its cause.
    StoppedBeforeCapture, // A stop reached the session before its capture produced anything.
    NoFramesDelivered,    // Capture ran but never handed over a frame.
};

[[nodiscard]] constexpr MissingCaptureCause ClassifyMissingCapture(bool has_recorded_cause, uint64_t frames_captured,
                                                                   bool stopped_before_start) noexcept {
    if (has_recorded_cause || frames_captured > 0)
        return MissingCaptureCause::None;
    return stopped_before_start ? MissingCaptureCause::StoppedBeforeCapture : MissingCaptureCause::NoFramesDelivered;
}

// Name of the backend a target kind records with, for the message below.
[[nodiscard]] inline const char* CaptureBackendName(CaptureTarget::Kind kind) noexcept {
    return kind == CaptureTarget::Kind::Monitor ? "DXGI desktop duplication" : "Windows Graphics Capture";
}

// Writes the cause into `result`. A no-op for MissingCaptureCause::None, so the
// caller can apply it unconditionally.
inline void ApplyMissingCaptureOutcome(RecorderResult& result, MissingCaptureCause cause,
                                       CaptureTarget::Kind target_kind) {
    switch (cause) {
    case MissingCaptureCause::None:
        return;
    case MissingCaptureCause::StoppedBeforeCapture:
        result.succeeded = false;
        result.error_code = E_ABORT;
        result.error_phase = ErrorPhase::Prepare;
        result.error_detail = "The recording was stopped before capture started; nothing was recorded.";
        return;
    case MissingCaptureCause::NoFramesDelivered:
        result.succeeded = false;
        result.error_code = E_FAIL;
        result.error_phase = ErrorPhase::VideoCapture;
        result.error_detail = std::string("The capture delivered no frames before the recording stopped (backend: ") +
                              CaptureBackendName(target_kind) + ").";
        return;
    }
}

} // namespace exosnap::engine
