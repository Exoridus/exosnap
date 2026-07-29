# ADR 0017: Crash Reporting Architecture

## Status

Accepted — implemented in 0.4.0 (see roadmap). Companion to ADR 0012
(update security model); the two share the official-build gate and the "no secret in the
client" rule.

**Updated 2026-06:** Self-hosted Germany-only endpoint superseded by **Sentry SaaS (EU data
residency)**. Self-hosted Sentry was rejected as too RAM-heavy for a small VPS. Stage 1
(automated opt-in upload) is now in scope for 0.4.0 — not deferred — subject to the consent
gate and the privacy controls enumerated in this ADR.

**Updated 2026-06 (0.4.0 release plumbing):** Stage 1 (Sentry EU SaaS) is **provisioned and
active in official builds**. The Sentry DSN is compiled in under `EXOSNAP_OFFICIAL_BUILD`,
ingests to the EU (`.de`) endpoint, and remains consent-gated and opt-in (off by default). What
ships in 0.4.0 is the **next-launch crash-report dialog** (detect dump on startup → scrubbed
report → consent-gated Send). The **immediate reporter (Crash A2)** — a separate process that
shows the dialog the instant a crash is written — is **deferred to a later release**; until then
the next-launch path is the sole surface. **Symbol upload via `sentry-cli` (Crash C) is deferred**
pending a Sentry auth token; in the interim, release PDBs are archived as CI artifacts for
offline symbolication.

**Updated 2026-06 (0.7.0 — non-fatal recording-error reporting):** The same consent-gated,
scrubbed pipeline is extended to **non-fatal recording failures** (a failed Record attempt, not a
process crash). See *Non-fatal recording errors reuse the crash pipeline* below.

**Updated 2026-07 (crash-dialog action simplification):** The next-launch dialog has one explicit
delivery path: consent-gated Sentry upload in official builds. The unused Stage-0 prefilled GitHub
issue builder and its overflow-menu action were removed. The action row is now `Send report`,
`Don't send`, and a directly visible tertiary `Open crash folder`; self-builds still retain local
minidumps and the folder action but never offer upload.

**Updated 2026-07 (transactional policy/privacy disclosure):** The old Boolean consent preference
is replaced by `AskEveryTime | AlwaysSend | NeverSend`. The next-launch dialog now leads with
truthful local session evidence, distinguishes the structured event from the native dump, and
commits Remember choices only on an explicit Send/Don't send action.

## Context

ExoSnap is a native C++/Qt application doing real-time capture, encode, and mux. The defects
that matter most on the road to a reliable `1.0` — driver faults, encoder edge cases, GPU TDR,
muxer corruption under load — surface as native crashes. Today a native crash produces nothing
actionable: there is no minidump, no stack, no record that it happened.

Constraints that shape the decision:

- **Strict privacy posture.** `PRIVACY.md` states ExoSnap makes no network connections during
  normal operation, has no telemetry, and that any future transmission is opt-in and off by
  default. Crash reporting must not weaken this.
- **No standing infrastructure.** As of 0.4.0 planning there is no crash backend, no symbol
  hosting, and no signing/CA setup. The explicit goal is to avoid running custom servers when a
  hosted free-tier service or GitHub can do the job.
- **Upload target now identified.** Stage 1 (automated opt-in upload) targets Sentry SaaS with EU
  data residency; this resolves the earlier open question. The *local* crash experience has no
  infrastructure precondition regardless.
- **Existing recovery machinery.** ADR 0015 already defines a recovery manifest + startup
  recovery overlay for unfinished recordings. Crash reporting must coordinate with it, not
  duplicate or fight it.

## Decision

### Crashpad as the out-of-process crash handler

Use Google Crashpad, vendored via FetchContent like libmatroska/libebml. The handler runs
out-of-process: an in-process handler cannot reliably execute after heap corruption or stack
overflow, which are exactly the cases worth capturing. Crashpad writes a minidump plus
structured metadata to a local database under `%LOCALAPPDATA%\ExoSnap\crashes\`.

### Local-first: capture works with zero infrastructure

Minidump capture, the local crash database, the report dialog, and privacy scrubbing all
function fully offline. Nothing is transmitted without explicit consent. This is the deliverable
that ships in 0.4.0 **regardless of backend availability**, and it is independently valuable:
a developer (or a self-builder) gets a real minidump to analyze even with upload compiled out.

### Crash reporting and recording recovery are coordinated, not merged

A crash mid-recording must cooperate with the recovery manifest (ADR 0015). The Crashpad handler
runs after in-process state is gone, so the recovery manifest remains the single source of truth
for unfinished recordings; the crash report *references* recovery state rather than duplicating
it. On next launch the two surfaces must not double-prompt: **recovery runs first (it owns user
data), the crash-report dialog second (it owns diagnostics).** The ordering is explicit, not
incidental.

### The report dialog is separate, single-stage, and explicit

A dedicated crash-report dialog — distinct from the recovery overlay — presents the crash. It is
**single-stage** and evidence-first. There is no first raw view followed by a second consent
window, no pre-checked upload box, and no claim that local summary fields are a byte-for-byte
preview of both Sentry channels.

Dialog contents and actions:

- A plain statement that the previous session did not shut down normally. The next-launch path
  does not infer a crash cause from the missing clean-exit marker.
- Recovery availability in the present tense, followed by a flat summary of dump availability,
  unavailable local cause, previous-session version and non-empty encoder/container/codec context.
  Empty exception/module/thread/stack fields are omitted.
- A collapsed-by-default, keyboard-accessible **What is included in this report?** disclosure.
  It states that the native dump is separate from the privacy-scrubbed structured event, can carry
  loaded-module/install paths, and is sent to Sentry's EU region only with consent.
- Actions: **Send report** (primary, present only when Sentry upload is active), **Don't send**
  (secondary), and a directly visible **Open crash folder** tertiary action. There is no overflow
  menu and no prefilled GitHub-issue action.
- An unchecked **Remember this choice for future crashes** draft. Toggle alone has no persistence
  or consent side effect. Send+remember commits AlwaysSend; Don't send+remember commits NeverSend;
  X, Escape and backdrop close commit nothing.

### Two triggers: an immediate reporter and a next-launch check

Because the crashed process is dead, it cannot draw its own UI. The report surfaces through two
complementary paths:

- **Immediate reporter (so a crash during recording is not missed).** *(Deferred past 0.4.0.)*
  After Crashpad writes the dump, a lightweight separate reporter process is launched and shows the
  dialog at once. It **does not hard-steal focus** — it shows its window and calls `FlashWindowEx`
  (taskbar flash); Windows generally blocks foreground theft from a background process anyway, and
  ExoSnap must not barge into an in-progress fullscreen capture by another app. 0.4.0 ships the
  next-launch path only; the immediate reporter lands in a later release.
- **Next-launch check (robust fallback).** On every startup ExoSnap checks for an unhandled dump.
  If the immediate reporter was missed (fullscreen, dismissed, machine powered off), the dialog
  appears then, after the recovery overlay. A dump handled by the immediate reporter is marked so
  the two paths never double-prompt for the same crash.

### Privacy scrubbing before display or upload

Before the report is shown or sent, scrub identifying data from the structured metadata:
filesystem paths (usernames in `%USERPROFILE%`, chosen output paths), machine name, and
recording file names, each replaced with a stable placeholder. Use an explicit **allowlist** of
system fields that may be included (OS build, GPU model/driver version, app version, active
encoder backend, container/codec) rather than a denylist of fields to remove. The minidump call
stack is retained but the surrounding metadata is annotated as scrubbed.

**No persistent install identifier.** The report carries no device-persistent id (rejecting the
design mappe mock's persistent install id, keeping faith with `PRIVACY.md` ("no account system,
no analytics"). At most a per-report random correlation id may be attached if a future automated
backend genuinely needs to de-duplicate a single user's repeated submissions — never a stable id
that links reports across time.

### Opt-in upload, gated, no secret in the client

Upload is off by default and requires explicit, informed consent (per-report or remembered).
The client binary contains no token or credential, consistent with ADR 0012.

The persisted policy has three values: `AskEveryTime` (default), `AlwaysSend`, and `NeverSend`.
Legacy `auto_send_crash_reports=true` migrates to AlwaysSend; false/missing migrates to
AskEveryTime. NeverSend suppresses the consent prompt only; ADR 0015 recovery remains independent.
Settings exposes all three values and reconciles SDK consent immediately.

sentry-native 0.15 stores consent at SDK/database scope. A one-shot Send therefore calls give,
flushes the pending transport work with a bounded timeout, and only then calls
`sentry_user_consent_reset()` to return to unknown. Resetting immediately after give could strand
the envelope; leaving consent given would authorize later same-session reports. Remembered
AlwaysSend uses persistent give; Ask uses reset; NeverSend uses revoke.

**The chosen automated upload target is Sentry SaaS with EU data residency (EU/Germany ingest).**
GitHub is unsuitable as an *automated* crash-ingest endpoint: it offers no unauthenticated minidump
ingest, and the Issues API would require a client-side token, which is forbidden. A self-hosted
Germany-only endpoint was considered but rejected: self-hosted Sentry is too RAM-heavy for a small
VPS, and lighter alternatives would require building and hardening custom ingest infrastructure.
Sentry SaaS with EU data residency satisfies the privacy posture via a Data Processing Agreement,
keeps data within the EU, and is available without standing up custom servers.

**Privacy controls for Stage 1 (Sentry EU SaaS):**

- `require_user_consent=1` — nothing is transmitted without explicit opt-in; upload is off by
  default and requires per-report (or remembered) consent.
- `enable_logs=0` — Crashpad breadcrumb logs are not collected.
- `debug=0` in Release builds.
- `before_send` scrubbing via an **allowlist**. Today app code populates encoder backend,
  container, video codec and audio codec. OS/GPU/app-version tag keys are allowlisted but not
  currently populated. Usernames, file paths (including output path and recording filenames), and
  the machine name are stripped from the structured event.
- The Crashpad minidump is a separate binary channel and can contain loaded-module paths,
  including a username-bearing install path for a portable install under `%USERPROFILE%`.
- Crash database at `%LOCALAPPDATA%\ExoSnap\crashes\`.
- The Sentry DSN is compiled in **only** under `EXOSNAP_OFFICIAL_BUILD`; self-builds never phone
  home. The DSN is a write-only ingest key (not a secret in the ADR 0012 sense — it cannot read
  data back from the project's Sentry org).
- **No persistent install identifier.** No stable device-level id is generated or stored; at most
  a per-report random correlation id may be attached for a single submission's de-duplication.
- Sentry org-level Security & Privacy settings are all ON: Require Data Scrubber, Require Default
  Scrubbers, Prevent Storing IP Addresses.

Automated upload is compiled out or hard-disabled in the absence of `EXOSNAP_OFFICIAL_BUILD`,
exactly like the official-build update gate.

### Delivery: consent-gated Sentry upload, local fallback

Official builds offer automated opt-in upload to Sentry with EU data residency. The DSN is
compiled in behind `EXOSNAP_OFFICIAL_BUILD`; both that gate and explicit user consent must pass.
Self-builds and users who decline retain the local minidump and can reveal it through **Open crash
folder**, but the app does not prepare or transmit an alternate GitHub issue. The remaining Crash C
work — automated `sentry-cli` symbol upload — is deferred pending a Sentry auth token (PDBs are
archived offline in the interim).

### Symbol pipeline via release artifacts, not a custom server

PDBs for each official release are archived by the release pipeline (GitHub Actions) as build
artifacts / release debug assets, keyed by build id and version. Symbolication is performed
offline against that archive, or by uploading symbols to the chosen crash service's symbol store.
No always-on custom symbol server is required for 0.4.0.

**0.4.0 state:** the release pipeline builds with `EXOSNAP_RELEASE_PDB=ON` so the Release linker
emits a separate `.pdb`, then uploads the PDBs as a versioned CI artifact (`...-pdb-<run id>`). This
is the **offline** symbolication path and ships in 0.4.0. **Automated `sentry-cli` symbol upload to
the Sentry org/project `exosnap` (so server-side minidump symbolication "just works") is deferred
(Crash C)** — it needs a Sentry auth token provisioned as a CI secret. A `TODO(Crash-C)` in the
release workflow marks the exact insertion point.

### Non-fatal recording errors reuse the crash pipeline

A *recording failure* (validation rejects the config, the encoder/muxer aborts mid-session) is
recoverable and the process is still alive to draw its own UI — so it is **not** a Crashpad crash.
But the same privacy machinery applies, so it reuses this architecture rather than inventing a
parallel one:

- **Surface — in-window modal, not a toast.** A failed Record attempt opens a modal
  `RecordingErrorOverlay` (a scrim + centered card parented to the central widget, mirroring the
  crash overlay / AboutOverlay; no native OS dialog). It shows a plain warning, the failure
  **phase / HRESULT / detail**, and the container/codec in play. This replaces the prior
  fire-and-forget "stopped unexpectedly" toast for engine failures, which was easy to miss or
  mistake for the (coral) recording chrome. The **disk-space auto-stop keeps its own actionable
  "Storage running low" notification** and is *not* routed to this modal — it is a user-actionable
  condition, not a defect.
- **Report API — `crash_capture::ReportNonFatalError(phase, detail)`.** A new UI-agnostic entry
  point emits a structured Sentry **message event** (`SENTRY_LEVEL_ERROR`, logger
  `recording.failure`). Because the `before_send` hook scrubs exception/tag fields but **not** the
  message body, this function **pre-scrubs both arguments via `ScrubString`** before they leave the
  process — a path leaking through `detail` (e.g. "output directory does not exist: C:\Users\…")
  would otherwise upload raw. Container/codec context rides along as the existing allow-listed tags
  (`SetEncoderContext`).
- **Same gates.** No-op without a compiled-in DSN (`EXOSNAP_OFFICIAL_BUILD`) and without consent;
  sentry-native suppresses capture until `GiveUserConsent()`. The modal's **"Send report" action is
  hidden entirely when `crash_capture::IsActive()` is false** (every self-build), so the opt-in is
  never offered where it cannot function. The user clicking Send *is* the per-report opt-in, exactly
  as in the crash dialog.

This keeps one privacy story (allow-list, scrub-before-send, consent gate, official-build gate) for
both fatal and non-fatal diagnostics.

### Official-build gating

Crash *upload* is only available in builds that define `EXOSNAP_OFFICIAL_BUILD` (the same gate as
updates). Self-built binaries capture local minidumps — useful to the developer — but never offer
upload and never phone home.

## Consequences

- 0.4.0 can ship the full local crash-capture + report + scrub experience with **no external
  infrastructure**. This is the primary stability win on the path to `1.0`.
- The only piece blocked on an external decision is opt-in upload + automatic symbolication. It
  sits behind a compile/config gate and can light up later without reopening this architecture.
- `PRIVACY.md` already pre-announces crash reporting as opt-in; it is updated in 0.4.0 to describe
  the concrete data set (allowlist), the Sentry EU SaaS target, and the consent gate.
- Crashpad adds a separate handler executable that must be packaged in the portable ZIP and the
  MSI, and located at runtime (alongside the main binary; resolved by application directory). In
  0.4.0 this is wired: `install(FILES crashpad_handler.exe DESTINATION ".")` places it at the
  install-tree root, the portable ZIP packages it automatically, and the MSI bundles it via a
  conditional `IncludeCrashpad` component (present only when built with crash capture ON, so an OFF
  self-build still packages cleanly). A dedicated CI job builds the ON configuration on `main` /
  manual dispatch / a `crash-capture`-labeled PR to keep the strand from silently breaking.
- Recovery (ADR 0015) and crash reporting share next-launch real estate; their ordering and the
  "no double-prompt" rule are a hard requirement, covered by tests.
- Crashpad's own license (Apache-2.0 / BSD components) must be vetted against the project's GPL
  model in the Crash A slice before it is wired in, like the x264 gate in ADR 0007.
