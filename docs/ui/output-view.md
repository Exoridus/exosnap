# Output View

## Core controls

- Container
  - MKV default
  - MP4 secondary
- Audio codec
  - choices depend on container
- Destination directory
- File name pattern
- Optional subfolders
- Estimated size / hour

## Suggested layout

```text
Output
────────────────────────

Container
  ○ MKV
  ○ MP4

Audio codec
  [ Opus ▼ ]   // choices depend on container

Destination
  [ D:\Recordings\ ] [Browse]

File naming
  [ {date}_{time}_{app}_{profile} ]

Estimated output
  Current bitrate: 42.1 Mb/s
  ≈ 18.5 GB / hour
```

## Container/audio compatibility behavior

### MKV
Expose:
- Opus
- AAC
- optional PCM if implemented

### MP4
Expose only compatible options.
MVP default:
- AAC

## Container-switch behavior

### MKV → MP4
- If current audio codec is invalid for MP4, switch automatically to a valid MP4 codec.
- Do not show invalid codec choices.

### MP4 → MKV
- Restore last valid MKV selection if remembered.
- Otherwise choose Opus.

## MP4 information text

When MP4 is selected, show a calm informational note:

> MP4 is less crash-resilient than MKV. If recording is interrupted unexpectedly, the file may require recovery or be unusable.

This is not a warning state and does not block recording.
