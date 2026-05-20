# Recording Pipeline

## Primary data flow

```text
Video source
  → Windows Graphics Capture
  → D3D11 texture
  → CFR scheduler
  → NVENC
  → muxer

APP audio
MIC audio
SYS audio
  → source capture
  → per-source metering / clock metadata
  → track resolver / optional merges
  → Opus or AAC encoding
  → muxer

Muxer
  → MKV or MP4
  → target file
```

## Video path

### Capture
- Primary API: Windows Graphics Capture
- GPU path: D3D11 textures
- Source types:
  - app/window
  - monitor
  - desktop

### Output timing
- Default output: 60 fps CFR
- Source frame arrival may be variable
- Scheduler behavior:
  - at each output tick, choose newest available frame
  - duplicate previous frame when no new frame is available
  - drop stale source frames when multiple arrive between ticks

### Video metrics
- source FPS
- output FPS
- duplicate rate/count
- drop count
- capture-to-encode latency
- encode timing
- bitrate

## Audio path

### Source semantics
- `APP`: selected application audio
- `MIC`: microphone input
- `SYS`: other system audio

### Track semantics
- Source rows are reordered by the user.
- Active rows become output tracks according to `Merge with above`.
- Output track order follows resolved row order, not hard-coded source type order.
- Default rows produce default tracks:
  1. APP
  2. MIC
  3. SYS
- Phase 4 adds a default microphone capture source via WasapiCaptureSrc for a single-source MIC → Opus → MKV vertical slice.

### Audio metrics
- per-source peak/bar meter
- mute status where available
- silence detection
- clipping detection
- drift metadata

## Muxing

### MKV
- primary recorder container
- default with Opus
- preferred for multi-track and crash resilience

### MP4
- secondary compatibility container
- only expose compatible audio codecs
- show info text about lower crash resilience
- reconcile audio codec when container changes

## Session state machine

```text
Idle
  → Preparing
  → Ready
  → Recording
  → Splitting
  → Recording
  → Stopping
  → Idle

Recording ⇄ Paused

Error transitions may enter Failed.
```

## Required actions

- Start recording
- Stop recording
- Pause/resume recording
- Split active recording
- Add marker
- Mute/unmute individual audio sources

## Failure handling

The system must fail early and visibly when:
- selected source cannot initialize
- required audio source cannot initialize
- codec/container combination is invalid
- encoder cannot initialize
- muxer cannot initialize
- target path is invalid or unwritable
- storage selftest fails beyond allowed threshold

### APP process tree exit
- Exit of a child process within the target APP process tree is logged
  as a device-change-style event; the recording session continues.
- Exit of the root process of the target APP process tree, or any
  loss of the selected video capture source, is handled by the
  existing source-loss policy: the session stops cleanly, a failure
  log entry is written, and no partially-corrupted output is produced.

## Preview

The live preview uses the same selected video source as the recording pipeline but must remain logically separate from encode/mux lifecycle. The user should be able to validate the source before recording begins.
