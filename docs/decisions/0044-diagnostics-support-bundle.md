# ADR 0044: Diagnostics support channel — log schema, session report, and support bundle

## Status

**Accepted — 2026-07-12.** ExoSnap has no telemetry, so the diagnostic artifact a user
manually shares *is* the support channel. This ADR records the decisions behind three
connected pieces that close that channel: a structured log schema with a session key, an
on-disk per-recording session report, and a one-click, scrubbed support bundle.

## Context

The diagnostics foundation (readiness gate, FixActions, the live `RecordingDiagnosticsSnapshot`)
was strong, but nothing let a user hand support a coherent picture after the fact. The
human-readable `exosnap.log` was free text with no correlation key; the structured
`engine.jsonl` was truncated on every launch and carried no session id; there was no
session-report file; and the Diagnostics "Export Report" button was permanently disabled.

## Decisions

### Log schema — one structured stream, one text stream, one key

- The human-readable `exosnap.log` stays the crash-safe support log, unchanged (100+ callsites,
  each line flushed — exactly what you want before an abort). It is not converted to JSON.
- The **engine JSONL becomes the canonical structured stream.** It now **appends and rotates**
  (`rotating_file_sink`, 5 MiB × 3 files) instead of truncating on every launch, and every
  record carries a **launch session id** in `fields{}` (via a new `LoggerConfig::baseFields`).
  Rotation thresholds are configurable (`maxFileBytes`/`maxFileCount`) so a test can drive a
  small threshold.
- A thin app entry point, `logEvent(severity, subsystem, event_code, fields)`, forwards **only**
  to the engine logger. The existing `EngineLogBridge` sink remains the single source of the
  flattened text line, so one event yields exactly one text line and one JSONL record — never a
  double. `logEvent` never writes AppLog directly.
- **`event_code` convention.** Stable, locale-independent tokens (e.g. `record.start`,
  `encoder.init`, `audio.discontinuity`, `mux.finalize`, `disk.hardstop`). Severity must be
  `>= Info`: the engine logger's `minimumLevel` is Info, so a Debug event would be dropped before
  reaching either stream. The JSONL runs continuously, governed only by that engine level —
  decoupled from the developer log-level switch (which only mutes the text line).
- Existing `AppLog::info` callsites are **not** migrated; only new/high-value events use
  `logEvent`. No MVP expansion.

### Session report — a per-recording `session-<id>.json`

- Written beside the logs (`<logdir>/reports/session-<id>.json`) at recording end, atomically
  (`QSaveFile`), keeping the **10 most recent** (pruned on write).
- The report id is a dedicated `recording_session_id_`, minted at `StartRecording`
  **independent of the (nullable) recovery store** and not cleared before the result is posted —
  stable across split segments. It is deliberately **not** `current_manifest_id_`, which is
  store-gated, cleared before `PostResult`, and re-minted per segment.
- Content: resolved output format, requested config, capture backend, encoder init parameters,
  drop/dup/discontinuity counters, duration skew, A/V drift and **peak** A/V drift, segment list,
  and failure phase. Metrics with no measurement are recorded as `"unavailable"`, never a fake 0.
- **Peak A/V drift moved into the engine aggregator** (a new snapshot field) so the live UI and
  the report read one authoritative value; the report card no longer maintains its own maximum.
- **Encoder init parameters** ride on the snapshot as a plain `EncoderInitInfo` struct (no NVENC
  types leak to the app layer) and are also emitted once as an `encoder.init` event, so they are
  in the JSONL even when a failure means no final snapshot.
- **Privacy:** the report holds no absolute paths — only byte counts, codecs, counters, and the
  **scrubbed** output *file name* (kept for support correlation, per the product decision).
- App-layer JSON uses **Qt JSON** (`QJsonDocument`), consistent with the other app-layer writers;
  `nlohmann` stays engine-internal.

### Support bundle — one click, scrubbed, allowlisted

- A pure `CollectBundleEntries` + a thin `WriteBundleZip`. ZIP via the already-vendored
  `exosnap_miniz` (a new `ZipWriter` unit; no new archive library, no lifting out of `libs/update`),
  reusing that library's `IsSafeZipEntryName` guard.
- Contents: rotated text + JSONL logs, the last N session reports, `capability.json`,
  `adapters.json`, `displays.json`, a scrubbed `settings.txt` (from `ConfigSummary`, never a raw
  `settings.ini`), and `bundle-manifest.json`. Structured files are **allowlist** — only known
  fields, never a raw dump.
- **Scrubbing.** Every text entry passes through `ScrubString` (paths/username/machine) **and** a
  bundle-local `RedactCaptureTargets` pass. The latter exists because `ScrubString` has a blind
  spot: a capture-target **window title** in a `target="…"` freetext field or a JSONL `"target"`
  field carries no drive prefix and would otherwise survive. `RedactCaptureTargets` replaces the
  value with `[capture-target]` (backend/event survive — "window vs monitor capture" is enough for
  support). It lives bundle-local, **not** in `crash_scrubber`, so the consent-gated crash path is
  unchanged. A regression test asserts a fixture window title appears in no entry.
- **Not included:** recordings, raw settings/preset/history files, absolute paths, username,
  machine name, crash dumps (those have their own consent-gated channel).
- **No auto-upload.** The bundle is written via a save dialog, then revealed in the file manager —
  consistent with no telemetry. Entry points: a "Create support bundle" button on Logs (primary,
  not expert-gated) and the re-enabled Diagnostics export button (second entry, one code path).

### Startup trace

A process-wide `StartupTrace` collector records the existing `StartupClock` milestones (label +
ms). Logs shows them as a compact table; the bundle carries `startup-trace.txt`. No new timing is
introduced — the same readings that were already logged are additionally recorded.

## Consequences

- The engine JSONL now persists across launches (bounded by rotation) instead of resetting — a
  deliberate behavior change; the truncate-on-launch reset is gone.
- Two distinct session ids exist and are named explicitly everywhere: `launch_session_id`
  (log/JSONL/bundle correlation) and `recording_session_id` (report name, stable across splits).
- `RecordingDiagnosticsSnapshot` gained `encoder_init`, `peak_av_drift_ms` (+ availability). This
  is a schema touch; pre-1.0, no migration.
- The privacy surface of the bundle is the main trust risk; it is the interface point to the
  separate privacy-review effort (bundle scrubbing is a checklist item there).

## Deliberately not built

No auto-upload/telemetry; no migration of old logs or of the 100+ existing `AppLog::info`
callsites; no change to the `av_drift_ms` metric (already a real clock-drift measurement); no new
diagnostics runtime checks; no crash-annotation changes.
