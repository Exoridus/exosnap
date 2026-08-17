# Release verification

`scripts/release-verify.ps1` runs the v0.9 release gate as one campaign. Where
`live-verify.ps1` accepts a *build*, this accepts a *release*: it prepares the Windows
environment each scenario needs, drives ExoSnap through its own semantic automation,
validates the output with an independent tool, and stops for a person only where a
person is genuinely irreducible.

This document describes the runner. It does **not** restate what the gates require —
that lives in `docs/release-checklist.md` and the specs, and each scenario cites its
source rather than paraphrasing it.

## The four kinds of truth

The whole design is a consequence of one distinction: some things ExoSnap owns, and
some things Windows, the hardware or the operator owns. Pretending otherwise is how a
suite ends up reporting green for something nobody checked.

| Kind | Who establishes it | How it is proven |
|---|---|---|
| Product truth | ExoSnap | the control channel's typed surfaces, always automated |
| Environment truth | Windows | `exosnap-envctl` transactions, where a documented restorable mechanism exists |
| Physical truth | the operator | they act; the runner verifies the consequence itself |
| Secure truth | UAC | they click; the runner observes before and after |
| Visual desktop truth | the operator's eyes | the runner prepares the state; the person judges it |

None of these boundaries is crossed with a pixel click, a `SendKeys` macro, an
undocumented API or a registry write. A gate that would need one is reported as
manual — which is true — rather than automated, which would not be.

**ExoSnap therefore never grows a Windows administration surface.** There is no
`windows.setHdr`, no `windows.setRefreshRate`, no `windows.setDefaultAudio`, no
`registry.set`, no `shell.execute`. A recording application that can reconfigure the
machine is a different product with a different threat model, and a release runner
wanting to toggle HDR is not a reason to ship one. The mutation lives in
`tools/envctl`, which is test-only: never installed, never linked into `exosnap.exe`,
never a service, never on autostart.

## Running it

```powershell
# Bind the campaign to an explicit artifact. There is deliberately no default:
# a release PASS says "these bytes behaved correctly".
pwsh scripts/release-verify.ps1 prepare -ExePath C:\rc\exosnap.exe -Tag v0.9.0-rc10

pwsh scripts/release-verify.ps1 list           # the catalog, with layers and requirements
pwsh scripts/release-verify.ps1 run            # everything runnable that is not opt-in
pwsh scripts/release-verify.ps1 run -IncludeClass display,audio-physical
pwsh scripts/release-verify.ps1 run -Only REL-CAP-001
pwsh scripts/release-verify.ps1 resume         # re-fingerprint, mark stale, continue
pwsh scripts/release-verify.ps1 retry -Only REL-ENV-003   # re-attempt a FAIL, explicitly
pwsh scripts/release-verify.ps1 status
pwsh scripts/release-verify.ps1 report         # release-verification.json + report.md + junit.xml
pwsh scripts/release-verify.ps1 recover        # restore a dirty environment, and nothing else
```

Run directory: `.workspace/release-verify/<campaign-id>/` (untracked).

Classes marked `[opt-in]` in `list` stay out of a default sweep and need
`-IncludeClass` or `-Only`. A 30-minute mixed-clock recording and a scenario that asks
the operator to unplug an audio interface are not things a runner should start because
somebody typed `run`.

## Results

Two verdicts per scenario, never merged.

**Product verdict** — reuses the Live Verify taxonomy, with two additions:

| State | Meaning |
|---|---|
| `PASS` / `FAIL` | measured |
| `UNVERIFIED` | attempted, outcome unknown (interrupted, evidence unusable) |
| `STALE` | passed once, against an artifact or environment that has since changed |
| `SKIPPED` | deliberately not run, with a recorded reason |
| `DEFERRED` | a human gate nobody could answer — no interactive stdin, `-NonInteractive`, or the operator postponed it |
| `UNAVAILABLE` | this machine cannot offer what the scenario declared it needs |

`DEFERRED` and `UNAVAILABLE` exist because neither is a failure and neither is a pass.
A question nobody was asked has no wrong answer, and "this desk has no 240 Hz mode" is
a statement about the desk. Recording either as `FAIL` makes the report unreadable
exactly where it needs to be trusted.

**Environment-restore verdict** — `NOT_APPLICABLE`, `RESTORED`, `RESTORE_PENDING`,
`RESTORE_PENDING_DEVICE_UNAVAILABLE`, `RESTORE_FAILED`.

A scenario can prove the product correct and still leave a display in the wrong mode.
That is release-relevant on its own, so a product `PASS` with a broken restore is a
`<failure>` in `junit.xml` and gets its own section in `report.md`.

## Staleness

Every terminal result carries the artifact fingerprint and a fingerprint over exactly
the environment keys its scenario declared. `resume` recomputes both and flips
anything that no longer matches to `STALE`.

`UNAVAILABLE` is invalidated by an environment change too, from the other side:
plugging the HDR display in makes the HDR scenario runnable again rather than leaving
it permanently written off.

A `FAIL` is a finding, so `run` and `resume` leave it alone — re-running the sweep can
never quietly erase one. `retry` is the explicit way back, and it drops the old
evidence link so the report never pairs a new verdict with an old artefact.

## Human gates

A gate prints five things, in this order, and none may be omitted:

```
REL-AUD-DEGRADE-001  Remove the recorded audio endpoint, then put it back

Why this is manual:
  A real endpoint loss is a physical or driver-level event. ...

Exact action:
  1. A recording is running RIGHT NOW with system audio enabled.
  2. Unplug the playback device bound to audio.render.normal.
  ...

Expected observable consequence:
  The recording does NOT stop. ...

How this runner will verify it:
  Polls pipeline.snapshot throughout and requires ...
```

Then it waits, **and then it checks for itself**. A gate whose `Verify` block returns
false is a `FAIL` even when the operator typed `done` — an operator can be mistaken
about what they just did, and a gate that trusts the answer instead of the machine is
a checkbox with extra steps. A gate that declares no `Verify` block at all is
`UNVERIFIED`, not `PASS`.

The unanswerable case is checked **before** the instructions are printed, so nobody
performs a two-minute physical action that cannot be confirmed afterwards.

Human gates sit **inside** the environment transaction. An operator who answers FAIL,
aborts, or walks away still leaves the machine restored.

## The environment transaction

Every mutation runs this sequence, and there is no path around it:

```
snapshot exact original
  -> persist recovery journal          (nothing is mutated before this is on disk)
  -> validate desired
  -> apply minimal delta               (a property already at the desired value is skipped)
  -> read back, independently
  -> verify actual == desired          (a setter returning success is not evidence)
  -> [ run the scenario ]
  -> restore exact original
  -> read back
  -> verify actual == original
  -> close
```

Two properties matter more than the rest.

**Restore means "put back what this machine actually had"**, never "set the defaults".
A machine that had HDR on gets HDR on again, not whatever Windows would pick.

**The journal is written before the first mutation.** A kill at any point leaves on
disk what was originally there, what has already changed, and what still has to be put
back. The next runner start restores it and refuses to begin a new mutating scenario
until it has.

**And it is machine-wide, not per campaign** — `.workspace/env-journal.json`, envctl's
own default, overridable only through `EXOSNAP_ENV_JOURNAL` and then for both the tool
and the runner at once. One machine has one environment, so it has one journal. A
journal filed under the campaign that wrote it is invisible to the next campaign,
because the campaign id is new on every `prepare`: the dirty gate finds nothing, the new
campaign snapshots the already-mutated value as its "original", and reports `RESTORED`
for a machine nobody put back.

A failed `begin` is not automatically clean either. It rolls back what it had already
applied, but that rollback can itself fail — envctl says which in `state`, and anything
other than `Clean` or `Restored` (including no state at all) leaves the run dirty. While
a transaction is open the runner also keeps an `exosnap-envctl --guard <pid>` process
alive, which restores from the journal if the runner dies; it is retired after the
restore, and it is a shortcut to recovery rather than a replacement for it.

What this cannot promise: instant recovery from a power loss or an OS crash. Nothing
in user space can. The guarantee is the persistent journal — written durably, so the
bytes reach the disk before the rename — plus a mandatory recovery pass, not an
unfalsifiable claim about surviving loss of power.

Once the environment IS dirty, every later mutating scenario reports `UNAVAILABLE` with
the reason and the way out (`release-verify.ps1 recover`), never `FAIL`. Nothing was
tested, so nothing failed; a page of red would read like a product collapse.

If the original device is gone at restore time the result is
`RESTORE_PENDING_DEVICE_UNAVAILABLE`, and **no other device is substituted**. The
evidence names the stable id, the friendly name, the original value and the remaining
restore action, so reconnecting the device and re-running `recover` finishes the job.

### Refresh rates are Windows' integers, not datasheet numbers

`ChangeDisplaySettingsEx` accepts a nominal `dmDisplayFrequency` it will never report
back. Ask a 59.94 Hz mode for **60** and the setter returns success, and then
`EnumDisplaySettingsEx(ENUM_CURRENT_SETTINGS)` reports the truncated integer **59** —
likewise around the 24/30/120/240 families on many panels. The transaction's read-back
comparison is exact, so it refuses and rolls back. That is the rule working, not a bug,
and it will **not** be softened with a tolerance: "close enough" on the read-back would
hollow out the one guarantee everything above rests on.

The desired value therefore has to be expressible in the vocabulary the read-back
speaks:

```powershell
exosnap-envctl list-modes --alias display.main-hdr    # or --kind display for all of them
```

It prints, per bound display, the `current` mode and every mode
`EnumDisplaySettingsEx` enumerates at that same resolution, colour depth and
orientation — each `refreshHz` verbatim, unrounded. A scenario picks "any supported
rate other than the current one" from that list instead of hardcoding a number that
only exists at one desk. Other resolutions are deliberately not offered: a
refresh-rate change must not become a resolution change, and the coupled-field guard
would refuse one anyway.

The list is necessary but not always sufficient, and the desk this was measured on
shows why: an LG 27GL850 at 2560x1440x32 enumerates **59, 60, 75, 100, 120 and 144**
— 59 and 60 as two separate entries — yet the transaction that asked for 60 read back
59. Windows enumerates the nominal and the actual rate separately and then collapses
them on apply. So prefer a rate with no nominal twin (here 75, 100, 120, 144) and
treat a `verify_mismatch` on one of a 59/60-style pair as the panel's answer, not as a
runner defect.

## Device aliases

Scenarios name **aliases**, never friendly names:

```yaml
requires:
  display: display.main-hdr
  audioRender: audio.render.44100-test
```

A scenario that names "27GL850" can only ever run at one desk — and a friendly name is
not even stable there, because two identical monitors share one. The alias profile is
the only machine-specific file; it maps alias to a **stable Windows identifier**
(DisplayConfig adapter LUID + target id for displays, the MMDevice endpoint id string
for audio). The friendly name survives only as a label in the human gates.

Two failures are reported rather than guessed at:

- `ambiguous_device` — more than one device matches. Nothing is chosen.
- `unbound_alias` — the profile does not bind it. The message says how to bind it.

A test suite that silently picks one of two matching devices is a test suite that lies
about which hardware it exercised.

## The field contract (`REL-SCHEMA-001`)

The catalog reads the control channel's typed surfaces by name, and a name that no
emitter emits **throws** under `Set-StrictMode -Version Latest`. Eight scenarios shipped
doing exactly that — `pipeline.audio.tracks[].degraded` against a snapshot that reports
`audio.sourceDegraded`, `pipeline.avDriftMs` against one that nests it under `avTiming`,
`notifications.notifications[].id` against an `entries[]` array keyed by `sequence`, an
`index` parameter on a `window.moveToScreen` that takes a screen *name*. They survived
because the opt-in scenarios were never executed, so nothing ever evaluated the paths;
several of them would have thrown *inside a human gate*, after the operator had already
unplugged an audio interface or answered a UAC prompt.

`REL-SCHEMA-001` is the early failure mode for that class. It runs first, connects once,
and walks every path in `Get-ReleaseFieldContract` — idle surfaces, then the pipeline
groups with a recording running (they are absent by design while `valid` is false), then
`record.result`. It asserts **existence only**; what a field says is the other scenarios'
business.

Three outcomes, kept apart on purpose:

| Outcome | Meaning |
|---|---|
| present | the path exists |
| missing | **FAIL**, naming the path and, from `UsedBy`, every scenario about to throw |
| empty | the collection's name is proven, its element shape is not — reported as unchecked, never as a pass |

A refused command is reported as its own line too, because "the command said no" and
"the field is gone" call for different work.

Each contract entry names the scenarios that read it, and a unit test enforces that
those scenarios still exist — a `UsedBy` pointing at a deleted scenario is the contract
rotting quietly.

## No synchronisation sleeps

A wait is a `stateRevision` advance, a semantic event, a device notification, a
process handle, or a bounded poll of the actual state. Never a fixed delay standing in
for "it has probably finished by now".

Time-based *product* requirements are a different thing and stay allowed: the
window-capture stall threshold is 10 s, so waiting up to 30 s for the stall
consequence is measuring the product, not guessing at a schedule. Likewise a recording
runs for its configured duration because that is what makes a file with content in it.

## Evidence

Per scenario: the artifact identity, the environment before, what was requested and
applied, the product state, the assertions, any human actions, the environment after
restore, timestamps, both verdicts. Nothing that identifies the person at the machine —
a run directory is evidence other people read.

`report` writes `release-verification.json` (machine-readable release verdict),
`report.md` and `junit.xml` from one state, so the three cannot disagree.

## Tests

`scripts/tests/release-verify.tests.ps1`, registered as CTest
`live_verify.release_runner`.

Everything runs against a **fake** `envctl` that reports whatever the test needs,
including lying about success. That is the point: the failures worth pinning — a
setter that claims it worked while the read-back disagrees, an original device that
vanished mid-restore, a journal left dirty by a killed runner — cannot be produced on
demand by a real display or a real audio endpoint. Nothing in the suite touches the
machine's configuration.
