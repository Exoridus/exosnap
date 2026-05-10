# Record View

## Purpose

The Record view is the operational center of the app.

It must answer:
1. What will be recorded?
2. Is the source arriving?
3. Are the audio sources arriving?
4. Is the system ready?
5. What is happening right now during recording?

## Idle layout

```text
Profile: High Quality AV1 / MKV + Opus                    [Edit]

Video
┌────────────────────────────┐
│        live preview        │
└────────────────────────────┘
Source: Game.exe · 2560×1440 · SDR

Audio
APP ▂▅▇  MIC ▂▃  SYS ▂▅▇
Tracks: APP · MIC · SYS

Output
D:\Recordings\
1.8 TB free · ≈ 16 h 42 min remaining

Readiness
✓ GPU encoder ready
✓ Audio sources ready
✓ Target drive ready
! Driver older than validated baseline

[ Run Check ]                                    [ ● Start Recording ]
```

## Recording layout

```text
REC 00:12:41 | 3.82 GB

Video
┌────────────────────────────┐
│        live preview        │
└────────────────────────────┘

FPS 143 → 60.0 | BIT 42.1 Mb/s
DUP 0.4% | DROP 0 | A/V +0.4 ms
LAT 1.8 ms | ENC 0.3 ms | WRT 0.2 ms

APP ▂▅▇  MIC ▂▃  SYS ▂▅▇

[ Add Marker ]   [ Split Recording ]   [ ■ Stop Recording ]
```

## Required behavior

- Preview must be visible before recording starts.
- Start button disabled when readiness is `Blocked`.
- When recording begins:
  - replace readiness summary emphasis with live technical status
  - keep preview visible
  - keep audio meters visible
- `Split Recording` creates a new segment without leaving recording state.
- `Add Marker` records a time marker.
- If recording was started by hotkey:
  - switch to this view when app is visible
  - do not restore if minimized

## Non-goals

- No overlay/HUD controls in MVP.
- No dense system dashboard here.
- No full diagnostics tree here.
