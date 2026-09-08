# The release-verify harness (ExoSnap.Verify)

The typed harness that will decide whether a release candidate may be promoted.
ADR 0070 records why it is a .NET program rather than a PowerShell orchestrator;
this document is how to work on it.

`docs/dev/release-verify.md` describes the PowerShell campaign that is still the
running system. Until a gate is migrated, that document is the one to follow.

## Layout

```
tools/release-verify/
    global.json                  SDK pin, and the Microsoft.Testing.Platform opt-in
    Directory.Build.props        net10.0-windows, nullable, warnings as errors, analyzers
    Directory.Packages.props     central package versions
    ExoSnap.Verify.slnx          the solution every command below takes

    ExoSnap.Verify/              CLI, engine, models
        Models/                  outcomes, results, evidence, descriptors, qualification record
        Json/                    source-generated contracts for every document written
        Capabilities/            machine probes, capability set, tool resolution
        Processes/               the process runner and the tool-output contract
        Adapters/                ffprobe, envctl, the Live Verify session, PresentMon
        Gates/                   the migrated scenario bodies and the logic they share
        Engine/                  catalog, plan, run directory, run state, campaign, qualification
        LiveVerify/              the control-channel client
        Catalog/                 the release scenario catalog
        Cli/                     argument parsing

    ExoSnap.Verify.Windows/      Win32, COM, WASAPI, DXGI, job objects, elevated worker boundary
    ExoSnap.Verify.Tests/        unit, contract and hostile-input tests
    ExoSnap.Verify.Fixtures/     a child process that misbehaves on request
```

`bin/` and `obj/` are ignored. No build output is ever committed.

## Running it

Every command runs from `tools/release-verify`, because `global.json` selects both
the SDK and the test runner.

```
dotnet restore ExoSnap.Verify.slnx --locked-mode
dotnet build   ExoSnap.Verify.slnx --no-restore -warnaserror
dotnet test    ExoSnap.Verify.slnx --no-restore
```

`pwsh scripts/verify.ps1 -Full` runs exactly that as its `verify-harness` stage, and
CI runs it as the `verify-harness` job. A locked restore is deliberate: a package
that moved underneath the harness must fail here rather than upgrade quietly on the
machine that decides whether a release ships.

For the release machine, publish self-contained so no matching runtime is needed:

```
dotnet publish ExoSnap.Verify/ExoSnap.Verify.csproj -p:PublishProfile=win-x64
```

## Commands

```
ExoSnap.Verify capabilities [--out <path>]
```

Measures this machine, read-only, and writes `machine-capabilities.json`. Each key
carries the mechanism that produced it, so a value can be audited rather than
believed. A key the probes could not answer is written as `unknown`.

```
ExoSnap.Verify list [--json]
```

The scenario catalog: id, class, isolation, interaction, whether it is opt-in, its
requirements and its title. `--json` emits the same catalog as the document a run
records beside its verdicts.

```
ExoSnap.Verify prepare --exe <path> [--rc <tag>] [--commit <sha>] [--package <path>]...
```

Binds a campaign to explicit bytes. There is deliberately no default artifact: a
release verdict says "these bytes behaved correctly", so the executable is named and
hashed, each published package is hashed, and the machine is measured once before
anything runs. The run directory then holds `campaign.json`,
`machine-capabilities.json`, `scenario-catalog.json` and `state.json`.

```
ExoSnap.Verify run [--id <id>] [--class <c>] [--include-opt-in]
```

Runs the selected scenarios against the prepared campaign and records one verdict per
scenario. Exits non-zero when anything failed or could not be carried out.

Both `run` and `qualify` revalidate the executable hash, campaign/state identity and
prepared catalog before using recorded results. Changed bytes or a changed catalog
require a new campaign; results cannot be attributed to the old binding.

```
ExoSnap.Verify report
```

Prints the verdicts recorded so far, with a count per state.

```
ExoSnap.Verify qualify [--required <id>]...
ExoSnap.Verify qualify --dry-run [--rc <tag>] [--include-opt-in] [--class <c>] [--id <id>]
```

`qualify` writes `release-verification.json` and prints either
`QUALIFIED FOR PROMOTION` with the commit and RC it qualifies, or `NOT QUALIFIED`
followed by every reason. `--required` names an opt-in gate this release must also have
answered; an unknown id is an error rather than an empty set, because a typo that
quietly required nothing is the exact failure the lock exists to prevent.

Every required ID must have exactly one passing verdict, and evidence files must
still match their recorded SHA-256 digests. A requalification removes the previous
export first, so a failed attempt cannot leave an old successful record to promote.

`--dry-run` evaluates the catalog against this machine and prints what each scenario
would do, without running anything and without touching the machine beyond the
capability probes. It exits non-zero and prints `NOT QUALIFIED`: a dry run produces no
qualification record, and nothing should be able to mistake its output for one.

Nothing here tags, publishes or promotes. The most a run produces is a record saying
promotion is permitted; who acts on that is a decision outside this program.

The record is written in the shape `scripts/check-release-qualification.ps1` reads, so
either producer can be checked by the one lock, and
`scripts/tests/verify-harness-record.tests.ps1` feeds a record this harness actually
wrote through that lock. Two fields the PowerShell producer carries are deliberately
absent here rather than invented: `startedUtc` and `finishedUtc`, because the engine
measures how long a body ran (`durationMs`) and never recorded wall-clock boundaries.
Nothing reads them, and a timestamp nobody measured would be worse than a missing one.

## The outcome taxonomy

| Outcome | Means |
|---|---|
| `Pass` | The product behaved as the scenario requires. |
| `Fail` | The product did not. Reserved for exactly that. |
| `InfrastructureError` | The scenario could not be carried out; nothing was learned about the product. |
| `Blocked` | A harness precondition was not met, so the scenario never started. |
| `Unavailable` | This machine does not satisfy a capability the scenario requires. |
| `Deferred` | Postponed by decision rather than by machine state. |
| `Skipped` | Not selected for this run. |
| `Stale` | A recorded verdict that no longer describes the artifacts or catalog in front of it. |

An exception escaping a scenario body becomes `InfrastructureError` in the engine,
never `Fail`. A scenario body therefore does not need to catch its own infrastructure
failures, and there is one place fewer for it to get that wrong.

Missing or malformed recording counters and coarse delegated-script failures are
infrastructure errors, not evidence of a product defect.

Every required gate must report `Pass`. An optional gate that was not selected is
harmless, but any recorded `Fail` or `InfrastructureError` disqualifies regardless of
whether the gate was required. A product defect cannot become releasable by making its
gate opt-in, and an infrastructure error means the run did not measure what it claims.

## The capability model

A scenario declares requirements as capability keys, and the machine is measured once
before anything runs.

- `os.windows`, `os.version`
- `elevated`, `interactiveDesktop`, `sandbox.available`
- `presentmon.available`, `soundvolumeview.available`, `ffprobe.available`
  (`EXOSNAP_PRESENTMON`, `EXOSNAP_SOUNDVOLUMEVIEW`, `EXOSNAP_FFPROBE` win over PATH,
  so a run can pin the exact build of a tool whose output it parses)
- `gpu.vendor`, `gpu.d3d11`
- `display.hdr`, `display.refresh.<hz>`
- `audio.endpoint.<sampleRate>`
- `device.<alias>`, which is `bound` or `unbound`

Two rules the whole model rests on.

A capability that could not be determined is `unknown`, and `unknown` satisfies
nothing. A requirement that cannot be checked has not been met, and the scenario is
reported `Unavailable` rather than run against a guess.

`display.hdr` is what an output is presenting **right now**, not what a panel could do
if somebody switched it. A scenario that needs HDR needs the desktop to be in HDR.

An unsatisfied requirement is reported verbatim as
`capability <key>=<value> not satisfied`.

## Tiers

| Tier | Isolation | What runs there |
|---|---|---|
| 0 | `Hermetic` | No device, no real UI, no registry. Algorithms, state machines, parsers, protocols, scenario logic. Runs in CI. |
| 1 | `Desktop` | The real binary on a real desktop, no special hardware. Process lifetime, window lifecycle, control channel, file creation, ffprobe. |
| 2 | `DisposableOs` | Windows Sandbox or a throwaway VM. Installs, upgrades, package managers, registry footprint, rollback. |
| 3 | `HardwareLab` | Declared hardware. Encoders, HDR, refresh rates, present modes, audio endpoints, soaks. |

Tier 3 is selected from the capability document, never from the accident of an
RTX or an HDR panel being present.

## The adapters

Each external mechanism is an interface the engine consumes, so a gate's logic can be
exercised without the mechanism behind it.

| Interface | Drives | Notes |
|---|---|---|
| `IFfprobe` | `ffprobe` | typed streams and container facts, plus the first-to-last packet span per stream |
| `IEnvctl` | `exosnap-envctl` | one JSON document per subcommand, with the exit code kept because several subcommands answer a verdict with it |
| `ILiveVerifySession` | `exosnap.exe` | launched with its own run id under a throwaway config directory inside a job object |
| `IPresentMon` | a PresentMon capture | a header-driven CSV reader; nothing here starts PresentMon |

Three properties are worth stating out loud.

**The packet span is measured from packets, not from a duration tag.** A live-muxed
MKV carries no per-stream `DURATION`, so a reader that took the tag would see every
track as full length whatever it actually contains.

**Every envctl mutation is a transaction, and the restore is in a `finally`.** It has
to survive an assertion failure, a product failure, a harness bug, a timeout and
cancellation, because a human gate sits inside a transaction and an operator who walks
away must not leave the machine reconfigured. The product verdict and the restore
verdict stay separate: a scenario can prove the product correct and still leave a
display in the wrong mode, and one field cannot say both.

**A session never reaches the visible desktop by default.** The launcher sets the
offscreen Qt platform unless the caller overrides it, so a campaign cannot take focus
from whoever is using the machine.

PresentMon columns are matched by header rather than by position: PresentMon 2.x
changes which optional metrics it emits with its command line, so a positional reader
is correct for exactly one invocation and silently wrong for every other. Only
`ProcessID` and `PresentMode` are required; everything else is read when present and
reported as null when it is not.

## Testing the harness

The harness decides whether a release ships, so it is itself tested at three levels:
scenario logic against a fake, adapter contracts against captured real fixtures, and
platform smoke against the real mechanism. The ffprobe and envctl fixtures were
captured from those tools with machine-specific paths sanitized; the PresentMon CSV is
synthetic and identified as such in its contract test because starting its ETW session
requires the disposable VM.

The hostile inputs are explicit cases in `ProcessRunnerTests`, because each is a way a
gate has been made to report the wrong thing by a tool, a path or a locale rather than
by the product: a path with spaces, an ampersand or non-ASCII characters; a tool that
writes to standard error; a tool that floods a stream; a tool that hangs; a tool that
exits without output; malformed JSON; a child that spawns a child; and a tool
reporting a decimal comma. The last is refused as a contract violation rather than
misread, which is the difference between a gate and a guess.

`ExoSnap.Verify.Fixtures` is the child process those tests drive. A shell would not
do: its own quoting rules would be what the tests measured. The tests copy the whole
fixture directory to a hostile path, because a framework-dependent executable needs
its assembly beside it.

Exactly one test starts ExoSnap: the Live Verify smoke, which launches the Debug build
with the offscreen Qt platform under a throwaway configuration directory inside a job
object, completes the handshake, reads `app.identity`, and asserts the session tears
down without leaving the process or that directory behind. It skips when the Debug
build or the pinned Qt is not installed -- a missing build is a statement about the
tree, not about the product, and a suite that went red for it would be red on every
fresh clone.

Offscreen is also why `ShutdownAsync` has three answers rather than two. A windowless
process owns no window a close request can reach, so it reports `NotRequestable`
instead of waiting out a deadline nobody was asked to meet. `REL-SHUTDOWN-001` declares
`Desktop` isolation for exactly that reason, and treats `NotRequestable` as an
infrastructure error: a gate that ran against a windowless instance measured the
harness, not the product.

Nothing else in the suite starts an installer, a sandbox, PresentMon against a real
target, or any GUI, and nothing mutates machine state -- the envctl smoke is
`snapshot`, which only reads.

## What is not migrated yet

The gate-by-gate table lives in `docs/dev/release-verify.md`, next to the campaign it
describes, and `ExoSnap.Verify list` prints the same column. A scenario whose body has
not been written carries `NotMigratedBody` and reports `Skipped ("not migrated")`. A
gate that has not been written must never look like a gate that ran, so nothing here
can report `Pass` until its body exists.

`scripts/lib/ReleaseScenarios.ps1` remains the running system meanwhile, and
`scripts/release-verify.ps1 -Engine DotNet` is how a campaign opts into this one. The
remaining migration order is UI Automation and the elevated worker, then Windows
Sandbox and MSI, then audio and device state.

Also not built yet, and deliberately so:

- The elevated worker executable. `ElevatedWorker` defines the boundary - a separate
  elevated process, a result document, no cross-integrity UI inspection - and refuses
  to run, because an entry point that returned success without doing anything would be
  indistinguishable from one that had.
- UI Automation. FlaUI arrives with the first scenario that needs it.
- Candidate-bound portable updates. `REL-UPD-PORTABLE-001` reports `Unavailable`
  until the handoff can prove that the installed bytes match the prepared candidate;
  a successful update to an arbitrary version offered by the live feed is insufficient.
- Starting PresentMon. That needs an elevated ETW session on a machine presenting
  something worth measuring, so the capture is produced in the disposable guest and
  this side only reads it. `REL-PRESENT-XCHECK-001` reports `Unavailable` with that
  reason rather than pretending otherwise, and its CSV fixture is synthetic, written
  against the documented PresentMon 2.5.1 column contract.
