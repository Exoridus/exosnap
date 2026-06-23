# ADR 0033: Diagnostics Engine, FixAction Model, and PresentProvider

## Status

Proposed — scopes the 0.8.0 "Diagnostics as a feature" wave (resequenced ahead of Software
encoding and AMD/Intel; see `docs/roadmap.md`). Builds on the existing `app/diagnostics`
scaffold (`DiagnosticResult`, `RecommendationEngine`, `SelfTestRunner`, the disk/filesystem
providers) and the capability layer. Relates to [[0010-media-compatibility-registry]] (the
combo source of truth), [[0017-crash-reporting-architecture]], [[0002-structured-app-log-buffer]],
[[0015-recovery-finish-continue-delete]], and [[0022-edit-output-save-surface]] (the post-flight
"Review" consumer).

## Context

The signals needed for real diagnosis already exist but are scattered and under-exploited:
capability/runtime facts (NVENC/AAC presence, driver/API versions), disk/filesystem providers,
the error→user-message mapping, recovery manifest, crash reports, the structured log buffer, and
live pipeline byproducts (capture/encode/PTS timestamps, drop counters, A/V-drift, encoder
backpressure, committed-byte rate). `DiagnosticResult` already carries `summary`/`detail`/
`current_value`/`recommendation`/`optional_fix`/`affected_features`, and `RecommendationEngine`
already runs config-time checks (refresh-rate mismatch, MP4 crash resilience, codec availability,
drive space, FAT32, profile support).

The gap is not raw signals; it is (1) turning `optional_fix` from a description into an executable
action, (2) adding live (during-recording) and pre-flight phases, (3) correlating signals into a
named root cause rather than listing numbers, and (4) elevating the page UX. The goal: diagnostics
that perform deep analysis, surface problems legibly to power users, describe solutions, and offer
one-click fixes where safe.

A guiding insight drives scope: most failures **we** have hit (audio crackling, A/V offset, duration
mismatch, NVENC P6 no-frames, hvcC metadata) were our own bugs, now fixed and covered by regression
tests. A runtime check for a fixed internal bug can never fire — it is dead weight. The checks that
earn their place are **environment/config conditions that recur per user** (driver, disk, filesystem,
unsupported-on-this-GPU, format mismatch, VRR/refresh mismatch) and manifest **live or pre-flight**.

## Decision

### Diagnostic + FixAction model

Extend the existing model rather than replace it. `optional_fix` (string) is superseded by an
optional structured action:

```
struct FixAction {
  std::string id;
  std::string label;              // "Switch to a supported codec for this container"
  enum class Safety { Auto, Assisted, External } safety;
  bool reversible;
  std::string changes_summary;    // shown in a preview/confirm before applying
  // engine-side: a command token the UI invokes; the engine performs the change
};
```

- **Auto** — safe, reversible, config-only (e.g. `ReconcileCodecs()`'s "nearest valid combination",
  create output dir, lower CQ, pick the available audio device, clear a stale recovery file).
  Config-changing auto-fixes are never applied silently: `changes_summary` drives a confirm/preview.
- **Assisted** — opens the right Settings section pre-focused, opens a folder, copies a command.
- **External** — cannot be performed by the app (driver install): show the exact required version and
  a deep link only.

The engine stays UI-agnostic: a pure check registry + value model; the UI renders the prioritized
list (worst first), one-line summary + severity color, expandable evidence (`current_value` + log
excerpt), `recommendation`, and a `FixAction` button when present.

### Three phases

- **Pre-flight** — run all blocker+notice checks before recording; green/amber/red readiness with
  fixes. Blockers already gate recording start.
- **Live** — instrumentation taps that cost O(1) per frame with no hot-path allocation; aggregation
  (running counters, ring buffers, EWMA) happens off-thread and updates the UI at ~1–4 Hz (the
  existing meter cadence). Forbidden on the hot path: per-frame image analysis, decoding frames back,
  SSIM/VMAF, re-reading the output file. Diagnostics is a **consumer** of signals the pipeline
  already produces, never added producer-thread work.
- **Post-flight** — intentionally minimal in this wave: a report card surfacing the already-accumulated
  live stats (drop %, achieved vs target FPS/bitrate, max drift, file valid). The **full** post-flight
  integrity analysis is the content of the 0.9.0 Edit/Output/Save "Review" step and lands there.

### Three-level detection

- **Level 1 — raw signals** (measured): capture/source-present cadence, encode latency, drop count,
  drift ms, queue depth, disk write rate, RC stats, audio underruns, monitor mode/VRR, driver/NVENC
  versions.
- **Level 2 — derived metrics** (cheap math): drop %, achieved/target FPS, drift trend, and an
  encoder-bound vs capture-bound vs disk-bound classification.
- **Level 3 — correlation / root cause**: combine Level-2 + config + environment into a named diagnosis
  with a confidence and a remediation. First showcase: **VRR/refresh-rate vs CFR-capture judder** —
  extends `checkRefreshRateMismatch` from a static config check to a live correlation using DXGI-OD
  `DXGI_OUTDUPL_FRAME_INFO.LastPresentTime`/`AccumulatedFrames` (source present cadence + variance) +
  NVAPI VRR state + our configured CFR rate. Distinguished from encoder-bound / disk-bound / preview-lag,
  which have their own signals.

### Signal sources and the PresentProvider

Baseline sources need **no new dependency**: internal pipeline counters, DXGI-OD frame info, NVAPI
(VRR/refresh; `monitor_refresh_rate` is already a `RecommendationEngine` input), and the existing
capability probe. These cover the monitor-capture path and most of the failure taxonomy.

Per-game **present-mode and tearing** are only robustly available via ETW present tracing — i.e.
**PresentMon** (Intel, MIT-licensed; the engine behind NVIDIA FrameView). FrameView itself is an
end-user app, not embeddable; PresentMon is. It slots behind a `PresentProvider` interface (same
provider pattern as `DiskSpaceProvider`/`FilesystemProvider`) as an **optional, opt-in, elevation-gated**
provider. The engine functions without it; PresentMon enriches the window/game-capture path (where
DXGI-OD frame accounting is unavailable) and adds tearing/present-mode.

### Elevation, portability, and anti-cheat posture

Realtime ETW present sessions require elevation (or `Performance Log Users` membership). Rather than a
privileged background service (which breaks the portable ZIP story), the design is:

- **Detect elevation at runtime** (process token integrity level). When not elevated, present-diagnostics
  render a **disabled state with a hint** ("Restart as Administrator to enable present/tearing
  diagnostics") plus an explicit **opt-in toggle**.
- When elevated **and** the toggle is on, the app opens the ETW session directly — **no service install**,
  so this works fully for the **portable** distribution (elevation is per-launch, not an installed
  component). Non-elevated portable use degrades gracefully.
- **Do not run elevated by default.** An elevated process loses drag-drop from the non-elevated shell
  (UIPI) and routinely running a recorder as admin is poor practice. Elevation is an on-demand
  "relaunch to unlock" path, not the default launch mode.
- **The "Elevated mode" control is a relaunch, not an in-place change.** A process's integrity level
  is fixed at creation, so a Settings toggle (or toggling on a feature that needs elevation) triggers a
  self-relaunch via `ShellExecuteEx`/`runas` (UAC consent) and exits the current instance. This is
  offered through a NotificationHub action ("This needs elevated mode — restart as administrator?")
  rather than a blocking modal. Constraints: never relaunch during an active recording (defer the offer
  until stop, cf. [[0012-update-security-model]]'s no-update-during-recording rule); hand off state
  (settings + current view + a flag to re-enable the toggled feature) so the restart is seamless; handle
  UAC decline gracefully (stay non-elevated, feature stays disabled).

**Elevation is useful beyond PresentMon**, so the relaunch unlocks a coherent bundle, not a single
feature: capturing **games/apps that themselves run elevated** (UIPI blocks a non-elevated capturer from
WGC-hooking a higher-integrity window — a real, common failure that looks like "black capture"), and
optionally raising process/thread scheduling priority (note: `HIGH_PRIORITY_CLASS` and MMCSS task
registration need no elevation; only `REALTIME_PRIORITY_CLASS` does, and it is rarely advisable). The
OBS "run as admin" folklore is mostly these two effects — chiefly elevated-game capture — not a magic
stability switch. A useful smart-diagnostic falls out of this: detect "you are capturing an elevated
window but are not elevated → relaunch as Administrator," with a `FixAction` (Assisted: relaunch elevated).

On **anti-cheat**: ETW present tracing does **not** inject into or read the game process — it consumes
kernel/DWM events — so it stays in the same low-risk class as our DXGI-OD/WGC capture (the same APIs OBS
uses). Elevation-gating + an explicit opt-in toggle is therefore **consent and transparency**, not a
technical anti-cheat shield: nothing system-wide runs silently, and cautious users can leave present
diagnostics off during competitive play and enable them only when troubleshooting. This matches the
project's standing posture (no injection; info + opt-out rather than auto-disable).

## Consequences

- The diagnostics engine becomes the framework later waves register into: each new encoder/vendor
  (0.11.0 software, 0.12.0 AMD, 0.13.0 Intel) contributes checks + a capability/diagnostics provider
  instead of bolting on ad-hoc error handling.
- Reduced support burden for the shipping NVIDIA-first product; unavoidable-but-confusing capture
  problems (VRR judder, encoder/disk-bound drops, preview-lag confusion) become named and fixable.
- New dependency only if the PresentMon provider is built: PresentMon (MIT — compatible with the
  project's GPL model, far simpler than x264's GPL+patent gate). It is optional and capability-gated;
  the portable build degrades gracefully without elevation.
- Diagnostics may surface **product** improvements, not just user fixes — e.g. recommending VFR or
  display-matched capture for VRR/>refresh users instead of the stock CFR-60 default.

## Alternatives considered

- **PresentMon as a hard dependency / always-on service.** Rejected: elevation/service install breaks
  the portable distribution and forces privilege on users who do not need present diagnostics.
- **Post-flight bug-replay checks for past incidents.** Rejected: checks for already-fixed internal bugs
  can never fire and are dead weight; regression tests own that surface. Post-flight is limited to a
  report card here, with full integrity review deferred to the 0.9.0 Review step.
- **Per-frame image-quality analysis (SSIM/VMAF) live.** Rejected on the hot path; belongs to an offline
  quality-validation matrix (1.0 gate), not live diagnostics.
