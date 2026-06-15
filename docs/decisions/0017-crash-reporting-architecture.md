# ADR 0017: Crash Reporting Architecture

## Status

Proposed — implementation scheduled for 0.4.0 (see roadmap). Companion to ADR 0012
(update security model); the two share the official-build gate and the "no secret in the
client" rule.

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
- **Roadmap precondition.** The roadmap only pulls crash *upload* forward once backend, privacy,
  and symbol hosting actually exist. The *local* crash experience has no such precondition.
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
**single-stage**: it shows the already-scrubbed report directly. There is no first "raw view" that
later turns into a separate "anonymized send" window; what the user sees is exactly what would be
sent ("preview equals payload"). There is no silent send and no pre-checked "upload" box.

Dialog contents and actions:

- A plain statement that ExoSnap crashed, plus — when a recording was active — a reassurance that
  the recording was secured and will be restored on next launch (via the ADR 0015 recovery
  manifest).
- A two-column **"What gets sent / Never sent"** transparency block (adopted from the design
  mappe mock): the left column lists exactly the allowlisted fields, the right column the
  categories that are never transmitted (file paths/filenames, recording content, folder names).
  This makes the scrubbing promise visible at a glance, not buried in fine print.
- The scrubbed, read-only report in a **collapsed-by-default** details area (stack frame, exception
  code, app version, GPU/driver, encoder backend), so the dialog is not dominated by a wall of
  text. The raw binary minidump is never shown here.
- Actions: **Restart ExoSnap** (primary), **Report on GitHub** (Stage 0 prefilled issue),
  **Open crash folder** (reveals the raw `.dmp` for users who want it), **Close**. When automated
  upload lights up (Stage 1), a **Send** action joins them, gated and opt-in. (The design mappe
  mock shows a Stage-1 "Send report" primary + "Send automatically next time" — that is the
  *target* state, not the 0.4.0 shipping state, which has no upload backend.)

### Two triggers: an immediate reporter and a next-launch check

Because the crashed process is dead, it cannot draw its own UI. The report surfaces through two
complementary paths:

- **Immediate reporter (so a crash during recording is not missed).** After Crashpad writes the
  dump, a lightweight separate reporter process is launched and shows the dialog at once. It
  **does not hard-steal focus** — it shows its window and calls `FlashWindowEx` (taskbar flash);
  Windows generally blocks foreground theft from a background process anyway, and ExoSnap must not
  barge into an in-progress fullscreen capture by another app.
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
design mappe mock's "anonymous install id"), keeping faith with `PRIVACY.md` ("no account system,
no analytics"). At most a per-report random correlation id may be attached if a future automated
backend genuinely needs to de-duplicate a single user's repeated submissions — never a stable id
that links reports across time.

### Opt-in upload, gated, no secret in the client

Upload is off by default and requires explicit, informed consent (per-report or remembered).
The client binary contains no token or credential, consistent with ADR 0012.

**The upload target is deliberately a gated integration point, not hardcoded in the 0.4.0 local
slice.** GitHub is unsuitable as an *automated* crash-ingest endpoint: it offers no anonymous
minidump ingest, and the Issues API would require a client-side token, which is forbidden.

The preferred automated target is a **self-hosted, Germany-only endpoint** on the maintainer's
own infrastructure (VPS or homelab), which avoids any third-party data processor and the
associated DPA/AVV burden — a strong privacy advantage over SaaS. The concrete software (e.g.
self-hosted Sentry, a lighter Sentry-compatible backend, or a minimal minidump receiver) is a
Crash-C decision and must be verified for native Crashpad *minidump* ingest; self-hosted Sentry
is RAM-heavy and would not fit a small VPS. A hosted SaaS (Sentry) remains a fallback only.

**A crash-ingest endpoint accepts untrusted binary data from arbitrary internet clients and parses
it — a real attack surface.** Automated upload activation is therefore deferred not only until a
concrete target exists and passes privacy review, but until the hosting network is hardened
(logging, intrusion detection, DMZ/VLAN segmentation). Until then automated upload ships compiled
out or hard-disabled, exactly like the official-build update gate, and Stage 0 (assisted GitHub
issue) is the only delivery path.

### Staged delivery: assisted manual report first, automated upload later

Report delivery is a two-stage capability so that 0.4.0 ships something useful with **zero
backend**:

- **Stage 0 — assisted manual report (ships in 0.4.0, no infrastructure).** The crash-report
  dialog presents the scrubbed, plain-text report and offers: *Copy report* (to clipboard),
  *Open crash folder* (reveals the `.dmp` so the user can attach it), and *Report on GitHub*
  (opens a prefilled issue from a `.github/ISSUE_TEMPLATE/crash.*` template with the stack
  trace and allowlisted metadata pre-populated via URL query). The structured **text** travels
  in the prefilled issue; the **minidump binary** cannot — GitHub prefill URLs are length-limited
  (~8 KB) and the Issues API would need a client token — so the user attaches the `.dmp` manually.
  This is an accepted OSS pattern: full consent, full transparency, no third-party data processor,
  and no DPA obligation. Users without a GitHub account fall back to the clipboard copy.
- **Stage 1 — automated upload (later, gated).** When a concrete automated target exists and
  passes privacy review, opt-in automated upload (e.g. Sentry minidump ingest) lights up behind
  the gate above, replacing the manual attach step for users who consent. Stage 0 remains as the
  no-account / declined-upload fallback.

The trade-off is explicit: Stage 0 captures fewer reports (manual steps lower conversion) and
omits the binary unless the user attaches it, but it requires no infrastructure, no account on
ExoSnap's side, and no data-processing agreement. Stage 1 raises fidelity and conversion at the
cost of an external dependency and a privacy review.

### Symbol pipeline via release artifacts, not a custom server

PDBs for each official release are archived by the release pipeline (GitHub Actions) as build
artifacts / release debug assets, keyed by build id and version. Symbolication is performed
offline against that archive, or by uploading symbols to the chosen crash service's symbol store.
No always-on custom symbol server is required for 0.4.0.

### Official-build gating

Crash *upload* is only available in builds that define `EXOSNAP_OFFICIAL_BUILD` (the same gate as
updates). Self-built binaries capture local minidumps — useful to the developer — but never offer
upload and never phone home.

## Consequences

- 0.4.0 can ship the full local crash-capture + report + scrub experience with **no external
  infrastructure**. This is the primary stability win on the path to `1.0`.
- The only piece blocked on an external decision is opt-in upload + automatic symbolication. It
  sits behind a compile/config gate and can light up later without reopening this architecture.
- `PRIVACY.md` already pre-announces crash reporting as opt-in; it must be updated to describe the
  concrete data set and target when upload actually ships.
- Crashpad adds a separate handler executable that must be packaged in the portable ZIP and the
  MSI, and located at runtime (alongside the main binary; resolved by application directory).
- Recovery (ADR 0015) and crash reporting share next-launch real estate; their ordering and the
  "no double-prompt" rule are a hard requirement, covered by tests.
- Crashpad's own license (Apache-2.0 / BSD components) must be vetted against the project's GPL
  model in the Crash A slice before it is wired in, like the x264 gate in ADR 0007.
