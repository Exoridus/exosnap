# Logs View

## Purpose

Expose a human-usable history, not just developer text dumps.

## Log categories

- Recording sessions
- Selftests
- Start attempts
- Blocked starts
- Markers
- Splits
- Device changes
- Errors
- Config changes

## Suggested layout

```text
Logs
────────────────────────

Sessions
[ 2026-05-10 21:14 — Game.exe — 00:42:11 — Completed ]
[ 2026-05-10 19:02 — Game.exe — 00:03:04 — Failed: disk full ]

Events
[ 21:16:08 — Marker added at 00:02:14 ]
[ 21:22:19 — Split created: segment 2 ]
[ 21:31:04 — MIC default device changed ]

[ Export selected ]
[ Open log folder ]
```

## Requirements

- Logs must be structured internally.
- UI should show readable summaries.
- Export should support support/debug workflows.
