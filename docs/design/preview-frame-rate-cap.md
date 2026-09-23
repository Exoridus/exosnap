# Preview frame-rate cap

**Status: Accepted, not implemented.** This document owns only the proposed preview preference. The current preview is producer-driven as described in [capture and preview](../architecture/capture-and-preview.md); the preference below is not a shipped setting.

## User contract

Add a **Preview frame rate** choice under Settings > Video quality & timing: Off, 15, 30, 60 or 120 fps, default 60. Keep values visible but unavailable when the display-derived cap cannot support them, with the same honest reason presentation as the recording-rate control.

The preference affects presentation only, both before and during recording. Recording rate, timestamp selection, encoder behavior and file output are unchanged. A preview capped below recording rate is a sampled view, not proof that frames are missing from the file.

Off stops preview rendering and its idle capture subscription, with a static **Preview off** placeholder. During recording, it does not stop the recording producer or the existing pre-encode tap. The preview may not show framing or PiP placement while Off; that consequence must be explicit.

## Ownership and implementation constraints

Store one global application preference, not a recording preset field: `0` for Off, otherwise 15/30/60/120, default 60. Use the current settings store and typed Quick adapter; no page-local duplicate authority. Validate load/import values and preserve the setting across preset switches.

Apply the limit at the Quick preview scheduling/consumption boundary. Do not introduce a periodic redraw loop that wakes a still desktop. Producer notifications remain edge-driven and coalesced. If a rate deadline delays a published frame, retain presentation debt and arrange one bounded deadline wakeup rather than dropping the edge forever. Expose, screen change and scene-graph recreation must still present the newest owed frame when eligible.

Off releases preview-only ownership without taking a recording lease away. Re-enabling establishes a new valid subscription/device-generation view and cannot resurrect a stale shared handle. Changes must work in idle and engine-fed preview modes without restarting the recording. Native capture/color/encoder policy remains outside QML.

No exact renderer method names or retired frontend call chain are prescribed here. Integrate with the current scheduling and lease owners rather than recreating a separate preview thread or a second capture during recording.

## Acceptance

Test every stored choice, invalid-value fallback, display relevance and preset independence. For each preview mode, prove a cap changes only presentation count and not recorded frames/PTS; Off creates no preview-only capture/render work. Verify re-enable, source change, hidden/exposed window, cross-monitor movement and scene-graph recreation with a pending frame.

Measure idle wakeups and representative capture load, not just an FPS label. A missing second producer frame must never leave the first frame pending forever. Once implemented, move the user contract to the product specification, scheduling invariants to architecture and operational measurement to the harness guide, then remove this design document.
