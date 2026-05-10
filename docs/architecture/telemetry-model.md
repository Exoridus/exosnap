# Telemetry Model

## Goal

Provide useful live metrics with negligible runtime overhead and strong diagnostic value.

## Design

### High-frequency producer layer
Hot paths write compact event data:
- timestamps
- frame IDs
- queue depths
- bytes
- sample counts
- status flags

### Low-frequency aggregator layer
Every 250–500 ms, aggregate:
- current values
- rolling averages
- maxima
- counts
- health flags

### Consumers
- Record view
- diagnostics
- logs
- optional future overlay
- optional developer profiling

## Recording-view technical layout

```text
REC 00:12:41 | 3.82 GB
FPS 143 → 60.0 | BIT 42.1 Mb/s
DUP 0.4% | DROP 0 | A/V +0.4 ms
LAT 1.8 ms | ENC 0.3 ms | WRT 0.2 ms
APP ▂▅▇  MIC ▂▃  SYS ▂▅▇
```

## Core metrics

### Session
- recording duration
- current file size
- segment count when applicable

### Video
- source FPS
- output FPS
- duplicate rate/count
- drop count
- capture-to-encode latency
- encode timing
- bitrate

### Audio
- per-source bar level
- mute status
- silence detection
- clipping detection
- A/V drift

### I/O
- write latency
- bytes written
- estimated remaining recording time

## Additional diagnostics-only metrics

- queue depths
- longest duplicate streak
- source-frame gap
- mux backlog
- audio discontinuity count
- resampler correction / drift compensation
- storage selftest throughput
- device-change events

## Naming

- `FPS source → output` is displayed as `FPS 143 → 60.0`
- `LAT` means capture-to-encode pipeline latency
- `ENC` means encode timing for the video path
- `WRT` means write timing
- `A/V` means measured audio/video drift
- `LAT` is measured from the capture-frame acquire QPC timestamp to
   the NVENC submit QPC timestamp.

## Bar meters

Bars are preferred in the recording UI over raw dB values.

Examples:
```text
APP ▂▅▇  MIC ▂▃  SYS ▂▅▇
MIC MUTE
MIC ─
MIC ▇!
```

## Update policy

- Do not push every raw event to UI.
- UI receives aggregated snapshots.
- Logging can retain richer samples than the visible UI.
