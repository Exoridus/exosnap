# Video View

## Standard controls

- Output frame rate
  - 60 fps default
  - other allowed values may be added later
- Resolution
  - source resolution default
  - optional scaling target
- Codec
  - AV1
  - HEVC
  - H.264
- Quality preset
  - High quality
  - Balanced
  - Smaller files
- Capture cursor
  - on by default

## Suggested layout

```text
Video
────────────────────────

Frame rate output
  ○ Constant 60 fps

Resolution
  ○ Source resolution
  ○ Scale to: [ ... ]

Codec
  ○ AV1        Recommended
  ○ HEVC
  ○ H.264

Quality
  ○ High quality
  ○ Balanced
  ○ Smaller files

Cursor
  [x] Capture mouse cursor

Advanced
  [ Expand ]
```

## Advanced area

- effective encoder
- NVENC preset
- rate control
- CQ/CQP value
- B-frames
- GOP / keyframe interval
- other explicit overrides

## UX requirements

- Show why AV1 is recommended when available.
- Do not expose unsupported codec choices as valid.
- Keep the standard area simple.
