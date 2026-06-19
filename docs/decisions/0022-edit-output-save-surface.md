# ADR 0022: Edit / Output / Save Surface (Post-Stop In-Window Mode)

## Status

Accepted — UI shell implemented in Production Suite wave (feat/production-suite-redesign).
Engine implementation (Quick Trim, stream-copy remux, chapter export) deferred to 0.11.

## Context

After a recording stops, users need to decide what to do with the captured file: keep the MKV as-is,
remux it to MP4 (stream-copy, no quality loss — ADR 0014), or re-encode to H.264+AAC (quality cost,
wider compatibility). They may also want to review the recording and optionally trim it before
exporting.

Several surface-shape options were considered:

- **Separate native dialog** — focus-management risk, cannot reuse the in-window nav shell.
- **New top-level tab** — inflates the nav to 6 items again; clashes with the 5-item target.
- **Overlay / modal** — correct for short decisions but too cramped for a full review/edit workflow.
- **In-window mode (stack replacement)** — the main content stack switches to the Edit/Export
  surface; Back returns to Record. Nav items remain accessible but the transport dock is hidden.
  This is the chosen shape.

## Decision

### Surface structure

`EditExportPage` replaces the main content area after recording stops. The surface has three linear
phases stepped by a top stepper bar:

```
Review → Edit → Output
```

- **Review**: video player placeholder (0.11), timeline with waveform simulation, duration / size /
  codec / container detail rail on the right.
- **Edit**: Trim / Marker / Split Chapter controls (all disabled until 0.11 engine ships).
- **Output**: three format cards (Keep MKV / Remux to MP4 / Re-encode H.264+AAC), destination row.
  After clicking Export: Exporting phase (progress bar) → Done or Failed result panel.

### Format / cost model

| Option            | Mechanism    | Quality   | Speed    |
|-------------------|-------------|-----------|----------|
| Keep MKV          | no-op        | lossless  | instant  |
| Remux to MP4      | stream-copy  | lossless  | instant  |
| Re-encode H.264+AAC | transcode  | quality cost | ~3 min (estimate) |

Stream-copy options use libavformat (ADR 0014 path). Re-encode uses the encoder factory (ADR
0006/0011). The default selection is "Keep MKV" — the lowest-friction, zero-risk path.

### IA decision: mode, not tab

The surface is a mode (stack replacement), not a new nav tab. Rationale:
- Keeps top-level nav at 5 items (Record · Settings · Diagnostics · Logs · About).
- The edit workflow is linear and transient — users enter it, decide, and leave.
- Adding a persistent tab implies the surface is always accessible, which is premature before the
  engine ships.
- Back returns cleanly to Record without polluting the nav history.

### Placeholder strategy

The 0.11 engine (Quick Trim, stream-copy segment remux, chapters) is not shipped in this wave.
The surface renders a dashed amber banner:
> "Editing & export tools arrive in 0.11 — this surface is a preview."

Edit controls (Trim / Add Marker / Split Chapter / timeline / player) are disabled. The Output
panel is functional: selecting a format card and clicking Export runs a stub 1.5 s timer.

### Phase stepper

The three-step stepper (Review / Edit / Output) dynamically highlights the current phase:
- Review phase → "Review" highlighted (accent underline).
- Edit phase → "Edit" highlighted.
- Output / Exporting / Done / Failed → "Output" highlighted.

This communicates progress without a modal wizard.

## Consequences

- `EditExportPage` is a pure UI shell; no engine coupling until 0.11.
- The main window stack (`QStackedWidget`) gains `EditExportPage` as an additional page,
  alongside Record, Settings, Diagnostics, Logs, About.
- No new engine API in this wave; the export stub emits `exportCompleted(path)` after a timer.
- `setRecordingInfo()` is the handoff API: MainWindow calls it on recording stop, passing
  file path + duration + size + codec metadata from the session.
- The surface must not emit `navPageRequested` — it only emits `backRequested` and
  `exportCompleted`.
- Post-0.11: the stub timer and placeholder banner are replaced; `EditExportPage` gains a
  `setEngineAdapter()` call or equivalent injection point. The three format cards become
  interactive with real cost estimates and progress reporting.

## Forward

- 0.11: Quick Trim engine, real player (QMediaPlayer or custom D3D11 preview), chapter export.
- Post-1.0: transcription (optional, capability-gated), batch export, GIF/WebP export.
