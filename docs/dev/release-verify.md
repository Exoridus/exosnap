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

## Which harness runs a gate

The campaign is being migrated to the typed C# harness under `tools/release-verify`
(ADR 0070). The two exist side by side while that happens, and the runner says which
one it is using rather than leaving a reader to guess.

```powershell
pwsh scripts/release-verify.ps1 run                    # the PowerShell campaign (default)
pwsh scripts/release-verify.ps1 run -Engine DotNet     # the typed harness
```

`-Engine DotNet` builds and publishes `ExoSnap.Verify` if it is missing or older than
its sources, then forwards `prepare`, `run`, `list`, `qualify` and `report` to it. It
refuses `recover`, `resume`, `retry` and `status`, which have no counterpart there yet
-- answering them with something that merely looks similar would be worse than saying
so. PowerShell stays the default until every gate has moved: a default that silently
ran a harness with fewer gates than the checklist names would produce a record about a
smaller bar than the one a reader assumes.

Both engines write `release-verification.json` in the same shape, and
`scripts/check-release-qualification.ps1` is the one reader that decides whether either
may promote a release. `scripts/tests/verify-harness-record.tests.ps1` feeds a record
the C# producer actually wrote through that reader, so the two cannot drift apart
unnoticed.

### Migration status

| Gate | C# body | Notes |
|---|---|---|
| `REL-ENV-001` | migrated | envctl `describe`, asserted over the catalogue |
| `REL-ENV-002` | migrated | envctl `resolve-aliases` |
| `REL-ENV-003` | migrated | full transaction, restore in a `finally` |
| `REL-SCHEMA-001` | migrated | the field contract, across all three stages |
| `REL-PRESENT-001` | migrated | control channel only |
| `REL-PRESENT-002` | declared | needs the elevated worker (slice 2) |
| `REL-PRESENT-XCHECK-001` | migrated | new gate; PresentMon as an independent oracle |
| `REL-CAP-001` | migrated | control channel plus ffprobe |
| `REL-CAP-STALL-001` | declared | operator-assisted |
| `REL-CAP-QUIET-001` | migrated | needs `probe_stall_window` |
| `REL-CAP-FSE-001` | declared | operator-assisted |
| `REL-AUD-DEGRADE-001` | declared | physical (slice 4) |
| `REL-AUD-SILENCE-001` | declared | operator-assisted |
| `REL-AUD-FORMAT-001` | declared | audio device state (slice 4) |
| `REL-AUD-CLOCK-001` | migrated | soak post-checks ported exactly |
| `REL-DISP-REFRESH-001` | migrated | refresh transaction plus a recording |
| `REL-DISP-HDR-001` | migrated | HDR transaction plus a recording |
| `REL-DISP-MIXED-001` | migrated | preview-freeze verdict |
| `REL-DISP-DPI-001` | migrated | scaling facts and the window minimum |
| `REL-VIS-OVERLAY-001` | declared | operator-judged |
| `REL-VIS-NOTIFY-001` | declared | operator-judged |
| `REL-UPD-PORTABLE-001` | unavailable | candidate-bound installed-byte evidence is not wired yet |
| `REL-UPD-MSI-DECLINE-001` | declared | Secure Desktop |
| `REL-UPD-MSI-001` | declared | Secure Desktop |
| `REL-PKG-CHOCO-001` | declared | Tier 2 (slice 3) |
| `REL-JOURNEY-001` | migrated | delegates to the journey script |
| `REL-SHUTDOWN-001` | migrated | own instance, own run id |

`ExoSnap.Verify list` prints the same column. A gate whose body has not been written
reports `SKIPPED ("not migrated")` and never `PASS`, so an unmigrated gate can never
look like a gate that ran.

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
pwsh scripts/release-verify.ps1 qualify        # the promotion verdict, and only with -Publish the upload
pwsh scripts/release-verify.ps1 recover        # restore a dirty environment, and nothing else
```

A campaign that is going to qualify a release needs three things `prepare` cannot work out for
itself, because a published portable ZIP carries no build manifest:

```powershell
pwsh scripts/release-verify.ps1 prepare `
    -ExePath C:\rc\portable\exosnap.exe -Tag v0.9.1-rc1 `
    -SourceCommit <the commit v0.9.1-rc1 points at> `
    -PortableZip C:\rc\ExoSnap-0.9.1-rc1-windows-x64-portable.zip `
    -Msi C:\rc\ExoSnap-0.9.1-rc1-windows-x64.msi
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
| `PASS` / `FAIL` | measured; `FAIL` means the **product** is wrong, and nothing else may claim it |
| `INFRA_ERROR` | nothing measured the product: the scenario threw, an external tool was missing or unparseable when it was needed, a bounded wait expired, an environment mechanism reported success and then read back something else |
| `UNVERIFIED` | attempted, outcome unknown (interrupted, evidence unusable) |
| `STALE` | passed once, against an artifact or environment that has since changed |
| `SKIPPED` | deliberately not run, with a recorded reason |
| `DEFERRED` | a human gate nobody could answer — no interactive stdin, `-NonInteractive`, or the operator postponed it |
| `UNAVAILABLE` | this machine cannot offer what the scenario declared it needs |

`INFRA_ERROR` exists so that `FAIL` can mean one thing. A campaign with a single
`INFRA_ERROR` is not releasable either -- the publish gate refuses both equally -- but
it has not claimed a product defect nobody measured. The split is made once, in
`Resolve-ReleaseScenarioOutcome` (`scripts/lib/ReleaseQualification.ps1`), from the
environment transaction's own error codes: `apply_rejected`, `device_not_present`,
`unknown_property` and `not_mutable` are facts about the desk and stay `UNAVAILABLE`;
every other code is a mechanism that misbehaved.

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

## The human layer

Eleven scenarios can reach a person. Most of them no longer do.

The rc19 campaign found zero product defects and four runner defects, all in this
layer, and three of the four were consequences of one thing: gates that install,
update or reconfigure ran on the developer's real machine, against their real
configuration directory, their real installed product and the machine-wide
single-instance mutex. The accept gate removed the older build the decline gate
needed; a killed soak left a recovery manifest that both MSI gates refused to start
against; a declined update left the updater holding `exosnap-updater.exe`, so the next
launch could not stage its own. The fourth was the prompt itself, and it is described
under *The operator protocol* below.

So the layer is now built from four mechanisms, in this order of preference:

1. **A clean machine.** The install, update and Chocolatey gates run inside **Windows
   Sandbox**. Every run starts from nothing, the logon command already holds an
   administrative token so `msiexec` raises no prompt, and the accept gate cannot
   remove a starting point because the machine is discarded afterwards.
2. **A named third-party tool**, called through one seam, for machine state Windows
   exposes no documented setter for. The tool is never on the release path and the
   runner never learns *how*: it says what it wants and reads the result back through
   `envctl`, which is the actual evidence either way.
3. **Fault injection at the exact call site**, for one state that only a Secure
   Desktop click could otherwise produce.
4. **A person**, for the two things nothing else can read.

### What still asks a person, and why

| Scenario | Asked when |
|---|---|
| `REL-PRESENT-002` | the runner is not elevated; an elevated one launches the child itself |
| `REL-CAP-FSE-001` | `probe_fullscreen` is not built (`-DEXOSNAP_BUILD_PROBES=ON`) |
| `REL-CAP-STALL-001` | `probe_stall_window` is not built |
| `REL-AUD-SILENCE-001` | no virtual cable endpoint, or no SoundVolumeView to route to it |
| `REL-AUD-FORMAT-001` | no SoundVolumeView |
| `REL-AUD-DEGRADE-001` | not elevated, or no device instance id resolves |
| `REL-UPD-MSI-DECLINE-001` | no sandbox and an updater without the fault seam |
| `REL-UPD-MSI-001` | no sandbox and an unelevated runner |
| `REL-PKG-CHOCO-001` | no sandbox |
| `REL-VIS-OVERLAY-001` | **always** -- colour on a capture-excluded overlay |
| `REL-VIS-NOTIFY-001` | **always** -- the severity tint of a toast |

The last two are the sight checks and they are the point of the whole arrangement.
`WDA_EXCLUDEFROMCAPTURE` defeats screenshots, screen recording and `PrintWindow` by
design, and the visual harness only grabs the scene graph, which shows correct alpha
even when the window composes wrongly on screen. What UI Automation *can* say is that
the window is really there with really that text -- asserted separately, so
"`overlay.snapshot` says five overlays are up" and "five overlays are up" stopped being
the same sentence. What it cannot say is what colour anything is.

### The operator protocol

One line per step. Two action forms, and each says who acts next:

```
REL-AUD-FORMAT-001  In Sound settings, set the device bound to audio.render.44100-test
                    to 44100 Hz and make it the default playback device.
  [Enter] wenn erledigt   (? = details, s = skip, x = abort)
```

`[Enter] startet jetzt` means the runner acts on Enter and nothing has happened yet.
`[Enter] wenn erledigt` means the operator has already acted and the runner is about to
verify. The line they replaced was `[y] yes [n] no`, which meant the first for the MSI
and Chocolatey gates and the second for the present, audio and visual gates -- one
keystroke, two opposite meanings, decided by which screen of instructions had scrolled
past. There is no third action form, and a test refuses one.

Everything the gate used to print before the prompt -- why a person is needed, what to
expect, how this will be verified -- is behind `?`. It is the same text; what changed is
that it no longer separates the question from its meaning by a screenful. An
unrecognised answer is asked again and decides nothing: a typo used to end the gate as
"aborted".

A sight check is spelled `[j] richtig  [n] falsch`, deliberately not with Enter, so the
reflex that answers an action prompt cannot pass a surface nobody looked at. `n` asks
for a one-line reason *there*, while the screen still looks the way it did, and that
reason reaches the report.

The whole sequence is printed once before the first gate, in the order it will happen,
with the dependencies named:

```
== Sequence
   1. automated   REL-PRESENT-001  Unelevated present diagnostics ...
   2. you         REL-PRESENT-002  Elevated present diagnostics ...
   3. you         REL-CAP-FSE-001  True exclusive fullscreen ...  (after REL-PRESENT-002)
  ...
  2 of 26 step(s) may ask you something.
```

The dependencies are `DependsOn` in the catalog and are resolved by a stable
topological sort, so `list` and a run read the same way and a cycle is reported rather
than silently broken. Three of them each cost a campaign a FAIL:

- `REL-CAP-FSE-001` after `REL-PRESENT-002`, and sharing its **elevated session**.
  Present diagnostics need elevation and the single-instance guard is machine-wide, so
  the second instance FSE used to launch was swallowed and the gate reported "could not
  connect to the Live Verify endpoint" for a healthy product. The shared instance is
  released the moment the next scenario does not want it.
- `REL-UPD-MSI-001` after `REL-UPD-MSI-DECLINE-001`, and both closing the updater
  afterwards.
- `REL-PKG-CHOCO-001` after both, because its rehearsal reinstalls the release MSI.

### The gate contract

A gate prints its line, waits, **and then checks for itself**. A gate whose `Verify`
block returns false is a `FAIL` even when the operator pressed Enter -- an operator can
be mistaken about what they just did, and a gate that trusts the keystroke instead of
the machine is a checkbox with extra steps. A gate that declares no `Verify` block at
all is `UNVERIFIED`, not `PASS`.

The unanswerable case is checked **before** anything is printed, so nobody performs a
two-minute physical action that cannot be confirmed afterwards.

An automation can be the one that acts. `-Attest REL-XXX-000` says the caller has
already performed that gate's action: the question is skipped and the `Verify` block
runs exactly as it would have. It is not a way to pass a gate. What it cannot buy is a
sight check -- the question there *is* the verdict, and no caller can perform someone
else's looking, so an attested sight check is `DEFERRED`.

A `Verify` block may name its own terminal state by returning `Result` alongside `Ok`.
`UNVERIFIED`, `UNAVAILABLE` and `DEFERRED` then travel through the gate as themselves
instead of collapsing into `FAIL`, which is the same distinction rules 1 and 3 rest on.
`PASS` and `FAIL` keep travelling through `Ok`.

A `Verify` block never reaches for the runner's session state directly either. It asks
the context (`Get-ReleaseGateConnection -Context $context`), which re-establishes a
session that went away and reports one that cannot be re-established as a verdict
rather than an unhandled exception. Gates whose subject was the *previous* process
record the session id they prepared in `$Gate.State` and report `UNVERIFIED` when the
connection comes back from a different one. A gate handed an already-open connection
(the shared elevated session) reuses it: the endpoint serves one client at a time, so
reconnecting would be the runner waiting on itself.

Human gates sit **inside** the environment transaction. An operator who reports a
defect, stops, or walks away still leaves the machine restored.

## External tools

Every gate that stopped asking a person asks a tool instead, and the tools are
deliberately not ours: an oracle we wrote would answer with the same ETW session, the
same WASAPI call and the same window handle the product used, and agreeing with itself
is not evidence.

| Tool | Used for | Named by | Where to get it |
|---|---|---|---|
| Windows Sandbox | the install, update and Chocolatey gates | `EXOSNAP_SANDBOX_EXE` | `Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM` (elevated, reboot) |
| Intel PresentMon | the independent present-mode oracle | `EXOSNAP_PRESENTMON` | <https://github.com/GameTechDev/PresentMon/releases> |
| NirSoft SoundVolumeView | the default endpoint and its shared-mode format | `EXOSNAP_SOUNDVOLUMEVIEW` | <https://www.nirsoft.net/utils/sound_volume_view.html> |
| VB-CABLE | a render endpoint nothing is routed to | `EXOSNAP_SILENT_AUDIO_ENDPOINT` (name pattern) | <https://vb-audio.com/Cable/> |
| `pnputil` | disabling the audio device for the degradation gate | `EXOSNAP_PNPUTIL` | ships with Windows |
| UI Automation | reading the capture-excluded overlays and toasts | -- | `UIAutomationClient`, part of the Windows desktop runtime |

Three rules hold for all of them:

1. **A missing tool is never green.** It produces `precondition missing: ...` with the
   exact way to install it, and the scenario reports `UNAVAILABLE` -- an unmet
   requirement is not a failure, and it is not a pass either. A report can be searched
   for `precondition missing` to get the exact list of what somebody has to install
   before the next campaign.
2. **Nothing is discovered by guessing.** Each tool has one environment variable that
   names it and one documented default location. A tool found by neither is absent,
   however many similarly named binaries are on `PATH`.
3. **Every invocation goes through one seam** (`Invoke-ReleaseTool`), which is what
   lets the whole human layer be exercised without a machine action.

Two gates keep the older, caller-named tool arrangement, and it works the same way:
`REL-UPD-PORTABLE-001` and the MSI gates read `EXOSNAP_UPDATE_FROM` (an older official
`exosnap.exe`) or `EXOSNAP_UPDATE_FROM_MSI` (its installer, for the sandbox);
`REL-AUD-DEGRADE-001` accepts `EXOSNAP_ENDPOINT_VISIBILITY_TOOL`, called as
`<tool> set-visibility <endpointId> 0|1`. `REL-AUD-DEGRADE-001` also takes
`EXOSNAP_AUDIO_DEVICE_INSTANCE_ID`, which short-circuits the friendly-name match:
a machine with two identically named headsets cannot be resolved by name, and this
refuses to guess rather than disabling the wrong device.

### Why the audio properties are not envctl transactions

`device-format` and `default-roles` are `ENV_HUMAN` in the envctl catalogue, and that
is a decision rather than an omission: the only mechanisms Windows offers are the
Settings drop-down and the undocumented `IPolicyConfig`, and envctl refuses to write
either by policy. Using a private COM interface to make a test more convenient would
put an unsupported mechanism on the release path.

So the *mechanism* lives outside the release path in a named third-party tool, and the
*evidence* is unchanged: envctl reads the endpoint's shared-mode format and its default
render role back, and the gate refuses to continue until both actually read what the
scenario needs. Whatever the gate changed it puts back, from a `finally` block.

`REL-AUD-FORMAT-001` is the gate this fixed. It used to hand the operator a four-part
text block whose third part was "make it the DEFAULT playback device" -- a machine-state
precondition, asked of a person, and then recorded as a product `FAIL` when they set
the format and not the role.

## Windows Sandbox

`scripts/lib/ReleaseSandbox.ps1` stages a worker plus the files it needs into a
per-campaign directory, writes a `.wsb` that maps that directory read-write and
PowerShell 7 read-only, and waits for the worker's own marker file. The staging
directory is copied rather than mapped from the repository: a gate that could write
into the working tree is a gate that can change the thing it is verifying. PowerShell 7
is mapped rather than installed because Sandbox ships Windows PowerShell 5.1 only, and
the alternatives were a second 5.1 spelling of the control-channel client or
downloading an installer inside a machine whose whole value is a known starting state.

`WindowsSandbox.exe` returns as soon as the virtual machine is asked for, not when the
work inside finishes, so completion is read from the worker's marker. That is the only
honest signal available: a sandbox that crashed, was closed by hand or never started
leaves no marker, and the gate reports `UNVERIFIED` for it rather than reading a
partial result as a verdict. A step the worker never reached is `UNVERIFIED`; a step
that failed is a `FAIL`; only a complete set of passing steps is a `PASS`.

Two workers run in there:

- `sandbox-update-worker.ps1` installs an older release from `EXOSNAP_UPDATE_FROM_MSI`,
  selects the Preview channel, declines an update (through the fault seam), asserts
  `failureCase uacDeclined` with the installation intact, **closes the updater**, and
  then accepts an update and asserts the version changed. Both MSI gates read that one
  result document, because both describe one sequence.
- `sandbox-choco-worker.ps1` installs the release MSI, runs the product once so the
  user configuration directory exists to be judged against, bootstraps Chocolatey and
  then calls the existing `choco-rehearsal-worker.ps1` unchanged.

`REL-PKG-CHOCO-001` on a real machine is unchanged and still available: it is the one
gate that installs software, it raises exactly one prompt, and the reinstall runs from
a `finally` block so a rehearsal that threw halfway does not leave the machine without
ExoSnap. Three properties of it are worth knowing before running it that way:

- **`vcredist140` is not restored.** It is a declared Chocolatey dependency of the
  package, so the install can install or upgrade the Visual C++ redistributable; the
  version is recorded before and after and the verdict names the change. Downgrading a
  machine's C++ runtime to undo it would be worse than the change.
- **The empty parent directory and registry key are recorded, not asserted.** The MSI
  declares no owner for the `Codexo` parent directory or its parent registry key; the
  `ExoSnap` directory, the ARP entry, the shortcut and the Chocolatey lib directory are
  strict.
- **The tracked package is never modified.** The rehearsal packs a copy whose
  `url64bit`/`checksum64` point at the local MSI, because the tracked checksum
  describes a file that does not exist until the release is published.

The MSI is not bound by `prepare` (the campaign binds the portable `exosnap.exe` only):
the gate looks for a sibling `ExoSnap-<version>-windows-x64.msi` beside the artifact,
requires its Property table to declare ProductName `ExoSnap` by Manufacturer `Codexo`,
compares it against a `.msi.sha256` sidecar when one is there, and reports `UNAVAILABLE`
when it finds none or several. `EXOSNAP_RELEASE_MSI` names one explicitly.

## The declined elevation prompt

`EXOSNAP_UPDATER_FAULT=uacDeclined` makes the updater's elevation call behave as if the
prompt had been declined: the same `ERROR_CANCELLED`, at the same call site, producing
the same `FailureCase::UacDeclined` and the same C1 re-handoff. The product assertion
is unchanged -- `failureCase uacDeclined` with the installation intact, and
`strandedInBackup` is a `FAIL`.

The seam can only turn a step into a failure the product already models. It cannot skip
a verification, relax a signature check, or make an install succeed; a fault that could
make something succeed would be a security defect regardless of how it is gated. It is
armed for exactly one child process and read at the call site rather than cached, so the
accept gate that follows cannot inherit a decline it never asked for.

Either way the updater is **closed** afterwards. A declined or failed update leaves it
running with its result on screen, which is correct product behaviour and wrong for the
next gate: the running process holds `exosnap-updater.exe`, so the next update cannot
stage its own over it. That is the whole of the rc19 "Failed to stage updater file"
finding, which was reported as an MSI failure.

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

Every bounded poll takes its deadline from `Get-ReleaseGateDeadline`, in one place, so
the dry run can shorten it. That changes only how long a loop is willing to wait for a
machine that is not there; the loop, and what it polls for, are the shipped ones.

## Driving the artifact: the control channel, never `--auto-record`

Anything that has to record against the release under test goes through the Live
Verify control channel (`--live-verify-control <run id>`, then `settings.set`,
`record.selectTarget`, `record.start`, `record.stop`, `record.result`). This includes
the checks a person runs by hand, such as the HDR media matrix.

`--auto-record` is not an option here, and the failure is silent. The auto-record
harness sits behind `EXOSNAP_HARNESS_GATE` (`app/quick/ExoSnap/Quick/CMakeLists.txt`):
non-Release builds get it implicitly, a Release build only with
`-DEXOSNAP_BUILD_BENCHMARK_HARNESS=ON`, which the release workflow does not pass. A
shipping binary therefore parses the flag, finds no harness behind it, and starts as
the ordinary interactive application — a window, CPU, no recording and no exit. It
also writes to the user's real configuration directory, because the scratch-config
isolation lives inside the same `#if`.

The flag string alone is no evidence either way: `cli/CommandLineFlags.cpp` lists it
in every build so an unknown-option message stays accurate. To tell the two apart,
look for a string that only exists INSIDE the guard, such as the auto-record result
JSON's `session_report_path`.

## Evidence

Per scenario: the artifact identity, the environment before, what was requested and
applied, the product state, the assertions, any human actions, the environment after
restore, timestamps, both verdicts. Nothing that identifies the person at the machine —
a run directory is evidence other people read.

`report` writes `report.md`, `report.json` and `junit.xml` from one state, so the three
cannot disagree, and `release-verification.json` -- the qualification record -- from
the same state again.

## The qualification record

`release-verification.json` is what a release is promoted on. It is the only thing the
publish gate reads, so it has to carry everything a publisher would otherwise have to
take on trust from the person who ran the campaign.

| Field | What it answers |
|---|---|
| `schema` | which shape this is (`exosnap.release-verification/1`); an unknown one is refused, not guessed at |
| `generatedUtc` | when the verdict was taken |
| `runId` | which campaign directory produced it |
| `rcTag`, `sourceCommit` | which candidate, and which commit, this verdict is about |
| `artifact` | the exe identity the campaign was bound to: SHA-256, size, product/file version, Qt runtime, install tree |
| `packages` | the RC's published downloadables by file name and SHA-256 -- what ties the verdict to the bytes on the release page |
| `machineFingerprint`, `environment`, `capabilities` | which machine, in which state, and what it could offer |
| `harness` | version, the commit the harness was checked out at, and whether that tree was modified |
| `catalog` | the scenario catalog's version, a digest over every id and its opt-in flag, and the count |
| `required` | the policy, the required ids, and the opt-in ids named for this release |
| `checks` | every verdict: state, `required`, `optIn`, attempts, timestamps, message, restore verdict, per-property environment evidence, and a SHA-256 for every evidence file cited |
| `summary`, `restoreSummary` | the product and environment-restore verdicts, counted |
| `qualification` | `QUALIFIED` or `NOT_QUALIFIED`, with every blocking reason |

`QUALIFIED` requires all of: no `FAIL`, no `INFRA_ERROR`, no required gate in any state
other than a measured one, no environment-restore verdict outside
`NOT_APPLICABLE`/`RESTORED`, no cited evidence file missing, at least one package hash,
and a complete identity (commit, RC tag, machine, harness commit, catalog version and
digest). **Required** is every scenario that is not opt-in, plus the opt-in scenarios
named with `-Required` for that release; naming an id the catalog does not have is an
error rather than an empty set.

`scripts/check-release-qualification.ps1` applies exactly these rules on a clean
checkout, plus the three questions only the publisher can ask -- is this record about
the commit being tagged, about the RC whose assets were verified, and do its package
hashes match the `.sha256` sidecars that RC actually published. The record's own
`QUALIFIED` claim is re-derived rather than believed, so a hand-edited verdict does not
survive the gate. `.github/workflows/release-candidate.yml`'s `require-qualification`
job runs it on every final tag, before anything is created or uploaded.

## Tests

`scripts/tests/release-verify.tests.ps1`, registered as CTest
`live_verify.release_runner`, and `scripts/tests/release-qualification.tests.ps1`
(`live_verify.release_qualification`) for the record and the publish lock -- where the
cases that matter are the refusals: a missing record, a record about a different commit,
a record about different bytes, a `FAIL`, an `INFRA_ERROR`, a required gate nobody
answered, a forged `QUALIFIED`.

Everything runs against a **fake** `envctl` that reports whatever the test needs,
including lying about success. That is the point: the failures worth pinning — a
setter that claims it worked while the read-back disagrees, an original device that
vanished mid-restore, a journal left dirty by a killed runner — cannot be produced on
demand by a real display or a real audio endpoint. Nothing in the suite touches the
machine's configuration.

### The dry run

A gate that has never run does not get shown to a person. So every gate that can ask
one is exercised in the suite **RED once and GREEN once**, against a simulated operator
and simulated tools: the shipped scenario bodies out of `Get-ReleaseScenarioCatalog`,
the shipped human-gate contract out of `release-verify.ps1`, and a scripted control
channel in place of the product. A harness that tested its own transcription of a gate
would prove nothing about the gate.

Nothing in it launches a process, opens a pipe, starts a virtual machine, reads a real
audio endpoint or writes a registry value. Four seams make that possible, and they
exist in the runner for this reason:

| Seam | Replaces |
|---|---|
| `Set-ReleaseOperatorReader` | the console |
| `Set-ReleaseToolInvoker` | PresentMon, SoundVolumeView, pnputil, `WindowsSandbox.exe`, UI Automation |
| `Set-ReleaseToolAvailability` | which of those exist on this machine |
| `Get-ReleaseGateDeadline` | how long a bounded poll waits for a machine that is not there |

The sandbox path is exercised for real up to the virtual machine itself: the staging
copy, the `.wsb`, the mapped-folder paths and the marker-plus-result transport all run,
and only the launcher is simulated, by one that writes what the worker would have
written.
