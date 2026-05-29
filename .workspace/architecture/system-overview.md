# System Overview

## Architectural goal

Keep the high-performance recording engine native and UI-agnostic while keeping the user interface simple, declarative, and easy to evolve.

## Top-level components

```text
┌─────────────────────┐
│ Qt 6 Widgets UI     │
│ - pages             │
│ - bindings          │
│ - commands          │
└─────────┬───────────┘
          │
          │ thin application/service boundary
          ▼
┌─────────────────────┐
│ Recorder Engine     │
│ - session control   │
│ - diagnostics       │
│ - telemetry         │
│ - settings models   │
└─────────┬───────────┘
          │
          ├───────────────┬───────────────┬───────────────┐
          ▼               ▼               ▼               ▼
   Video Capture     Audio Capture     Encoders        Muxers
   DXGI OD/WGC       WASAPI            NVENC/Opus      MKV/MP4
   + D3D11
```

## UI technology

- **Qt 6 + Qt Widgets**
- Dark mode requested by default
- UI does not implement media logic
- UI renders engine state and submits user intent

## Engine technology

- **C++20/23**
- Native Windows APIs
- Explicit session state machine
- Structured diagnostics and telemetry
- Profiling hooks available without making profiling a runtime requirement

## Core responsibilities

### UI
- Render pages
- Bind to view models
- Collect user commands
- Display readiness, preview, metrics, and logs
- Never duplicate engine resolution logic

### Engine
- Own settings validation
- Resolve audio rows into resulting tracks
- Validate container/codec compatibility
- Manage recording session lifecycle
- Run diagnostics
- Produce live metrics snapshots

### Capture layer
- Video source acquisition
- Audio source acquisition
- Device/source change handling

### Encode layer
- NVENC for video
- Opus / AAC for audio

### Mux layer
- MKV primary
- MP4 secondary
- Split and marker handling

## Theme decision

Dark mode is the default appearance for the MVP. The UI should use native theme resources instead of custom visual redesign work. Light mode support can remain a later setting; default behavior is dark.

## Design principles

1. **Visible truth over hidden magic**
   - preview before recording
   - resolved tracks visible
   - readiness visible
2. **Invalid states do not start**
   - blockers prevent recording
3. **Separation over convenience**
   - APP, MIC, SYS remain independently modelled even when merged into tracks
4. **Measure before optimizing**
   - internal telemetry from day one
5. **MVP discipline**
   - no overlay/HUD until the core recorder is solid
