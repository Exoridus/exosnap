# Video View

## Standard controls

- Output frame rate
  - CFR 60 fps (fixed for MVP)
- Resolution
  - source resolution default
  - optional scaling target
- Codec
  - AV1
  - HEVC
  - H.264
  - Hardware encode state is represented in this section (`HARDWARE ENCODE`)
- Quality preset
  - High quality
  - Balanced
  - Smaller files
- Capture cursor
  - on by default
  - control is hosted on Video view (not on Record view)

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
- Output frame rate is fixed at CFR 60 in MVP. Alternative output rates
   are out-of-scope, including under Advanced.
