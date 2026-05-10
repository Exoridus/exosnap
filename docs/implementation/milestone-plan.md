# Milestone Plan

## M0 — Specification Freeze

### Goal
Establish product, architecture, UI, and workflow baselines.

### Deliverables
- canonical docs
- ADRs
- agent instructions

### Exit criteria
- no unresolved MVP-level product decision blocks implementation

---

## M1 — Repository Foundation ✅

*Status: completed. See [M1 Implementation Notes](notes/M1.md).*

### Goal
Create a clean engineering baseline.

### Deliverables
- CMake setup
- build presets
- CI
- formatting/linting
- test framework
- logging skeleton
- project skeleton

### Exit criteria
- clean clone builds and tests successfully

---

## M2 — Capability Probes

### Goal
Prove risky low-level technologies in isolation.

### Deliverables
- WGC preview probe
- WASAPI / Process Loopback probe
- NVENC probe
- Opus + MKV probe
- storage probe

### Exit criteria
- every core technology is demonstrated independently

---

## M3 — Core Models and Config

### Goal
Freeze the core domain model before large integration work.

### Deliverables
- settings models
- resolved recording plan
- audio-row resolver
- diagnostics models
- session-state models
- serialization

### Exit criteria
- UI and engine share canonical models
- resolver logic is unit-tested

---

## M4 — Audio Engine

### Goal
Implement APP/MIC/SYS capture and track resolution end to end.

### Deliverables
- source capture modules
- default/explicit device binding
- meters
- mute/silence/clip state
- Opus encode path
- track resolver integration
- AAC support as needed for MP4

### Exit criteria
- default APP/MIC/SYS recording works
- reorder/merge combinations resolve correctly
- no APP/SYS duplication bug

---

## M5 — Video Engine

### Goal
Implement source capture, preview, CFR output, and hardware encode.

### Deliverables
- WGC production capture
- preview feed
- QPC-based frame timing
- CFR 60 scheduler
- AV1/HEVC/H.264 encode
- video telemetry

### Exit criteria
- variable-rate source becomes stable CFR 60 output
- duplicate/drop metrics are correct

---

## M6 — Session, Muxing, and Output

### Goal
Create complete recordings from engine API.

### Deliverables
- session lifecycle
- MKV output
- MP4 output
- codec/container validation
- split recording
- marker handling
- file naming and destination handling

### Exit criteria
- full recording works headlessly through an integration harness

---

## M7 — Diagnostics and Selftest

### Goal
Make readiness explicit and enforceable.

### Deliverables
- diagnostic checks
- blocker propagation
- selftest
- expandable explanation data
- report export

### Exit criteria
- invalid configurations cannot start
- readiness states are correct

---

## M8 — UI MVP

### Goal
Build the complete MVP user interface on stable engine APIs.

### Deliverables
- Record
- Video
- Audio
- Output
- Hotkeys
- Diagnostics
- Logs
- Advanced

### Exit criteria
- full ordinary workflow can be completed through UI

---

## M9 — Hardening

### Goal
Turn the MVP from working into usable.

### Deliverables
- long-run tests
- source-loss tests
- device-change tests
- storage pressure tests
- hotkey conflict tests
- improved logs
- docs cleanup

### Exit criteria
- first usable release candidate

---

## M10 — First Usable Release

### Goal
Ship the first coherent release.

### Exit criteria
- docs current
- known limitations documented
- smoke tests passing
- no critical blocker left unresolved
