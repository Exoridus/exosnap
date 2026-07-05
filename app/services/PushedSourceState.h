#pragma once

namespace exosnap {

// Pure state machine for the DXGI preview's "pushed source" mode — the WYSIWYG
// preview that samples the engine's shared, pre-encode frame during recording.
// It holds NO GPU resources and touches NO threads: DxgiPreviewRenderer owns the
// real D3D handles and drives this to decide what to do each render iteration.
// Extracted so the switch-over decisions are unit-testable without a live device.
//
// Lifecycle across one recording:
//   WGC preview running (not active)
//     -> engine handle opened on the render device        : OnSourceOpened()
//     -> preview's own WGC capture graph closed (once)     : OnWgcGraphStopped()
//     -> first engine frame copied into the local texture  : OnFrameConsumed()
//     -> recording stops / resources released              : Reset()
struct PushedSourceState {
    bool active = false;      // a shared engine handle is open; engine is the source
    bool has_frame = false;   // >= 1 engine frame copied into the local present target
    bool wgc_stopped = false; // the preview's own WGC capture graph has been closed

    void OnSourceOpened() noexcept {
        active = true;
        has_frame = false;
    }
    void OnWgcGraphStopped() noexcept {
        wgc_stopped = true;
    }
    void OnFrameConsumed() noexcept {
        has_frame = true;
    }
    void Reset() noexcept {
        active = false;
        has_frame = false;
        wgc_stopped = false;
    }

    // The engine frame is the background this present tick (it already contains the
    // cursor + webcam PiP exactly as recorded). Until the first engine frame lands
    // the renderer holds its last WGC image instead — no black flash on countdown.
    [[nodiscard]] bool DrawsPushedBackground() const noexcept {
        return active && has_frame;
    }
    // Poll the WGC frame pool only while the engine is NOT the source.
    [[nodiscard]] bool PollsWgc() const noexcept {
        return !active;
    }
    // Draw the renderer's OWN webcam overlay everywhere EXCEPT when a pushed engine
    // frame is the background (that frame already has the PiP — drawing it again
    // would double it). The overlay still draws during the countdown hold.
    [[nodiscard]] bool DrawsWebcamOverlay() const noexcept {
        return !DrawsPushedBackground();
    }
    // Stop the preview's own WGC capture graph exactly once, when pushed mode first
    // becomes active (no second capture running alongside the engine).
    [[nodiscard]] bool ShouldStopWgcGraph() const noexcept {
        return active && !wgc_stopped;
    }
};

} // namespace exosnap
