#MVP Scope

##Product goal

        Create a Windows 11 desktop application that records gameplay or
    desktop / application content with a native,
    efficient,
    diagnostics -
            first pipeline.

            The MVP should already feel like a serious recorder rather than a prototype : -robust -
            understandable - debuggable -
            difficult to misconfigure silently

            ##Core user promise

        > When the app says it is ready to record,
    the selected pipeline is actually valid, the source is visibly arriving, the configured audio tracks are clear,
    and the resulting file is technically inspectable during recording.

        ##Primary use case

        A user records SDR game footage on Windows 11 with:
- an NVIDIA RTX 50-series GPU
- AMD Ryzen-class CPU
- variable source framerate
- desired constant 60 fps output
- application audio, microphone input, and other system audio as independently controllable sources

## In scope

### Capture
- Application/window capture
- Monitor capture
- Desktop capture
- Live preview thumbnail on the Record page

### Video
- CFR 60 fps default output
- AV1, HEVC, H.264 video codec support through hardware encoding
- SDR-only pipeline
- Cursor capture toggle
- Source FPS and output FPS telemetry
- Duplicate/drop accounting

### Audio
- Three source rows:
  - `APP`
  - `SYS`
  - `MIC`
- Default source order:
  1. APP
  2. SYS
  3. MIC
- Default state:
  - all enabled
  - all separate tracks
- Drag-and-drop reordering
- `Merge with above` option for non-top active rows
- Follow-default-device or explicit-device selection where applicable
- Per-source audio meters
- Opus support in v1
- AAC support where needed for MP4 compatibility

### Output
- MKV primary container
- MP4 secondary container
- Valid container/audio-codec gating
- File size estimate and remaining recording time estimate
- Track naming and ordering
- Split active recording
- Add marker

### Diagnostics
- Readiness checks
- Critical blockers
- Start recording disabled when blockers exist
- Selftest / pipeline test
- Expandable explanations
- Structured logs
- Diagnostic export

### UI
- Dark mode by default
- Pages:
  - Record
  - Video
  - Audio
  - Output
  - Webcam
  - Hotkeys
  - Diagnostics
  - Logs
  - Advanced
- Record page has idle and recording states
- Recording page shows:
  - live preview
  - duration
  - file size
  - FPS
  - bitrate
  - duplicate/drop count
  - A/V drift
  - compact pipeline timings
  - audio bars

### Hotkeys
- Start/Stop Recording
- Pause/Resume Recording
- Split Active Recording
- Mute/Unmute Microphone

## Out of scope for MVP

- Instant replay / replay buffer
- Streaming
- HDR
- Hook-based game capture
- Deep game performance metrics
- Video editing/export workflows
- Plugin system
- Cloud sync
- Automatic system-wide optimization changes
- Multi-platform support
- Mobile UI

## Default profile

```text
Container: MKV
Video: H.264 NVENC
Audio: AAC
Output FPS: 60 CFR
Resolution: source resolution
Audio source order: APP, SYS, MIC
Audio resulting tracks: APP, SYS, MIC
Theme: Dark
```

## Success criteria

The MVP is successful when:
1. A user can select a source, confirm preview/audio presence, and record without needing external tools.
2. Invalid configurations cannot accidentally start.
3. The app produces technically correct, multi-track recordings with stable CFR output.
4. The app explains enough about its own environment that support and optimization are feasible without guesswork.
