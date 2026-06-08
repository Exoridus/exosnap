# RECORDING-INTERACTION-R1 - Countdown and Editable Region Workflow

## Implementation Notes

- Countdown is handled by `RecordingCountdownController`, a small monotonic-time controller that accepts only 3, 5, and 10 second delays. `Off` remains an immediate start path.
- `RecordPage` owns the transient `InteractionMode` for countdown and region editing. Countdown blocks source/settings changes, changes the transport action to Cancel, and starts the backend only after the controller reaches zero.
- Cancel, Escape, and the record hotkey cancel an active countdown and restore Ready without starting a session.
- Region editing now reopens the selection overlay from the last confirmed region. Dragging inside moves the rectangle; dragging the four corners resizes it; Confirm commits and Cancel/Escape restores the previous valid region.
- Region geometry is centralized in `RegionGeometry` with one minimum size policy: 64 x 64 physical pixels. Move/resize/preset placement clamps to the selected monitor in virtual-screen physical coordinates, including non-zero and negative monitor origins.
- Region presets now create a centered region when none exists, or resize the existing region around its current center where possible. Oversized presets scale down to fit while preserving aspect ratio.
- Preview/recording parity continues to use the existing `CaptureRegion` virtual-screen coordinates and preview restart keys. No renderer rewrite was done.
- The visual harness registry now includes deterministic countdown and region source-picker states. Test-only visual state is compiled behind `EXOSNAP_ENABLE_VISUAL_TEST_HARNESS` and is not present in Release builds.

## Tests

- Added `recording_countdown_tests` for supported durations, monotonic display, delayed ticks, duplicate starts, cancel, complete, and reset.
- Added `region_geometry_tests` for move clamping, negative origins, each corner resize, minimum-size enforcement, inversion prevention, monitor bounds, preset centering, center preservation, oversized scaling, and invalid monitor rejection.
- Updated source-picker tests for preset resizing around an existing region and monitor selection from the existing region center.
- Updated transport dock tests for real countdown selection, Countdown state Cancel action, and locked selector behavior.
- Updated view-model state guard tests so Countdown, Preparing, RegionSelecting, and Stopping cannot start again.
- Updated visual scenario tests for new deterministic scenario IDs and parser rejection of invalid countdown/region metadata.

## Doc Updates

- Updated `docs/design/exosnap-hybrid-port-plan.md` to remove the obsolete "planned/disabled" countdown status and describe the real countdown workflow.
- This implementation note records the milestone summary, tests, and unresolved manual validation items.

## Unresolved Issues

- Real multi-step region drawing, moving, resizing, target switching, global-hotkey behavior, real recording output inspection, and multi-monitor/DPI workflows remain manual validation items by design.
- Countdown selection is session-scoped in this slice. Persisting it in profiles/settings is intentionally deferred until the preset/settings schema work is scoped.
- Visual screenshots are deterministic single-state captures only; no baseline comparison system was introduced in this package.
