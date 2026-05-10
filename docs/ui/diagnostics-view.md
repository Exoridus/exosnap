# Diagnostics View

## Goal

Make recording readiness inspectable, actionable, and explainable.

## Suggested layout

```text
Diagnostics
────────────────────────

Overall status: Ready with 2 notices
Last check: 10 May 2026, 21:14

[ Run System & Pipeline Check ]
[ Export Diagnostic Report ]

0 Blockers   2 Notices   18 Passes

Operating System
GPU & Encoder
Display
Audio
Storage
Pipeline
Settings Compatibility
```

## Result rows

Each row shows:
- severity
- title
- short summary
- optional action
- expandable detail

## Expanded detail structure

1. What is this?
2. Why does it matter?
3. What was detected?
4. What are the options?
5. What is recommended here and why?

## Required groups

- Operating System
- GPU & Encoder
- Display
- Audio
- Storage
- Pipeline
- Settings Compatibility

## Start blocking

Any active blocker must be reflected here and in the Record view. Recording start stays disabled while blockers remain.
