# ADR 0021: Dual Independent Time + Size Split Thresholds ("Whichever First")

## Status

Accepted — implemented in 0.5.0 (feat/0.5.0-split-by-size).

## Context

ExoSnap already supported time-based automatic segment splitting (SPLIT-RECORDING-R1):
a single `RecordingSplitSettings { RecordingSplitMode mode; uint64_t duration_ms; }`
struct that mapped a UI enum (Off / Every15Min / Every30Min / Every60Min / Custom) onto a
media-time interval. The engine's video thread compared encoded-frame PTS against
`next_auto_threshold_ns` and armed a forced keyframe when the threshold was crossed.

The product design spec (`.workspace/design/mappe-bundle/project/mappe-ia.jsx`, "Hard cases")
requires a **parallel size threshold**: "Output → Advanced: two paired inputs (every N min ·
every N GB) with an explicit **whichever comes first**." Time and size are independent: both
can be active simultaneously, and the first one hit in a given segment triggers the rollover.

## Decision

### Engine model (`RecordingSplitSettings`)

Replace the `(mode, duration_ms)` pair with two independent numeric thresholds:

```cpp
struct RecordingSplitSettings {
    uint64_t duration_ms = 0; // 0 = time-based splitting disabled
    uint64_t size_bytes  = 0; // 0 = size-based splitting disabled
};
```

`RecordingSplitMode` is retained in the header as a legacy comment/alias for documentation
continuity but is no longer used at runtime. The coordinator resolves both UI dimensions to
numeric values before handing them to the engine.

This keeps the engine UI-agnostic: it sees only thresholds, not named presets.

### UI model (`SplitRecordingSettings`)

A second independent axis is added to `SplitRecordingSettings`:

```cpp
enum class SplitSizeMode { Off, Custom };

struct SplitRecordingSettings {
    // Time axis (unchanged)
    SplitRecordingMode mode = SplitRecordingMode::Off;
    uint32_t custom_minutes = 30;
    // Size axis (new)
    SplitSizeMode size_mode = SplitSizeMode::Off;
    uint32_t custom_size_mb = 2048; // 2 GiB default
};
```

Resolver: `SplitSizeBytes(s)` → `0` (off) or `mb × 1024 × 1024` (clamped to
[`kMinSizeMb` = 50, `kMaxSizeMb` = 1 TiB]).

### "Whichever comes first" runtime mechanism

Two separate threads own the two thresholds:

**Time threshold** — VideoThread (unchanged):
- `next_auto_threshold_ns` is computed from `duration_ms` and compared against encoded-frame
  PTS on each frame. When `pts_ns >= threshold`, VideoThread arms a forced keyframe and emits
  a `SplitSentinel`, then resets the threshold for the new segment.

**Size threshold** — MuxThread (new, SPLIT-BY-SIZE-R1):
- After each mux-queue drain, `sample_mux_diagnostics()` reads `seg.writer->bytes_written()`
  (the cumulative bytes flushed to the file for the current segment).
- When `bytes >= size_bytes` and no size split has been armed yet for this segment,
  the mux thread does:
  1. Sets `m_state.size_split_armed = true` (atomic CAS; guards against repeated triggers).
  2. Writes `split_last_trigger = AutomaticSize`.
  3. Bumps `split_request_seq` (the same monotone counter used by manual splits).
- VideoThread sees the seq bump on its next frame, sets `split_armed = true`, arms a forced
  keyframe, and enqueues a `SplitSentinel` through the normal rollover path.
- When `begin_new_segment()` runs in MuxThread (on the `SplitSentinel`), it resets
  `size_split_armed = false` so the next segment can also trigger if needed.

Because both thresholds funnel into the same `split_request_seq` / `SplitSentinel` path,
"whichever comes first" is automatic: the first condition to fire increments the seq; the
other's check runs after the boundary and finds `split_armed = true` (already coalescing),
so it cannot double-fire.

### Thread safety

- `split_request_seq` is `std::atomic<uint64_t>` (pre-existing).
- `split_last_trigger` is `std::atomic<uint32_t>` (pre-existing).
- `size_split_armed` is `std::atomic<bool>` (new); set via `compare_exchange_strong` to
  prevent concurrent duplicate triggers.
- No new mutexes; no new cross-thread blocking paths.

### Persistence (TOML, schema version 7)

Two new keys under `[presets.output]`:
- `split_size_mode` (string: `"off"` / `"custom"`)
- `split_custom_size_mb` (integer, MiB)

Schema version bumped 6 → 7. Files from older versions reset to defaults (pre-1.0 policy).

### UI placement (`ConfigPage`)

A "Split by size" row is added in the Output panel immediately below the existing
"Split recording" (time) row, using the same combo/spin pattern. Both rows are
independently controllable. MP4 disables both (MP4 is single-file-only in the engine).

### Disk reserve

The existing low-disk guard (`DiskSpaceThresholds`) already tracks the 2 GiB hard-stop
and the MP4 remux reserve. Size-split segments are bounded by the configured threshold
(e.g. 2 GiB), so no reserve-math change is needed for the common case. A future
improvement could reduce the MP4 remux reserve estimate to `max(size_bytes, duration_ms ×
estimated_bitrate)`, but that is deferred post-1.0.

## Consequences

- Pre-1.0: schema version 6 preset files are silently reset (consistent with project policy).
- Time-only splits behave exactly as before (no behavioral change when `size_bytes == 0`).
- Manual splits continue to work regardless of automatic thresholds.
- The engine struct `RecordingSplitSettings` is now pure numeric (no enum); the coordinator
  resolves all UI policy before handing settings to the engine.
- A size split is approximate ("approximately N GB"), not byte-exact, because it triggers
  on a keyframe after the threshold is crossed.

## Forward

- The UI suffix label currently says "MB" but means MiB; a future polish pass can clarify
  ("GiB / MiB" display).
- Post-1.0: an estimated remaining-time / remaining-size indicator in the recording view.
