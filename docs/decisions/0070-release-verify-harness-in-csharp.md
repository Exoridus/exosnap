# ADR 0070: The release-verify harness is a typed C# program

- Status: Accepted
- Date: 2026-09-08
- Supersedes: nothing
- Related: ADR 0066 (Live Verify control channel), ADR 0067 (shared control layer), ADR 0068 (validated update handoff), ADR 0069 (transactional environment orchestration)

## Context

Release qualification orchestrates process lifetimes, named pipes, elevation, Windows
UI, audio endpoints, installers, package managers, devices and external tools, and
then decides whether a build may ship. That work had grown into a large dynamic
PowerShell orchestrator: hashtables whose shape is a convention, scenario bodies as
script blocks, `$script:` seams between them, and tool invocations whose output was a
string that had to be re-parsed at every call site.

Two consequences, both observed rather than predicted.

Nothing distinguished "the product is wrong" from "the harness could not look".
A broken escape, an unparseable tool output, a lost pipe or a runner timeout produced
the same red as a genuine defect, so a red campaign could not be read without
re-running it by hand.

The catalog declared what a scenario needed as free text a person had to honour.
Scenarios therefore ran on machines that could not satisfy them, and the resulting
mess was reported as a product defect nobody had measured.

Separately, the publishing pipeline has no machine-readable proof that a release
candidate was ever verified. A person pushes `vX.Y.Z`, the workflow treats the tag as
the approval, and packaging being green is the whole gate. `v0.9.0` was published that
way before its own checklist had been completed.

## Decision

**Release verification is a strictly typed .NET program, and PowerShell keeps only
the bootstrap.**

```
tools/release-verify/
    ExoSnap.Verify/           CLI, scenario engine, models
    ExoSnap.Verify.Windows/   Win32, COM, WASAPI, DXGI adapters; the elevated worker boundary
    ExoSnap.Verify.Tests/     unit, contract and hostile-input tests
    ExoSnap.Verify.Fixtures/  a child process that misbehaves on request, for the tests
```

Self-contained `win-x64` publish, so the release machine needs no matching runtime.
The published binary is never committed.

Five properties are the decision.

**A failure verdict means one thing.** `Fail` is reserved for "ExoSnap is wrong".
Everything on the way to an observation - an unparseable tool output, a tool that
never started, a lost named pipe, a harness timeout, an inconsistent fixture - is
`InfrastructureError`. A campaign carrying either is not releasable, but only one of
them claims a defect. `Blocked`, `Unavailable`, `Deferred`, `Skipped` and `Stale`
complete the taxonomy, and an exception escaping a scenario body becomes
`InfrastructureError` in the engine rather than at each body's discretion.

**Hardware is a capability system, not optional code.** Every scenario declares its
requirements as capability keys. The harness measures the machine once, read-only,
and writes `machine-capabilities.json`. A requirement the machine does not satisfy
yields `Unavailable: capability <key>=<value> not satisfied`, never a failure and
never an attempt to change the machine into shape. A capability the probes could not
determine is reported as `unknown`, and `unknown` never satisfies anything - a
requirement that cannot be checked has not been met.

**Devices are named by alias, never by product name.** `device.display.main-hdr`, not
a monitor model. The alias profile is the only machine-specific file; a friendly name
is not stable even at one desk, because two identical devices share one.

**Child processes are contained.** Every tool runs through one runner:
`ProcessStartInfo` with `ArgumentList`, no shell, both streams drained concurrently,
an explicit deadline, and a Windows job object with kill-on-close so an aborted gate
leaves no updater or probe behind to poison the next scenario.

**A standard verifier never inspects elevated UI.** User Interface Privilege
Isolation forbids it, so the arrangement is inverted: the elevated half runs as its
own process and communicates only through a result document. The Secure Desktop is
never automated at all.

## Consequences

The harness has to be tested itself, and its own tests are the reason to believe a
verdict. The hostile inputs are explicit cases rather than hopes: a path containing
spaces, an ampersand or non-ASCII characters, a tool writing to standard error, a tool
that hangs, a tool that exits without output, malformed JSON, a child that spawns a
child, and a tool reporting a decimal comma. That last one is refused as a contract
violation rather than silently misread, which is the whole difference between a gate
and a guess.

Migration is incremental and the catalog is honest about it. Every scenario in
`docs/dev/release-verify.md` exists as a descriptor today, and every body reports
`Skipped ("not migrated")`. A gate that has not been written must never look like a
gate that ran, so no unmigrated scenario can report `Pass`, and `qualify` refuses to
produce a record until the gates behind it exist.

The PowerShell catalog remains the running system until each gate is ported. Two
catalogs exist during the migration; the C# one is a declaration, not a second
implementation, and it is deleted from the PowerShell side as each gate moves.

Promotion becomes a technical lock rather than a habit. A run produces
`release-verification.json` carrying the RC tag, the source commit, artifact digests,
the machine fingerprint, the harness version, the catalog version, the capabilities,
every verdict, the environment restore verdicts, the evidence hashes and an overall
qualification. Publishing refuses a missing record, a record carrying anything but
passes on the required scenarios, and a record whose commit does not match the tag.

The cost is a second toolchain in the repository: a pinned .NET SDK, central package
versions and lock files. That is accepted deliberately. The alternative was to keep
deciding releases with a program whose types existed only in the author's head.

Not adopted: Robot Framework, Appium and WinAppDriver as the base. Marker, tag and
skip semantics are borrowed from pytest and Robot, isolation from Windows Sandbox,
desktop automation from FlaUI and UI Automation, and independent oracles from
PresentMon - but the harness itself stays a small, strictly typed .NET program.
