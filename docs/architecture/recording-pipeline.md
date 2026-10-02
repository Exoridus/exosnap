# Recording pipeline and durability

This document owns admission, worker/resource lifetime, packet flow, splitting, finalization and recording recovery. [Timing](timing-and-cfr.md) owns timestamps; [encoding and containers](encoding-and-containers.md) owns media representation.

## Transactional preparation

The GUI-side start performs a reentrancy/start-state gate, snapshots every configuration input by value and exposes Preparing. The named recording worker performs potentially blocking work: destination validation and creation, disk and display queries, effective configuration assembly, source and encoder preparation, webcam startup, recovery registration and the preview lease handshake. It then enters the recording session without returning blocking work to the GUI thread.

Preparation owns the webcam just as recording does. Idle preview synchronization must not concurrently stop or reopen the camera between the Preparing callback and the worker's recording flag. Cancellation is cooperative at preparation checkpoints; an already-running device operation finishes before the cancellation takes effect. A failed preparation closes resources it acquired and reports its phase. It must not flash Recording or restore a false Ready state after the user explicitly started a failed operation.

Input snapshots include output, split, video, webcam, audio, target context and resolved capabilities. Reading mutable presentation models from that worker would create a recording assembled from different settings moments.

## Workers and queues

`RecorderSession` owns shared session state. Video work owns its D3D11 immediate/video contexts and encoder resources. Audio workers own capture, conversion and encoding for their resolved tracks. The mux worker serializes encoded audio/video packets into the container. UI callbacks are queued observations and may not touch an engine context from the GUI thread.

Codec initialization and packet arrival are asynchronous. A bounded pre-mux buffer holds packets until the video private data and every declared audio track are ready. Readiness is explicit: PCM legitimately has empty codec-private bytes, which cannot be confused with an uninitialized track. Audio pre-mux storage is bounded at 600 packets. Exceeding the preparation bound is a visible failure rather than unbounded memory growth.

The steady-state mux queue is also bounded. Producers wait for room within the queue policy, then fail cleanly if storage cannot keep up. They do not silently discard encoded audio or relabel a shortened recording as complete. Stop/failure wakes the waiters, and stop-related incomplete tails are distinguishable from ordinary backpressure.

Ownership survives a timeout. Workers retain shared state until their `Run()` returns; a timed-out join cannot leave a thread writing through a destroyed object. NVENC flush/event waits have finite budgets so a lost device does not wedge process shutdown indefinitely. An incomplete drain remains a failure/limited result, not proof that every submitted packet reached disk.

## Output artifacts

The configured destination is revalidated for the actual start. A missing/unreachable/unwritable destination is not a low-space query failure and blocks admission. Once reachability and writability are established, an unavailable free-space reading disables that protection with a log warning; it is not interpreted as zero free bytes. A measured zero is a full disk.

Valuable unfinished recordings use `.partial` on the configured volume. Disposable staging is separate from the valuable source. Final MKV/WebM publication uses an atomic rename after successful finalization. Remux/repair output uses a sibling temporary on the destination's volume and replaces the final path only after success. Cross-volume copy is not an atomic replacement primitive.

MP4 delivery records a Matroska source and stream-copies it to progressive MP4. The single-file successful MP4 path can retain the source as an `.edit.mkv` edit master. Failed/canceled remux preserves recoverable source media. The retained edit master consumes additional space; do not document successful MP4 delivery as unconditional deletion of every intermediate recording. Splits use their own per-segment completion path.

The low-disk hard-stop reserve includes coexistence of source and remuxed output and pending segment jobs. FAT32 produces an advisory, not automatic protection at the per-file limit. User-selected size splitting is approximate and keyframe-bound, so a limit near the filesystem maximum is not a safe guarantee.

## Split ordering

Time and size are independent limits, with zero disabling the respective axis. Time is measured on media progress; size comes from committed container bytes, not filesystem polling. The first limit reached requests a split. A shared request sequence prevents two observers from turning one boundary into two rolls.

The split crosses the packet pipeline as an ordered boundary. Force the next segment's first video picture to be independently decodable, carry the correct codec/color information, and do not move an old segment's packets behind the new header. Per-segment counters reset at that boundary. A size threshold is therefore approximate, not an exact byte ceiling.

MKV and WebM segments finalize independently. MP4 segments remux in the background while the next recording segment runs. The application may report a segment's completion separately, but the session is not Saved until all required remux work has completed. Manual split availability follows the product transport gate and is separate from automatic splitting.

## Recovery manifest

The recovery entry captures the intended final format and output snapshot, not whatever settings happen to be active on the next launch. A manifest write failure does not destroy a healthy recording or block its start; it produces a Recovery protection unavailable notification, and that session must no longer be represented as protected.

Scanning discards missing or empty artifacts rather than offering an impossible recovery repeatedly. Recovery offers only real candidates:

| Operation | Contract |
|---|---|
| Finish | Repair/finalize/remux according to the candidate's saved intent; keep valuable source media if the repair fails |
| Continue | Only an unfinished crash candidate; retain/finish the old slice and arm the next independent slice paused |
| Delete | Explicit destructive confirmation before removing the candidate |
| Decide later | Leave it registered for a later launch |

Only one continuation can be armed. Choosing another resolves the previously armed continuation rather than leaving two hidden sessions. Continued slices are independent files, not concatenated streams. Already-finalized segments remain useful even if the active tail cannot be repaired.

Repair publishes to a sibling temporary and atomically replaces the candidate's intended output only after success. An interrupted remux must not leave a corrupt file at the public final path and a successful repair under an unrelated name.

## Durability boundary

Periodic durability flushing bounds what the process attempts to commit; container clustering and buffered reordering also affect the salvageable tail. Neither a unit test nor a successful `FlushFileBuffers` call proves a universal one- or two-second maximum loss on every storage device and power-failure mode. A crash can lose the active tail or the active segment. State that boundary instead of promising zero loss.

Synthetic truncation drills prove parser/repair behavior on real container bytes. They do not execute every coordinator crash window or prove physical power-loss durability. The [soak and recovery runbook](../dev/soak-and-recovery-drills.md) separates those tests from controlled live interruption.

## Implementation and tests

The primary owners are [RecordingCoordinator](../../app/services/RecordingCoordinator.cpp), [RecorderSession](../../libs/engine/src/recorder_session.cpp), [session internals](../../libs/engine/src/session_internal.h), [audio worker](../../libs/engine/src/audio_thread.cpp), [mux worker](../../libs/engine/src/mux_thread.cpp), [RecoveryService](../../app/services/RecoveryService.h) and [atomic file operations](../../app/services/AtomicFileOps.h). Their adjacent tests cover rollback, split ordering, queue limits, manifest handling and atomic publication.
