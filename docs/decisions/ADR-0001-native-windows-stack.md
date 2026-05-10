# ADR-0001: Native Windows Stack

## Status
Accepted

## Decision
Use a native Windows-oriented stack:
- C++ engine
- WinUI 3 application UI
- D3D11-centered GPU path
- Windows-native capture and audio APIs

## Rationale
The app depends on native Windows capabilities, low-level timing, GPU resources, and diagnostics. A native architecture minimizes friction at the integration boundaries.

## Consequences
- Windows-only MVP
- More control and lower abstraction leakage
- Need for disciplined API boundaries between UI and engine
