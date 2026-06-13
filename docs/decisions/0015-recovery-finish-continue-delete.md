# ADR 0015: Recovery UI — Finish / Continue / Delete (v0.2.0)

## Status

Accepted — implements the v0.2.0 recovery overlay action model.

## Context

The previous recovery overlay (v0.2.0 wave 1, PR #44) offered three actions per
candidate:

- **Keep as MKV** — repair-remux or rename to `.mkv`.
- **Export as MP4** — remux to progressive `.mp4`.
- **Discard** — delete the artefact.

This model required the user to make a format choice at recovery time, decoupled
from how the recording was originally configured. It also offered no way to
continue recording from a crash point. The bare `×` dismiss button was ambiguous
about its semantics.

## Decision

### Action model (per-candidate row)

1. **Finish** — always shown. Honours the manifest snapshot:
   - `intended_container` = `mkv` + `finalized = false` → repair-remux via
     `RemuxToMkv` → `.mkv` output.
   - `intended_container` = `mkv` + `finalized = true` → collision-safe rename
     to `.mkv` output.
   - `intended_container` = `mp4` (any finalized state) → `RemuxToProgressiveMp4`
     → `.mp4` output.
   - Destination directory = stored `final_output_path` parent when it exists on
     disk; fallback to the current configured output directory (injected via
     `RecoveryService::SetFallbackOutputFolder`); last resort: artefact parent.
   - There is **no user-facing format choice** — the recording finishes as what
     it was configured to be.

2. **Continue** — only shown for **non-finalized** candidates (true crashes where
   `finalized = false`). Finalized entries are deliberate-stop artefacts whose
   remux failed; they should be Finished or Deleted, not continued.
   - Slice-based continuation. The coordinator enters `ArmedFromRecovery` (paused)
     state. Resume starts the next capture slice; Stop finalizes the whole session.
   - No byte-level appending. The crash artefact is repaired-remuxed by
     `RecoveryService::Finish` in the background as the session's first slice.
   - The 1–2 s data loss at the crash boundary is accepted and visible as the
     slice boundary.
   - If the original capture target no longer exists, the user is prompted for
     target selection on Resume (reuses the existing source picker; no new UI).

3. **Delete** — inline two-step confirm, destructive styling. Same semantics as the
   previous Discard action.

4. **Decide later** — explicit text button at the bottom of the card, replacing the
   bare `×`. Escape / backdrop-click share the same semantics. Entries remain in
   the manifest; the overlay re-shows at the next launch.

### Multi-recovery rule

At most **one** `ArmedFromRecovery` session at a time. Choosing Continue on a
second candidate while one is already armed:
1. Calls `RecordingCoordinator::FinalizeArmedRecovery()` on the current session
   (its slices become a finished recording; the background remux is already in
   flight or completed).
2. The new candidate takes its place via `ArmFromRecovery()`.

### Coordinator state

`UiRecordingState::ArmedFromRecovery` is added between `Paused` and `Stopping`.
From the TransportDock's perspective it is visually equivalent to `Paused`.

- `StartRecording` allows the transition from `ArmedFromRecovery` to `Preparing`
  (the next slice).
- `RevalidateCapabilities` treats `ArmedFromRecovery` as busy (does not
  overwrite the state).
- `IsWebcamOverlayEditable` includes `ArmedFromRecovery`.

### RecoveryService changes

- `Finish(entry, progress_cb)` replaces the separate `KeepAsMkv` / `ExportAsMp4`
  entry points. The output format is determined by `intended_container`.
- `SetFallbackOutputFolder(folder)` accepts the current configured output dir so
  Finish can resolve a valid destination when the stored folder no longer exists.
- `KeepAsMkv` and `ExportAsMp4` are preserved as legacy aliases (delegating to
  `Finish`) so existing tests continue to pass unchanged.

## Consequences

- **No concat-into-one-file.** The continued session produces independent slices
  (one per recording run after the crash). Quick Trim (0.11.0) is the planned
  path for post-hoc joining.
- **No new toast/notification surface.** `ArmedFromRecovery` state is visible in
  the TransportDock only (paused-state visual).
- **No new UI pages or dialogs.** The source picker for target re-selection is the
  existing `SourcePickerOverlay`.
- **Pre-v1 breaking change:** the manifest schema is unchanged (no new fields
  needed); the overlay widget state is rebuilt from scratch at startup.
- **KNOWN_LIMITATIONS.md** is updated to reflect the new action vocabulary.

## Decisions made beyond the product model

- `KeepAsMkv` / `ExportAsMp4` are kept as aliases rather than removed, to avoid
  a test-breakage that would require a non-trivial test rewrite without adding
  coverage value. They are not re-exposed in the UI.
- `RecoverySessionInfo.target_valid = false` is always set by
  `RecordPage::armFromRecovery` because the capture target is not recorded in the
  manifest (it was not a field before this ADR). The user is prompted for target
  selection on Resume. A future slice could add `capture_target_description` to
  the manifest schema to pre-populate the source picker.
- `FinalizeArmedRecovery` transitions to `Ready` / `Blocked` (not `Completed`)
  because the armed session's remux was already running (or completed) by the time
  the user clicks Continue on the next candidate; there is no additional work to do
  at finalization time in the coordinator.
