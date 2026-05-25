# ADR-0007: No Overlay/HUD in MVP

## Status
Accepted

## Decision
Do not implement an overlay/HUD in the MVP.

## Rationale
The Record view can show the same live information during recording. An external overlay adds separate compatibility and presentation complexity without improving the core recording pipeline.

## Consequences
- MVP prioritizes Hotkeys over Overlay settings
- Future overlay design must consume the existing telemetry model rather than invent a parallel one
