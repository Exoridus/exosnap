#pragma once

// Device-loss policy for WASAPI audio capture sources (ADR 0046).
//
// Unlike the original fail-closed policy (ClassifyWasapiAcquireFailure -> Fail
// ends the whole recording), an audio endpoint lost mid-recording no longer
// takes the session down with it. The affected *source* is degraded to honest
// silence, the recording keeps running (video and every other audio source are
// untouched), and the engine throttled-reactivates the source with the same
// identity. This mirrors the video (Output Duplication held-frame + reopen) and
// webcam (frozen-frame + reopen) paths so every accessory source degrades
// gracefully instead of only the audio one being session-fatal.
//
// Two pure decisions live here so the policy is unit-pinned and hardware-free:
//   1. ClassifyAudioSourceLoss(hr): is a reported acquire failure a benign
//      no-data tick or a recoverable device loss that must degrade the source?
//   2. DecideAudioDeviceLoss(...): after a reactivation attempt, continue live
//      or wait and retry? Unbounded — a degraded source keeps retrying at the
//      poll cadence forever (matching the video reopen loop's std::nullopt
//      budget); honest silence never gets worse by waiting, so there is no
//      give-up-and-kill branch.

#include <Audioclient.h>
#include <windows.h>

#include <chrono>
#include <cstdint>

namespace exosnap::engine {

// How a reported source acquire failure translates into the session's reaction.
enum class AudioLossReaction {
    KeepDraining,  // benign "no data this tick" — not a failure, keep going.
    DegradeSource, // recoverable endpoint loss: silence THIS source, keep
                   // recording, throttled-reactivate. Never kills the session.
};

// Classify the HRESULT the source reported for a failed acquire. Consulted only
// AFTER a source has already returned a failure (AcquireBuffer == false with a
// non-empty message / a fatal LastCaptureHresult), so the only benign codes are
// the "no packet this tick" ones; everything else — a real device-loss HRESULT
// (AUDCLNT_E_DEVICE_INVALIDATED / _SERVICE_NOT_RUNNING), an unexpected HRESULT,
// or a failure a source surfaced by message only (hr left at 0/E_FAIL) —
// degrades the source rather than ending the recording.
[[nodiscard]] inline AudioLossReaction ClassifyAudioSourceLoss(int32_t hr) noexcept {
    switch (hr) {
    case AUDCLNT_S_BUFFER_EMPTY:
        return AudioLossReaction::KeepDraining;
    default:
        return AudioLossReaction::DegradeSource;
    }
}

enum class AudioReactivateAction {
    Reactivated, // Reinit succeeded — the source is live again.
    RetryAfter,  // still down — stay silent, retry after retry_delay.
};

struct AudioReactivateDecision {
    AudioReactivateAction action = AudioReactivateAction::RetryAfter;
    std::chrono::milliseconds retry_delay{0};
};

// Reactivation cadence for a degraded audio source. Unbounded on purpose (see
// header): success wins immediately; otherwise wait the poll delay and retry,
// forever. 500 ms matches the webcam reconnect cadence.
inline constexpr std::chrono::milliseconds kAudioReactivatePollDelay{500};

[[nodiscard]] inline AudioReactivateDecision
DecideAudioDeviceLoss(bool reactivated, std::chrono::milliseconds /*elapsed_since_last_attempt*/,
                      std::chrono::milliseconds poll_delay) noexcept {
    if (reactivated) {
        return {AudioReactivateAction::Reactivated, std::chrono::milliseconds{0}};
    }
    return {AudioReactivateAction::RetryAfter, poll_delay};
}

} // namespace exosnap::engine
