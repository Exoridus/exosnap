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
        Engine/                  catalog, plan, run directory, run state, qualification
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
ExoSnap.Verify qualify --dry-run [--rc <tag>] [--include-opt-in] [--class <c>] [--id <id>]
```

Evaluates the catalog against this machine and prints what each scenario would do,
without running anything and without touching the machine beyond the capability
probes. It exits non-zero and prints `NOT QUALIFIED`: a dry run produces no
qualification record, and nothing should be able to mistake its output for one.

A `qualify` without `--dry-run` is refused in this revision, because no gate has been
migrated and a record backed by nothing is worse than no record.

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

Only `Pass` permits qualification. An infrastructure error anywhere disqualifies,
required or not: it means the run itself did not work, so the scenarios around it
were measured on a machine in an unknown condition.

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

## Testing the harness

The harness decides whether a release ships, so it is itself tested at three levels:
scenario logic against a fake, adapter contracts against captured real fixtures, and
platform smoke against the real mechanism.

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

Nothing in the suite starts ExoSnap, an installer, a sandbox, PresentMon against a
real target, or any GUI.

## What is not migrated yet

Every scenario in the catalog carries `NotMigratedBody` and reports
`Skipped ("not migrated")`. The declarations are real; the gates are not. A gate that
has not been written must never look like a gate that ran, so nothing here can report
`Pass` until its body exists.

`docs/dev/release-verify.md` and `scripts/lib/ReleaseScenarios.ps1` remain the running
system meanwhile. The migration order is process and ffprobe first, then UI
Automation, then Sandbox and MSI, then audio and device state.

Also not built yet, and deliberately so:

- The elevated worker executable. `ElevatedWorker` defines the boundary - a separate
  elevated process, a result document, no cross-integrity UI inspection - and refuses
  to run, because an entry point that returned success without doing anything would be
  indistinguishable from one that had.
- Environment mutation. `exosnap-envctl` (ADR 0069) stays the only thing that changes
  machine state; the harness declares what a scenario mutates and will record whether
  it came back.
- UI Automation. FlaUI arrives with the first scenario that needs it.
- Writing a real qualification record. The model exists and is tested; `qualify`
  produces one only once there are gates behind it.
