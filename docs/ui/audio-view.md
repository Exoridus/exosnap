# Audio View

## Purpose

Let the user shape the eventual output-track layout directly, without hiding the result.

## Core layout

```text
Audio
────────────────────────────────────────

Sources & tracks
Drag to reorder the recording track layout.

APP — Selected application audio
[x] Enabled
Source: Game.exe + child processes
Meter: ▂▅▇

MIC — Microphone
[x] Enabled   [ ] Merge with above
Device: Follow Windows default — Shure MV7
Meter: ▂▃

SYS — Other system audio
[x] Enabled   [ ] Merge with above
Source: Everything except selected application
Meter: ▂▅▇

Resulting tracks
1. APP
2. MIC
3. SYS

Encoding
Codec: Opus
```

## Exact wording

The visible option label must be:

```text
Merge with above
```

## Behavior rules

- Rows are drag-and-drop reorderable.
- Default order:
  1. APP
  2. MIC
  3. SYS
- Default result:
  1. APP
  2. MIC
  3. SYS
- Topmost enabled row does not show `Merge with above`.
- Disabled row:
  - does not participate in result
  - has merge disabled or hidden
- `Resulting tracks` updates immediately.

## Device selection

### APP
- selected application / process tree

### MIC
- Follow Windows default
- or choose explicit input device

### SYS
- other system audio, with source semantics documented

## Metering

- Show compact bars per source
- Surface mute/silence/clip states when applicable
