# ADR 0072: StartRecording runs off the GUI thread behind a real Preparing state

## Status

Accepted, implemented.

Related: ADR 0040 (preview source-tap), ADR 0041 (capture hub lease, the release handshake this ADR's worker thread blocks on).

## Context

`RecordingCoordinator::StartRecording` ran entirely on the calling thread, which is the GUI thread: a synchronous free-space query on the output volume, output path resolution and directory creation, a fresh DXGI display-facts query, the webcam device open, and the capture-hub release handshake (`RequestEngineLease`, which blocks up to 750 ms) all happened before the recording thread was ever started. `PostStateChange(Preparing)` already existed but was posted through a queued connection immediately followed, on the same synchronous call stack, by `PostStateChange(Recording)`: both were processed by the event loop before a repaint could happen between them, so the `Preparing` state was never actually visible. With the default 0 s countdown, `Preparing` was the only possible pre-record feedback, and it did not render.

## Decision

**Thin gate, fat worker.** The GUI thread does only three cheap things: a synchronous re-entrancy guard, a startable-state check, and a full **by-value snapshot** of every input and config model the body reads (`PrepareContext`). It then posts `Preparing` and starts the worker thread.

The snapshot is not just the target, audio and crop fields. It covers every settings model the worker touches (`output_settings_`, `split_settings_`, `video_settings_`, `webcam_settings_`, `resolved_user_config_`, `output_target_context_`, `caps_`), because all of them remain writable from the GUI thread while a prepare is in flight.

Everything else runs on that one worker thread: the disk and filesystem checks, the DXGI facts refresh, config assembly, `Validate`, the webcam start, the recovery-manifest write, and the release handshake. It then falls straight into `session_.Record()` with no second thread hop.

The worker reads only from its own snapshot, never from a mutable coordinator member. The one exception is `caps_` for the DXGI-facts refresh, which stays protected by the existing `RevalidateCapabilities` early-return while any of `Preparing`, `Recording`, `Paused`, `Stopping` or `ArmedFromRecovery` holds.

### Alternatives considered

- **B: move only the device-open steps off-thread, keep validation synchronous.** Only the DXGI-facts query, webcam start, and release hook would move to the worker; disk check, path resolve, `mkdir`, config assembly, and `Validate` would stay on the GUI thread. Rejected: path resolution and directory creation can themselves stall for seconds on a network or removable drive, so this only partially fixes the freeze. It also ties the HDR10-native config derivation to the DXGI facts, which would force splitting config assembly across the thread boundary, which is fragile and harder to maintain for an incomplete fix.
- **C: wrap the existing body in `QtConcurrent`/`std::async`.** Functionally option A with an anonymous future instead of a named `jthread`. Rejected: it loses the existing, cleanly-owning `recording_thread_` `jthread` semantics (joined in the destructor), and complicates cooperative cancellation and the direct fall-through into `Record()`.
- **Marshal the release hook back to the GUI thread via `BlockingQueuedConnection`.** Rejected outright, because `RequestEngineLease` is exactly the up-to-750 ms blocking step: marshaling it back to the GUI thread would freeze that thread again for up to 750 ms, reintroducing the problem this ADR exists to fix. The hook stays on the worker; it is thread-agnostic (it touches no GUI-only objects), and the `RecordPage` members its lambda reads are snapshotted by value when the hook is installed.

### Webcam device ownership across the prepare boundary

Moving the body off-thread creates a new race: a queued `Preparing` callback can run on the GUI thread while the worker has not yet set `is_recording_`, so `syncWebcamPreviewCapture` falls out of "idle" and stops the webcam device concurrently with the worker's own `webcam_service_.Start()`. A running prepare now owns the webcam device the same way a running recording does: an in-flight prepare flag gates both `SyncWebcamService`'s start/stop path and `SetWebcamSettings`'s `force_restart` path, so the GUI thread neither starts nor stops the device during the prepare window.

## Consequences

- `Preparing` is now genuinely visible: the GUI thread returns to its event loop immediately after posting it, so a repaint happens before `Recording` is posted.
- More state crosses the thread boundary than a narrower fix would need (`current_output_path_`, `caps_.runtime.displays`, `current_manifest_id_`), which is deliberate given the interleaving hazards a partial fix (option B) would have left in place.
- `StartRecording`'s boolean return value loses any "error detail" meaning, since the caller already ignored it.
