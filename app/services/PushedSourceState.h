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
    bool active = false;      // a shared handle is open; the pushed source is the background
    bool has_frame = false;   // >= 1 pushed frame copied into the local present target
    bool wgc_stopped = false; // the preview's own WGC capture graph has been closed
    // True when the pushed frames are RAW captures (an idle DXGI-hub source):
    // they carry neither the cursor nor the webcam PiP, so the renderer draws
    // both itself. False for the engine's recording frames, which arrive with
    // the overlays baked in exactly as encoded.
    bool raw_source = false;

    void OnSourceOpened(bool raw_source_frames) noexcept {
        active = true;
        has_frame = false;
        raw_source = raw_source_frames;
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
        raw_source = false;
    }

    // The pushed frame is the background this present tick. Until the first pushed
    // frame lands the renderer holds its last WGC image instead — no black flash
    // on countdown.
    [[nodiscard]] bool DrawsPushedBackground() const noexcept {
        return active && has_frame;
    }
    // Poll the WGC frame pool only while a pushed source is NOT active.
    [[nodiscard]] bool PollsWgc() const noexcept {
        return !active;
    }
    // Draw the renderer's OWN webcam overlay unless the background is an engine
    // frame with the PiP already baked in (drawing it again would double it).
    // A raw pushed background carries no PiP, so the renderer keeps drawing its
    // own — same as during the countdown hold.
    [[nodiscard]] bool DrawsWebcamOverlay() const noexcept {
        return !DrawsPushedBackground() || raw_source;
    }
    // Draw the renderer's own cursor sprite only over a raw pushed background:
    // engine frames carry the recorded cursor, and the WGC preview's frames
    // carry the cursor WGC composites itself.
    [[nodiscard]] bool DrawsCursorSprite() const noexcept {
        return DrawsPushedBackground() && raw_source;
    }
    // Stop the preview's own WGC capture graph exactly once, when pushed mode first
    // becomes active (no second capture running alongside the engine).
    [[nodiscard]] bool ShouldStopWgcGraph() const noexcept {
        return active && !wgc_stopped;
    }
    // When recording ends the preview reverts to its own WGC capture. The graph must
    // be rebuilt only if it was actually torn down when pushed mode began. If the
    // shared handle never opened (e.g. cross-GPU OpenSharedResource1 failure) the WGC
    // graph was never stopped and is still running — there is nothing to rebuild.
    [[nodiscard]] bool NeedsWgcRebuildOnRevert() const noexcept {
        return wgc_stopped;
    }
};

} // namespace exosnap
