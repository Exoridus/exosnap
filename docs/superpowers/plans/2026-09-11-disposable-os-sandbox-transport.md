# Disposable-OS Transport (Sandbox) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate the three Tier-2 disposable-OS gates (`REL-UPD-MSI-DECLINE-001`,
`REL-UPD-MSI-001`, `REL-PKG-CHOCO-001`) from the parallel PowerShell scenario system
to the typed C# harness, running them against Windows Sandbox through a new
`IDisposableOsTransport` abstraction that a second transport (the Hyper-V VM) can
be added behind later without touching the gate bodies.

**Architecture:** A transport-neutral model (`DisposableOsRunResult`, ported
verbatim from `Get-ReleaseSandboxStepVerdict`'s step-list verdict rule) is read by
gate bodies through `IDisposableOsTransport.RunWorkerAsync`. `SandboxTransport` is
the only implementation this plan ships: it stages a worker script and its payload
files into a fresh directory, writes the `.wsb`, launches `WindowsSandbox.exe`
through `ProcessRunner`, polls for the worker's marker file, and parses its
`result.json`. `DisposableOsRunner` tries a list of transports in order and reports
`Unavailable` when none are usable — the same shape `IElevatedWorkerHost` already
uses for "no path to the work, not a product verdict." The three gate bodies
(`UpdateDeclineGate`, `UpdateAcceptGate`, `ChocolateyRehearsalGate`) each build a
`DisposableOsWorkerRequest` naming the existing guest worker script and its payload,
call the runner, and turn the resulting step verdict into a `ScenarioResult`. The
guest-side PowerShell worker scripts (`scripts/lib/sandbox-update-worker.ps1`,
`scripts/lib/sandbox-choco-worker.ps1`, `scripts/lib/choco-rehearsal-worker.ps1`)
are **not rewritten** — they already run inside the disposable machine, are
transport-agnostic in what they need (a staging directory, a result path, a marker
path), and stay exactly as they are. Only the *host-side* orchestration — deciding
to launch, waiting, reading the result back, mapping it to a verdict — moves from
PowerShell to C#.

**Tech Stack:** C# / .NET (net10.0-windows), xUnit v3, the existing
`tools/release-verify/ExoSnap.Verify` and `ExoSnap.Verify.Tests` projects. No new
package dependencies.

**Spec:** `.workspace/design-verify-harness-2026-09-08.md` ("Execution plan"
section, Round 2 / Slice 3) for the vertical-slice intent; this plan narrows that
paragraph to what is buildable without a second, larger decision (see Non-Goals).

## Global Constraints

- One PR, the full gate only through the pre-push hook (`AGENTS.md` iteration
  rules); build the affected target and run the focused tests while implementing,
  full validation once at the end.
- `ScenarioResult.Fail` means "ExoSnap is wrong." Every failure to stage, launch,
  or read back a worker result is `ScenarioResult.InfrastructureError` or
  `ScenarioResult.Unavailable`, never `Fail`.
- No shell, no re-parsed command lines: every process launch goes through
  `ProcessRunner` with an argument list (`ExoSnap.Verify/Processes/ProcessRunRequest.cs`).
  `SandboxTransport` calls it the same way `Ffprobe`/`PresentMonReader` do.
- A gate whose body has not been written must report `Skipped`
  (`NotMigratedBody`), never `Pass`; a gate whose transport is unavailable must
  report `Unavailable`, never `Pass` or `Skipped`.
- Developer-facing source documentation is English, ASCII punctuation, no
  development provenance (no task/PR/branch references) in comments.
- C# nullable reference types are enabled project-wide; every new public API
  states its nullability explicitly, matching the surrounding code style (XML doc
  comments on every public member, `sealed` on classes with no designed
  subtype).

## Non-Goals (explicitly out of scope for this plan)

- **Hyper-V VM transport.** `Invoke-ReleaseVmRun.ps1` needs host-level elevation
  (Hyper-V Administrators, an elevated token — see `Test-ReleaseVmPrerequisite`),
  which the C# harness process does not hold. Crossing that boundary means
  generalizing `ElevatedWorkerResult` (currently a fixed present-diagnostics
  schema: `PresentCount`, `PresentMode`, `TearingAllowed`, `OracleNote`) into an
  envelope carrying a typed payload, and adding a second task type to
  `ExoSnap.Verify.Worker`. That is a real, separate design decision (confirmed
  with the developer during planning) and its own plan. `DisposableOsRunner` is
  built to accept a second transport without changing the gate bodies, so that
  plan only adds a `VmTransport` and re-orders the list (VM first, Sandbox
  fallback) — it does not touch this plan's files again.
- **Clean-first-start and old-to-new-upgrade gates.** The execution-plan paragraph
  names these, but neither has a catalog id, a descriptor, or prior PowerShell
  code today (confirmed: no `clean`/`upgrade`/`first.start` hits anywhere in
  `docs/dev/release-verify-catalog.md` or `scripts/lib/ReleaseScenarios.ps1`).
  `REL-UPD-MSI-001` already covers old-to-new (its title is "The MSI update
  elevates, installs and relaunches" and it `DependsOn` the decline gate for
  exactly the older-build starting point an upgrade needs). Clean-first-start has
  no existing analog and needs its own short design pass (what "clean" asserts:
  first-run onboarding? no prior config? — undefined today). Left for a follow-up
  plan with fresh `REL-*` ids, per the developer's own steer that the ids named in
  the 2026-09-08 execution-plan paragraph may not be the right ones.
- **Known limitation this plan accepts, documented rather than silently
  regressed:** today, a machine without Windows Sandbox still runs these three
  gates through a human-driven happy path on the real machine (a real UAC prompt,
  `Test-RunnerElevated` skipping it when the harness itself is elevated). After
  this plan, the C# body reports `ScenarioResult.Unavailable` when Sandbox is not
  present, with no PowerShell fallback. The three descriptors keep
  `Interaction: OperatorAssisted` / `Layer: Secure` unchanged (still true in the
  worst case — a person could still do this by hand, per
  `docs/release-checklist.md` sections 5, 7a, 8, which stay the manual runbook)
  rather than being flipped to `Automated`/`FullAuto`: the human-in-the-loop path
  is documented, not automated, so the descriptor must not claim otherwise. This
  is the "one or two documented minor limitations" `AGENTS.md` accepts at merge;
  call it out in the PR description.

## File Structure

New files:

- `tools/release-verify/ExoSnap.Verify/Adapters/DisposableOs/DisposableOsRunResult.cs`
  — the step-list model and verdict rule (pure data + parsing, no I/O).
- `tools/release-verify/ExoSnap.Verify/Adapters/DisposableOs/IDisposableOsTransport.cs`
  — `DisposableOsWorkerRequest`, `DisposableOsRun`, `DisposableOsRunKind`, the
  interface, and `DisposableOsRunner`.
- `tools/release-verify/ExoSnap.Verify/Adapters/DisposableOs/SandboxTransport.cs`
  — the Windows Sandbox implementation (staging, `.wsb`, launch, poll, guest path
  translation).
- `tools/release-verify/ExoSnap.Verify/Gates/DisposableOsGates.cs` — the three
  gate bodies: `UpdateDeclineGate`, `UpdateAcceptGate`, `ChocolateyRehearsalGate`.
- `tools/release-verify/ExoSnap.Verify.Tests/Fixtures/disposable-os-steps-pass.json`,
  `disposable-os-steps-fail.json`, `disposable-os-steps-incomplete.json` — the
  three step-list shapes the verdict rule distinguishes.
- `tools/release-verify/ExoSnap.Verify.Tests/DisposableOsTransportTests.cs` —
  contract tests for the model and `SandboxTransport`, plus a
  `PlatformSmokeTests`-style real-Sandbox smoke.
- `tools/release-verify/ExoSnap.Verify.Tests/DisposableOsGateTests.cs` — the
  three gate bodies' unit tests against a fake transport.

Modified files:

- `tools/release-verify/ExoSnap.Verify/Gates/GateServices.cs` — add
  `IDisposableOsRunner DisposableOs` to the `GateServices` record (see Task 4;
  `IDisposableOsRunner` is the interface `DisposableOsRunner` implements, so
  tests can fake it).
- `tools/release-verify/ExoSnap.Verify/Engine/Campaign.cs` — construct
  `SandboxTransport` and `DisposableOsRunner` in `CampaignServices.OpenAsync` and
  pass them into `GateServices`.
- `tools/release-verify/ExoSnap.Verify/Catalog/ReleaseCatalog.cs` — three
  `BodyFor` entries; `MigratedIds()` picks them up automatically.
- `tools/release-verify/ExoSnap.Verify.Tests/Fakes.cs` — `FakeDisposableOsRunner`
  in `GateFakes`; `GateHarness.CreateAsync` passes it into `GateServices`.
- `scripts/lib/ReleaseScenarios.ps1` — delete the `Run` blocks and catalog
  entries for the three ids (they are C#-owned now), and delete
  `Invoke-ReleaseSandboxUpdateRehearsal` / `Invoke-ReleaseSandboxChocolateyRehearsal`.
- `scripts/lib/ReleaseSandbox.ps1` — delete once nothing calls it (Task 8
  confirms this with a repository-wide grep before removing it; if some other
  PowerShell-only gate still depends on it, leave it and note that in the task).
- `docs/dev/release-verify-catalog.md` — regenerated (Task 9), not edited by
  hand.

## Task 1: The step-list model and verdict rule

**Files:**
- Create: `tools/release-verify/ExoSnap.Verify/Adapters/DisposableOs/DisposableOsRunResult.cs`
- Test: `tools/release-verify/ExoSnap.Verify.Tests/DisposableOsTransportTests.cs`
- Test fixtures: `tools/release-verify/ExoSnap.Verify.Tests/Fixtures/disposable-os-steps-pass.json`,
  `disposable-os-steps-fail.json`, `disposable-os-steps-incomplete.json`

**Interfaces:**
- Produces: `DisposableOsStepResult(string Name, bool Ok, string Detail)`;
  `DisposableOsRunResult(IReadOnlyList<DisposableOsStepResult> Steps)` with
  `static DisposableOsRunResult? Parse(string json)`; `DisposableOsVerdictKind`
  enum `{ Pass, Fail, Unverified }`; `DisposableOsVerdict(DisposableOsVerdictKind Kind, string Message)`
  with `static DisposableOsVerdict From(DisposableOsRunResult? result, IReadOnlyList<string> requiredSteps)`.
  Every later task in this plan consumes `DisposableOsVerdict.From`.

The worker's own JSON shape (confirmed against `scripts/lib/ReleaseSandbox.ps1`'s
`Get-ReleaseSandboxStepVerdict` and the worker scripts that write it) is:

```json
{ "steps": [ { "name": "install-base", "ok": true, "detail": "installed 0.9.0" } ] }
```

- [ ] **Step 1: Write the three fixture files**

`tools/release-verify/ExoSnap.Verify.Tests/Fixtures/disposable-os-steps-pass.json`:
```json
{
  "steps": [
    { "name": "install-base", "ok": true, "detail": "installed 0.9.0 from the base MSI" },
    { "name": "decline-offer", "ok": true, "detail": "update.check reported updateAvailable=true" },
    { "name": "decline-apply", "ok": true, "detail": "update.apply returned exit code 1" },
    { "name": "decline-state", "ok": true, "detail": "failureCase=uacDeclined, installState=intact" }
  ]
}
```

`disposable-os-steps-fail.json`:
```json
{
  "steps": [
    { "name": "install-base", "ok": true, "detail": "installed 0.9.0 from the base MSI" },
    { "name": "decline-offer", "ok": true, "detail": "update.check reported updateAvailable=true" },
    { "name": "decline-apply", "ok": false, "detail": "update.apply returned exit code 0, expected 1" }
  ]
}
```

`disposable-os-steps-incomplete.json` (the worker stopped after the first step):
```json
{
  "steps": [
    { "name": "install-base", "ok": true, "detail": "installed 0.9.0 from the base MSI" }
  ]
}
```

- [ ] **Step 2: Write the failing test**

```csharp
using ExoSnap.Verify.Adapters.DisposableOs;

namespace ExoSnap.Verify.Tests;

public sealed class DisposableOsVerdictTests
{
    private static readonly string[] DeclineSteps =
        ["install-base", "decline-offer", "decline-apply", "decline-state"];

    [Fact]
    public void EveryRequiredStepPresentAndOkIsPass()
    {
        var result = DisposableOsRunResult.Parse(Fixtures.Read("disposable-os-steps-pass.json"));

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Pass, verdict.Kind);
        Assert.Contains("4", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AStepThatRanAndFailedIsFail()
    {
        var result = DisposableOsRunResult.Parse(Fixtures.Read("disposable-os-steps-fail.json"));

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Fail, verdict.Kind);
        Assert.Contains("decline-apply", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ARequiredStepTheWorkerNeverReachedIsUnverified()
    {
        var result = DisposableOsRunResult.Parse(Fixtures.Read("disposable-os-steps-incomplete.json"));

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("decline-offer", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void NoResultDocumentAtAllIsUnverified()
    {
        var verdict = DisposableOsVerdict.From(null, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("no result document", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void UnparseableJsonParsesToNull()
    {
        Assert.Null(DisposableOsRunResult.Parse("{ not json"));
    }
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter DisposableOsVerdictTests`
Expected: FAIL to compile — `ExoSnap.Verify.Adapters.DisposableOs` does not exist yet.

- [ ] **Step 4: Write the model**

```csharp
using System.Text.Json;
using System.Text.Json.Serialization;

namespace ExoSnap.Verify.Adapters.DisposableOs;

/// <summary>One step a disposable-OS worker script recorded, in the order it ran them.</summary>
/// <param name="Name">The step's stable name, matched against a gate's required-step list.</param>
/// <param name="Ok">Whether the step succeeded.</param>
/// <param name="Detail">One sentence about what the step observed.</param>
public sealed record DisposableOsStepResult(string Name, bool Ok, string Detail);

/// <summary>
/// The result document a disposable-OS worker script writes: every step it
/// reached, in the shape <c>scripts/lib/sandbox-*-worker.ps1</c> already write.
/// </summary>
/// <param name="Steps">Every step the worker reached, in run order.</param>
public sealed record DisposableOsRunResult(IReadOnlyList<DisposableOsStepResult> Steps)
{
    /// <summary>Parses a worker's result document, or null when it is not valid JSON.</summary>
    public static DisposableOsRunResult? Parse(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        try
        {
            var document = JsonSerializer.Deserialize(json, DisposableOsJson.Default.DisposableOsRunResult);
            return document is null ? null : document with { Steps = document.Steps ?? [] };
        }
        catch (JsonException)
        {
            return null;
        }
    }
}

/// <summary>What a disposable-OS run's step list adds up to.</summary>
public enum DisposableOsVerdictKind
{
    /// <summary>Every required step was reached and reported ok.</summary>
    Pass,

    /// <summary>A required step was reached and reported not ok. ExoSnap is wrong.</summary>
    Fail,

    /// <summary>The worker never reached a required step, or wrote no result at all.</summary>
    Unverified,
}

/// <summary>The verdict a step list adds up to, and why.</summary>
/// <param name="Kind">The verdict.</param>
/// <param name="Message">One sentence naming what decided it.</param>
public sealed record DisposableOsVerdict(DisposableOsVerdictKind Kind, string Message)
{
    /// <summary>
    /// Applies the shared disposable-OS verdict rule: a step that ran and failed is
    /// a FAIL; a required step the worker never reached is UNVERIFIED; only a run
    /// where every required step is present and ok is a PASS. A worker that stopped
    /// early otherwise leaves nothing but passes, so completeness is checked before
    /// any step's own flag is trusted.
    /// </summary>
    public static DisposableOsVerdict From(DisposableOsRunResult? result, IReadOnlyList<string> requiredSteps)
    {
        ArgumentNullException.ThrowIfNull(requiredSteps);
        if (result is null)
        {
            return new DisposableOsVerdict(DisposableOsVerdictKind.Unverified, "no result document was produced");
        }

        var byName = result.Steps.ToDictionary(step => step.Name, StringComparer.Ordinal);
        var failed = new List<string>();
        var missing = new List<string>();
        foreach (var name in requiredSteps)
        {
            if (!byName.TryGetValue(name, out var step))
            {
                missing.Add(name);
                continue;
            }

            if (!step.Ok)
            {
                failed.Add($"{name}: {step.Detail}");
            }
        }

        if (failed.Count > 0)
        {
            return new DisposableOsVerdict(
                DisposableOsVerdictKind.Fail,
                $"{failed.Count} step(s) failed: {string.Join(" | ", failed)}");
        }

        if (missing.Count > 0)
        {
            return new DisposableOsVerdict(
                DisposableOsVerdictKind.Unverified,
                $"the worker never reached {missing.Count} required step(s): {string.Join(", ", missing)}");
        }

        return new DisposableOsVerdict(
            DisposableOsVerdictKind.Pass,
            $"all {requiredSteps.Count} required step(s) passed");
    }
}

/// <summary>Source-generated contract for the disposable-OS worker result document.</summary>
[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    DefaultIgnoreCondition = JsonIgnoreCondition.Never)]
[JsonSerializable(typeof(DisposableOsRunResult))]
public sealed partial class DisposableOsJson : JsonSerializerContext;
```

- [ ] **Step 5: Add the fixtures to the test project's copy list**

Check `tools/release-verify/ExoSnap.Verify.Tests/ExoSnap.Verify.Tests.csproj` for
the existing `<Content Include="Fixtures\**" CopyToOutputDirectory="PreserveNewest" />`
item (it already covers every file under `Fixtures/`, so the three new JSON files
need no project-file change — confirm this by building and checking they land in
`bin/Debug/net10.0/Fixtures/`).

- [ ] **Step 6: Run test to verify it passes**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter DisposableOsVerdictTests -v normal`
Expected: PASS, 5 tests.

- [ ] **Step 7: Commit**

```bash
git add tools/release-verify/ExoSnap.Verify/Adapters/DisposableOs/DisposableOsRunResult.cs \
        tools/release-verify/ExoSnap.Verify.Tests/DisposableOsTransportTests.cs \
        tools/release-verify/ExoSnap.Verify.Tests/Fixtures/disposable-os-steps-*.json
git commit -m "feat(verify): disposable-OS worker step verdict, ported from PowerShell"
```

## Task 2: The transport interface and the fallback runner

**Files:**
- Modify: `tools/release-verify/ExoSnap.Verify/Adapters/DisposableOs/IDisposableOsTransport.cs` (create)
- Test: `tools/release-verify/ExoSnap.Verify.Tests/DisposableOsTransportTests.cs`

**Interfaces:**
- Consumes: `DisposableOsRunResult`, `DisposableOsVerdict` from Task 1.
- Produces: `DisposableOsWorkerRequest`; `DisposableOsRunKind { Completed, Faulted, Unavailable }`;
  `DisposableOsRun(DisposableOsRunKind Kind, string Detail, DisposableOsRunResult? Result)`
  with `static Completed/Faulted/Unavailable` factories (mirrors
  `ElevatedWorkerRun` in `ExoSnap.Verify/Adapters/Elevation/IElevatedWorkerHost.cs`
  exactly, on purpose — same shape, same caller-side pattern); `IDisposableOsTransport`
  with `string Name`, `bool Available`, `string UnavailableReason`,
  `Task<DisposableOsRun> RunWorkerAsync(DisposableOsWorkerRequest request, CancellationToken)`;
  `IDisposableOsRunner` with `Task<DisposableOsRun> RunAsync(DisposableOsWorkerRequest request, CancellationToken)`
  and `IReadOnlyList<string> TransportNames`; `DisposableOsRunner : IDisposableOsRunner`.
  Task 3 (`SandboxTransport`) implements `IDisposableOsTransport`. Task 4 wires
  `IDisposableOsRunner` into `GateServices`. Tasks 5-6 (the gates) depend only on
  `IDisposableOsRunner`, `DisposableOsWorkerRequest`, `DisposableOsRun`, and
  `DisposableOsVerdict` — never on a concrete transport.

- [ ] **Step 1: Write the failing test**

```csharp
// appended to DisposableOsTransportTests.cs
public sealed class DisposableOsRunnerTests
{
    private sealed class FixedTransport : IDisposableOsTransport
    {
        private readonly DisposableOsRun run;

        public FixedTransport(string name, bool available, DisposableOsRun run)
        {
            this.Name = name;
            this.Available = available;
            this.run = run;
        }

        public string Name { get; }
        public bool Available { get; }
        public string UnavailableReason => this.Available ? string.Empty : $"{this.Name} is not available";
        public List<DisposableOsWorkerRequest> Requests { get; } = [];

        public Task<DisposableOsRun> RunWorkerAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken)
        {
            this.Requests.Add(request);
            return Task.FromResult(this.run);
        }
    }

    [Fact]
    public async Task SkipsAnUnavailableTransportAndUsesTheNextOne()
    {
        var unavailable = new FixedTransport("primary", available: false, DisposableOsRun.Faulted("unreachable"));
        var completed = DisposableOsRun.Completed(new DisposableOsRunResult([]));
        var fallback = new FixedTransport("fallback", available: true, completed);
        var runner = new DisposableOsRunner([unavailable, fallback]);
        var request = new DisposableOsWorkerRequest("worker.ps1", sourceFiles: [], workerArguments: []);

        var run = await runner.RunAsync(request, TestContext.Current.CancellationToken);

        Assert.Same(completed, run);
        Assert.Empty(unavailable.Requests);
        Assert.Single(fallback.Requests);
    }

    [Fact]
    public async Task NoAvailableTransportIsUnavailableNotFaulted()
    {
        var runner = new DisposableOsRunner(
        [
            new FixedTransport("primary", available: false, DisposableOsRun.Faulted("n/a")),
        ]);
        var request = new DisposableOsWorkerRequest("worker.ps1", sourceFiles: [], workerArguments: []);

        var run = await runner.RunAsync(request, TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Unavailable, run.Kind);
        Assert.Contains("no disposable-OS transport", run.Detail, StringComparison.Ordinal);
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter DisposableOsRunnerTests`
Expected: FAIL to compile — the types do not exist yet.

- [ ] **Step 3: Write the interface and runner**

```csharp
namespace ExoSnap.Verify.Adapters.DisposableOs;

/// <summary>
/// One worker script to run on a disposable machine: the script itself, the files
/// it needs staged alongside it, and the arguments specific to what it is
/// asserting. A transport appends its own trailing arguments (a staging directory,
/// a result path, a marker path, translated into whatever that transport's guest
/// sees) — a gate body never computes a guest path itself.
/// </summary>
/// <param name="WorkerFileName">The script's file name; must be one of <paramref name="SourceFiles"/>.</param>
/// <param name="SourceFiles">
/// Host paths to copy into the staging directory: the worker script and every file
/// it reads (a base MSI, a nuspec, the release package). Never mapped from the
/// working tree directly — a worker that could write into the source it copied
/// from is a worker that can change the thing it is verifying.
/// </param>
/// <param name="WorkerArguments">
/// The worker's own named parameters this gate is responsible for (for example
/// <c>-BaseMsiPath</c>, <c>-UpdateChannel</c>), as raw tokens. The transport
/// appends its own staging/result/marker parameters after these.
/// </param>
public sealed record DisposableOsWorkerRequest(
    string WorkerFileName,
    IReadOnlyList<string> SourceFiles,
    IReadOnlyList<string> WorkerArguments)
{
    /// <summary>How long a run may take before it is reported faulted.</summary>
    public TimeSpan Timeout { get; init; } = TimeSpan.FromMinutes(30);
}

/// <summary>How one disposable-OS worker run ended.</summary>
public enum DisposableOsRunKind
{
    /// <summary>The worker ran and wrote a result document.</summary>
    Completed,

    /// <summary>The worker could not be run or wrote no result. Never a product verdict.</summary>
    Faulted,

    /// <summary>No transport could carry this run out on this machine.</summary>
    Unavailable,
}

/// <summary>What one disposable-OS worker run produced.</summary>
/// <param name="Kind">How it ended.</param>
/// <param name="Detail">One sentence about how it ended.</param>
/// <param name="Result">The worker's result document, present only when <see cref="Kind"/> is Completed.</param>
public sealed record DisposableOsRun(DisposableOsRunKind Kind, string Detail, DisposableOsRunResult? Result)
{
    /// <summary>The worker wrote this result.</summary>
    public static DisposableOsRun Completed(DisposableOsRunResult result) =>
        new(DisposableOsRunKind.Completed, "the worker finished and wrote a result document", result);

    /// <summary>The worker could not be run, or ran and wrote nothing.</summary>
    public static DisposableOsRun Faulted(string detail) => new(DisposableOsRunKind.Faulted, detail, null);

    /// <summary>No transport could carry this run out.</summary>
    public static DisposableOsRun Unavailable(string detail) => new(DisposableOsRunKind.Unavailable, detail, null);
}

/// <summary>One way to run a worker script on a disposable machine.</summary>
public interface IDisposableOsTransport
{
    /// <summary>The transport's name, for evidence and log messages ("sandbox", "vm").</summary>
    string Name { get; }

    /// <summary>Whether this transport is usable on this machine right now.</summary>
    bool Available { get; }

    /// <summary>Why this transport is not usable here, or an empty string when it is.</summary>
    string UnavailableReason { get; }

    /// <summary>Stages the request and runs it, returning what the worker produced.</summary>
    Task<DisposableOsRun> RunWorkerAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken);
}

/// <summary>Runs a worker through the first available transport.</summary>
public interface IDisposableOsRunner
{
    /// <summary>The transports this runner tries, in order.</summary>
    IReadOnlyList<string> TransportNames { get; }

    /// <summary>Runs through the first available transport, or reports Unavailable.</summary>
    Task<DisposableOsRun> RunAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken);
}

/// <summary>
/// Tries each transport in order and uses the first one that is available. Mirrors
/// the PowerShell fallback rule this replaces: a transport that cannot run reports
/// so through <see cref="IDisposableOsTransport.Available"/>, never by throwing, so
/// trying the next one is always safe.
/// </summary>
public sealed class DisposableOsRunner : IDisposableOsRunner
{
    private readonly IReadOnlyList<IDisposableOsTransport> transports;

    /// <summary>Creates a runner over transports, tried in the given order.</summary>
    public DisposableOsRunner(IReadOnlyList<IDisposableOsTransport> transports)
    {
        ArgumentNullException.ThrowIfNull(transports);
        this.transports = transports;
    }

    /// <inheritdoc/>
    public IReadOnlyList<string> TransportNames => [.. this.transports.Select(transport => transport.Name)];

    /// <inheritdoc/>
    public async Task<DisposableOsRun> RunAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        foreach (var transport in this.transports)
        {
            if (!transport.Available)
            {
                continue;
            }

            return await transport.RunWorkerAsync(request, cancellationToken).ConfigureAwait(false);
        }

        var reasons = this.transports.Count == 0
            ? "no transport is configured"
            : string.Join("; ", this.transports.Select(transport => $"{transport.Name}: {transport.UnavailableReason}"));
        return DisposableOsRun.Unavailable($"no disposable-OS transport is available on this machine ({reasons})");
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter DisposableOsRunnerTests -v normal`
Expected: PASS, 2 tests.

- [ ] **Step 5: Commit**

```bash
git add tools/release-verify/ExoSnap.Verify/Adapters/DisposableOs/IDisposableOsTransport.cs \
        tools/release-verify/ExoSnap.Verify.Tests/DisposableOsTransportTests.cs
git commit -m "feat(verify): disposable-OS transport interface and fallback runner"
```

## Task 3: SandboxTransport

**Files:**
- Create: `tools/release-verify/ExoSnap.Verify/Adapters/DisposableOs/SandboxTransport.cs`
- Test: `tools/release-verify/ExoSnap.Verify.Tests/DisposableOsTransportTests.cs`

**Interfaces:**
- Consumes: `IDisposableOsTransport`, `DisposableOsWorkerRequest`, `DisposableOsRun`,
  `DisposableOsRunResult` (Tasks 1-2); `ProcessRunner`/`ProcessRunRequest`/`ProcessRunResult`
  (`ExoSnap.Verify/Processes/`); `ToolResolver`/`ResolvedTool` (`ExoSnap.Verify/Capabilities/ToolResolver.cs`).
- Produces: `SandboxTransport : IDisposableOsTransport`, constructed as
  `new SandboxTransport(ProcessRunner processes, ToolResolver tools, string stagingRoot)`.
  Task 4 constructs one of these in `CampaignServices.OpenAsync`.

Ported from `scripts/lib/ReleaseSandbox.ps1` (`New-ReleaseSandboxStaging`,
`New-ReleaseSandboxConfiguration`, `Start-ReleaseSandboxRun`) — same `.wsb` shape,
same guest desktop path (`C:\Users\WDAGUtilityAccount\Desktop\<staging leaf>`),
same marker-file completion signal (`WindowsSandbox.exe` returns once the machine
is *asked for*, not once the work finishes — only the worker's own marker file
means done), same "no marker means UNVERIFIED, never FAIL" rule. PowerShell 7 is
still mapped in read-only beside the staging directory, because Windows Sandbox
ships Windows PowerShell 5.1 only and the worker scripts declare `#Requires -Version 7.0`.

- [ ] **Step 1: Write the failing tests**

```csharp
// appended to DisposableOsTransportTests.cs
public sealed class SandboxTransportTests : IDisposable
{
    private readonly string stagingRoot = Path.Combine(Path.GetTempPath(), "exosnap-sandbox-transport-tests-" + Guid.NewGuid().ToString("N"));
    private readonly ProcessRunner processes = new();

    public void Dispose()
    {
        this.processes.Dispose();
        if (Directory.Exists(this.stagingRoot))
        {
            Directory.Delete(this.stagingRoot, recursive: true);
        }
    }

    private static ToolResolver UnresolvableTools() => new(
        readEnvironment: _ => null,
        fileExists: _ => false,
        readPath: () => null);

    [Fact]
    public void IsUnavailableWhenWindowsSandboxIsNotResolvable()
    {
        var transport = new SandboxTransport(this.processes, UnresolvableTools(), this.stagingRoot);

        Assert.False(transport.Available);
        Assert.Contains("WindowsSandbox.exe", transport.UnavailableReason, StringComparison.Ordinal);
    }

    [Fact]
    public async Task AMissingSourceFileIsFaultedNotUnavailable()
    {
        var tools = new ToolResolver(
            readEnvironment: _ => null,
            fileExists: path => path.EndsWith("WindowsSandbox.exe", StringComparison.OrdinalIgnoreCase),
            readPath: () => @"C:\Windows\System32");
        var transport = new SandboxTransport(this.processes, tools, this.stagingRoot);
        var request = new DisposableOsWorkerRequest(
            "missing-worker.ps1",
            sourceFiles: [Path.Combine(this.stagingRoot, "does-not-exist", "missing-worker.ps1")],
            workerArguments: []);

        var run = await transport.RunWorkerAsync(request, TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Faulted, run.Kind);
        Assert.Contains("does not exist", run.Detail, StringComparison.Ordinal);
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter SandboxTransportTests`
Expected: FAIL to compile — `SandboxTransport` does not exist yet.

- [ ] **Step 3: Write SandboxTransport**

```csharp
using System.Security;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Adapters.DisposableOs;

/// <summary>
/// Runs a worker inside a fresh Windows Sandbox and waits for its result document.
/// </summary>
/// <remarks>
/// <c>WindowsSandbox.exe</c> returns as soon as the virtual machine is asked for,
/// not when the worker inside it finishes, so completion is read from the
/// worker's own marker file — a sandbox that crashed or was closed by hand leaves
/// no marker, and that is reported as Faulted rather than read as a verdict.
///
/// The staging directory is the only channel in both directions: mapped
/// read-write, it carries the worker and its payload in, and the result document
/// and marker back. Nothing else is mapped from the repository, so a worker
/// cannot reach — and so cannot change — the thing it is verifying. The sandbox
/// logon token is already administrative, so no UAC prompt is ever raised inside
/// it; this transport itself needs no host elevation.
/// </remarks>
public sealed class SandboxTransport : IDisposableOsTransport
{
    /// <summary>The environment variable pinning WindowsSandbox.exe, matching the PowerShell layer's.</summary>
    public const string PathVariable = "EXOSNAP_SANDBOX_EXE";

    private const string GuestDesktop = @"C:\Users\WDAGUtilityAccount\Desktop";

    private static readonly TimeSpan LaunchTimeout = TimeSpan.FromSeconds(120);
    private static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(2);

    private readonly ProcessRunner processes;
    private readonly ResolvedTool sandbox;
    private readonly string stagingRoot;
    private readonly string powerShellHome;

    /// <summary>Creates a transport resolving WindowsSandbox.exe through the real machine.</summary>
    public SandboxTransport(ProcessRunner processes, ToolResolver tools, string stagingRoot)
        : this(processes, tools, stagingRoot, RuntimeEnvironment.GetRuntimeDirectory())
    {
    }

    /// <summary>Creates a transport with an injected PowerShell 7 home, for tests.</summary>
    public SandboxTransport(ProcessRunner processes, ToolResolver tools, string stagingRoot, string powerShellHome)
    {
        ArgumentNullException.ThrowIfNull(processes);
        ArgumentNullException.ThrowIfNull(tools);
        ArgumentException.ThrowIfNullOrWhiteSpace(stagingRoot);
        ArgumentException.ThrowIfNullOrWhiteSpace(powerShellHome);
        this.processes = processes;
        this.sandbox = tools.Resolve("WindowsSandbox", PathVariable);
        this.stagingRoot = stagingRoot;
        this.powerShellHome = powerShellHome;
    }

    /// <inheritdoc/>
    public string Name => "sandbox";

    /// <inheritdoc/>
    public bool Available => this.sandbox.Available && Directory.Exists(this.powerShellHome);

    /// <inheritdoc/>
    public string UnavailableReason => this.Available
        ? string.Empty
        : !this.sandbox.Available
            ? $"WindowsSandbox.exe was not found (checked {PathVariable} and PATH)"
            : $"the PowerShell 7 home '{this.powerShellHome}' does not exist, so the sandbox has no shell to run the worker with";

    /// <inheritdoc/>
    public async Task<DisposableOsRun> RunWorkerAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (!this.Available)
        {
            return DisposableOsRun.Unavailable(this.UnavailableReason);
        }

        var staging = Path.Combine(this.stagingRoot, "sandbox-" + Guid.NewGuid().ToString("N"));
        try
        {
            var stage = StageFiles(staging, request.SourceFiles);
            if (stage is not null)
            {
                return DisposableOsRun.Faulted(stage);
            }

            var resultPath = Path.Combine(staging, "result.json");
            var markerPath = Path.Combine(staging, "done.marker");
            var configurationPath = WriteConfiguration(staging, request.WorkerFileName, request.WorkerArguments, resultPath, markerPath);

            var launch = await this.processes.RunAsync(
                new ProcessRunRequest(this.sandbox.Path!, configurationPath) { Timeout = LaunchTimeout },
                cancellationToken).ConfigureAwait(false);
            if (!launch.Succeeded)
            {
                return DisposableOsRun.Faulted(
                    $"WindowsSandbox.exe exited {launch.ExitCode} without starting the worker: {launch.StandardError}");
            }

            return await this.AwaitResultAsync(markerPath, resultPath, request.Timeout, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            TryDelete(staging);
        }
    }

    private async Task<DisposableOsRun> AwaitResultAsync(
        string markerPath, string resultPath, TimeSpan timeout, CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow.Add(timeout < TimeSpan.Zero ? TimeSpan.Zero : timeout);
        while (!File.Exists(markerPath))
        {
            if (DateTime.UtcNow >= deadline)
            {
                return DisposableOsRun.Faulted(
                    $"the sandbox worker did not finish within {timeout.TotalMinutes:0} minute(s); no marker was written");
            }

            await Task.Delay(PollInterval, cancellationToken).ConfigureAwait(false);
        }

        if (!File.Exists(resultPath))
        {
            return DisposableOsRun.Faulted("the sandbox worker signalled completion but wrote no result document");
        }

        var json = await File.ReadAllTextAsync(resultPath, cancellationToken).ConfigureAwait(false);
        var result = DisposableOsRunResult.Parse(json);
        return result is null
            ? DisposableOsRun.Faulted("the sandbox result document is not valid JSON")
            : DisposableOsRun.Completed(result);
    }

    // Returns null on success, or a detail message naming the missing file.
    private static string? StageFiles(string staging, IReadOnlyList<string> sourceFiles)
    {
        Directory.CreateDirectory(staging);
        foreach (var source in sourceFiles)
        {
            if (!File.Exists(source))
            {
                return $"the sandbox run needs '{source}', which does not exist";
            }

            File.Copy(source, Path.Combine(staging, Path.GetFileName(source)), overwrite: true);
        }

        return null;
    }

    private string WriteConfiguration(
        string staging, string workerFileName, IReadOnlyList<string> workerArguments, string resultPath, string markerPath)
    {
        var guestStaging = GuestPath(staging);
        var guestWorker = Path.Combine(guestStaging, workerFileName);
        var guestShell = Path.Combine(GuestPath(this.powerShellHome), "pwsh.exe");
        var guestResult = Path.Combine(guestStaging, "result.json");
        var guestMarker = Path.Combine(guestStaging, "done.marker");

        var allArguments = workerArguments
            .Append("-StagingDirectory").Append(guestStaging)
            .Append("-ResultPath").Append(guestResult)
            .Append("-MarkerPath").Append(guestMarker);
        var quoted = string.Join(' ', allArguments.Select(argument => "'" + argument.Replace("'", "''") + "'"));
        var command = SecurityElement.Escape(
            $"cmd.exe /c \"{guestShell}\" -ExecutionPolicy Bypass -NoProfile -File \"{guestWorker}\" {quoted}");

        var configuration =
            $"""
             <Configuration>
               <VGpu>Disable</VGpu>
               <Networking>Default</Networking>
               <MappedFolders>
                 <MappedFolder>
                   <HostFolder>{staging}</HostFolder>
                   <ReadOnly>false</ReadOnly>
                 </MappedFolder>
                 <MappedFolder>
                   <HostFolder>{this.powerShellHome}</HostFolder>
                   <ReadOnly>true</ReadOnly>
                 </MappedFolder>
               </MappedFolders>
               <LogonCommand>
                 <Command>{command}</Command>
               </LogonCommand>
             </Configuration>
             """;
        var configurationPath = Path.Combine(staging, "release-verify.wsb");
        File.WriteAllText(configurationPath, configuration);
        return configurationPath;
    }

    // Where a host directory mapped into the sandbox appears inside it: the sandbox
    // user's desktop, under the host folder's own leaf name. Fixed by Windows.
    private static string GuestPath(string hostDirectory) =>
        Path.Combine(GuestDesktop, Path.GetFileName(Path.TrimEndingDirectorySeparator(hostDirectory)));

    private static void TryDelete(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Directory.Delete(path, recursive: true);
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
```

Note: `RuntimeEnvironment.GetRuntimeDirectory()` (`System.Runtime.InteropServices.RuntimeEnvironment`)
is a placeholder for "where this process's own PowerShell 7 lives" only in the
sense that it is *wrong* — the harness process is a .NET executable, not `pwsh.exe`,
so its runtime directory is not a PowerShell home. Resolve the real PowerShell 7
home the same way the existing PowerShell layer does (`$PSHOME` when
`ReleaseSandbox.ps1` runs under `pwsh`): add a `ToolResolver.Resolve("pwsh", "EXOSNAP_PWSH")`
call in the real constructor and take `Path.GetDirectoryName(resolved.Path)` as
`powerShellHome`, falling back to `Unavailable` when `pwsh` cannot be found. Update
the single-argument constructor and the `Available`/`UnavailableReason` logic
accordingly before writing the platform smoke in Step 5 — the injected-home
constructor above stays for the unit tests, which never resolve a real `pwsh`.

- [ ] **Step 4: Run test to verify it passes**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter SandboxTransportTests -v normal`
Expected: PASS, 2 tests.

- [ ] **Step 5: Add the real-Sandbox platform smoke**

Append to `tools/release-verify/ExoSnap.Verify.Tests/PlatformSmokeTests.cs`, following
the file's existing `Assert.Skip("<truthful reason>")` pattern (see
`FfprobeReadsARealEncodeItJustMade`, `TheElevatedWorkerSelfTestRoundTripsThroughTheResultFile`):

```csharp
[Fact]
public async Task SandboxTransportRunsARealWorkerAndReadsItsMarker()
{
    var tools = new ToolResolver();
    if (!tools.Resolve("WindowsSandbox", SandboxTransport.PathVariable).Available)
    {
        Assert.Skip("Windows Sandbox is not enabled on this machine");
    }

    using var processes = new ProcessRunner();
    var stagingRoot = Path.Combine(Path.GetTempPath(), "exosnap-sandbox-smoke");
    var transport = new SandboxTransport(processes, tools, stagingRoot);
    var worker = Path.Combine(this.TemporaryDirectory, "smoke-worker.ps1");
    // A minimal worker: no product under test, just proves the staging, launch,
    // marker-poll and result-read path works end to end against a real sandbox.
    await File.WriteAllTextAsync(worker, """
        param([string] $StagingDirectory, [string] $ResultPath, [string] $MarkerPath)
        Set-Content -LiteralPath $ResultPath -Value '{"steps":[{"name":"ran","ok":true,"detail":"smoke"}]}'
        New-Item -ItemType File -Path $MarkerPath -Force | Out-Null
        """);

    var run = await transport.RunWorkerAsync(
        new DisposableOsWorkerRequest("smoke-worker.ps1", [worker], []) { Timeout = TimeSpan.FromMinutes(5) },
        TestContext.Current.CancellationToken);

    Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
    var step = Assert.Single(run.Result!.Steps);
    Assert.Equal("ran", step.Name);
    Assert.True(step.Ok);
}
```

Check the class this is appended to for an existing `TemporaryDirectory` helper
property or fixture (`PlatformSmokeTests.cs` already manages scratch directories
for its other real-tool smokes); reuse that rather than inventing a second
temp-directory convention in the same file.

- [ ] **Step 6: Run the smoke locally (developer machine only)**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter SandboxTransportRunsARealWorkerAndReadsItsMarker -v normal`
Expected: PASS on a machine with Windows Sandbox enabled (takes 1-3 minutes — a
real virtual machine boots); `Assert.Skip` everywhere else, including CI.

- [ ] **Step 7: Commit**

```bash
git add tools/release-verify/ExoSnap.Verify/Adapters/DisposableOs/SandboxTransport.cs \
        tools/release-verify/ExoSnap.Verify.Tests/DisposableOsTransportTests.cs \
        tools/release-verify/ExoSnap.Verify.Tests/PlatformSmokeTests.cs
git commit -m "feat(verify): SandboxTransport, ported from ReleaseSandbox.ps1"
```

## Task 4: Wire the runner into GateServices

**Files:**
- Modify: `tools/release-verify/ExoSnap.Verify/Gates/GateServices.cs`
- Modify: `tools/release-verify/ExoSnap.Verify/Engine/Campaign.cs`
- Modify: `tools/release-verify/ExoSnap.Verify.Tests/Fakes.cs`
- Test: `tools/release-verify/ExoSnap.Verify.Tests/DisposableOsGateTests.cs` (create; the harness-plumbing test lives here since it needs a real gate to exercise `RequireServices()` — Task 5 fills this file's actual gate tests in)

**Interfaces:**
- Consumes: `IDisposableOsRunner` (Task 2), `SandboxTransport` (Task 3).
- Produces: `GateServices.DisposableOs` (type `IDisposableOsRunner`); `GateFakes.DisposableOs`
  (type `FakeDisposableOsRunner`, settable `Run` like `FakeElevatedWorkerHost.Run`).
  Tasks 5-6 read `context.RequireServices().DisposableOs` from their gate bodies.

- [ ] **Step 1: Add the field to GateServices**

In `tools/release-verify/ExoSnap.Verify/Gates/GateServices.cs`, add
`using ExoSnap.Verify.Adapters.DisposableOs;` and a new parameter to the
`GateServices` record, placed after `ElevatedWorker` (before the two optional
trailing parameters) — same "always present, an `Available`/verdict of its own
tells the story" pattern as `ElevatedWorker`:

```csharp
public sealed record GateServices(
    ArtifactUnderTest Artifact,
    IGateSessionHost Sessions,
    IFfprobe Ffprobe,
    EnvironmentOrchestrator Environment,
    IEnvctl Envctl,
    IPresentMon PresentMon,
    ProcessRunner Processes,
    ILiveVerifySessionFactory SessionFactory,
    IUiAutomation Uia,
    ISystemAppearance SystemAppearance,
    IElevatedWorkerHost ElevatedWorker,
    IDisposableOsRunner DisposableOs,
    PresentConfirmation? LastPresentConfirmation = null,
    string? PresentCapturePath = null);
```

Add one line to the record's doc comment block, matching the style of the
`ElevatedWorker` line above it:
`/// <param name="DisposableOs">Runs a worker script on a disposable machine (Sandbox, or a VM once one exists).</param>`

- [ ] **Step 2: Update Campaign.cs's composition root**

In `CampaignServices.OpenAsync` (`ExoSnap.Verify/Engine/Campaign.cs:71-122`), after
the existing `tools`/`processes` locals:

```csharp
var sandboxStaging = Path.Combine(Path.GetTempPath(), "exosnap-verify-sandbox");
var disposableOs = new DisposableOsRunner([new SandboxTransport(processes, tools, sandboxStaging)]);
```

and add `disposableOs` as the new argument to the `GateServices` constructor call,
in the same position as the record field above (after `new ElevatedWorkerHost(...)`,
before the trailing nulls). Add `using ExoSnap.Verify.Adapters.DisposableOs;` at
the top of the file.

- [ ] **Step 3: Update the test fakes**

In `tools/release-verify/ExoSnap.Verify.Tests/Fakes.cs`, add a fake mirroring
`FakeElevatedWorkerHost` (`Fakes.cs:469-505`):

```csharp
/// <summary>A configurable <see cref="IDisposableOsRunner"/> that starts no process.</summary>
internal sealed class FakeDisposableOsRunner : IDisposableOsRunner
{
    private DisposableOsRun? run;

    public IReadOnlyList<string> TransportNames { get; set; } = ["fake"];

    /// <summary>The run the next <see cref="RunAsync"/> call returns.</summary>
    public DisposableOsRun Run
    {
        set => this.run = value;
    }

    /// <summary>Every request the runner was asked to carry out, in call order.</summary>
    public List<DisposableOsWorkerRequest> Requests { get; } = [];

    public Task<DisposableOsRun> RunAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken)
    {
        this.Requests.Add(request);
        return Task.FromResult(
            this.run ?? throw new InvalidOperationException("FakeDisposableOsRunner.Run was not configured."));
    }
}
```

Add `public FakeDisposableOsRunner DisposableOs { get; } = new();` to `GateFakes`
(next to `ElevatedWorker`, `Fakes.cs:528`), and add `fakes.DisposableOs,` to the
`new GateServices(...)` call in `GateHarness.CreateAsync` (`Fakes.cs:618-631`), in
the same position as Step 1.

- [ ] **Step 4: Write a smoke test proving the plumbing compiles and threads through**

```csharp
// tools/release-verify/ExoSnap.Verify.Tests/DisposableOsGateTests.cs
using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

public sealed class DisposableOsHarnessPlumbingTests
{
    [Fact]
    public async Task TheHarnessDisposableOsFakeIsReachableFromAGateContext()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001",
            fakes => fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult([])),
            TestContext.Current.CancellationToken);

        var services = harness.Context.RequireServices();
        var run = await services.DisposableOs.RunAsync(
            new DisposableOsWorkerRequest("noop.ps1", [], []), TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
        Assert.Single(harness.Fakes.DisposableOs.Requests);
    }
}
```

This test names `REL-UPD-MSI-DECLINE-001` as the scenario id purely to get a valid
`ScenarioDescriptor` out of `ReleaseCatalog.Descriptors()` for `GateHarness.CreateAsync`
to build a context around — at this point in the plan that id's `BodyFor` entry is
still `NotMigratedBody`, which is irrelevant here since the test never calls the
body.

- [ ] **Step 5: Run test, build the whole harness**

Run: `dotnet build tools/release-verify/ExoSnap.Verify.sln` (or the harness's own
solution file — check the repository root for its exact name) then
`dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter TheHarnessDisposableOsFakeIsReachableFromAGateContext -v normal`
Expected: the build succeeds (this is the step that would fail if the new
`GateServices` positional argument was placed wrong anywhere), then PASS.

Run the full existing test project once here too, to catch any other
`new GateServices(...)` call site this plan's file listing missed:
`dotnet test tools/release-verify/ExoSnap.Verify.Tests`
Expected: PASS, no regressions.

- [ ] **Step 6: Commit**

```bash
git add tools/release-verify/ExoSnap.Verify/Gates/GateServices.cs \
        tools/release-verify/ExoSnap.Verify/Engine/Campaign.cs \
        tools/release-verify/ExoSnap.Verify.Tests/Fakes.cs \
        tools/release-verify/ExoSnap.Verify.Tests/DisposableOsGateTests.cs
git commit -m "feat(verify): wire the disposable-OS runner into GateServices"
```

## Task 5: UpdateDeclineGate and UpdateAcceptGate (REL-UPD-MSI-DECLINE-001, REL-UPD-MSI-001)

**Files:**
- Create: `tools/release-verify/ExoSnap.Verify/Gates/DisposableOsGates.cs`
- Modify: `tools/release-verify/ExoSnap.Verify/Catalog/ReleaseCatalog.cs`
- Modify: `tools/release-verify/ExoSnap.Verify.Tests/DisposableOsGateTests.cs`

**Interfaces:**
- Consumes: `IDisposableOsRunner`, `DisposableOsWorkerRequest`, `DisposableOsVerdict`,
  `DisposableOsVerdictKind` (Tasks 1-2); `IScenarioBody`, `ScenarioContext`,
  `ScenarioResult` (`ExoSnap.Verify/Engine/Scenario.cs`, `ExoSnap.Verify/Models/ScenarioResult.cs`).
- Produces: `UpdateDeclineGate : IScenarioBody`, `UpdateAcceptGate : IScenarioBody`.
  `ReleaseCatalog.BodyFor` maps `"REL-UPD-MSI-DECLINE-001" => new UpdateDeclineGate()`
  and `"REL-UPD-MSI-001" => new UpdateAcceptGate()`.

Both gates run the SAME guest script, `scripts/lib/sandbox-update-worker.ps1`
(param block confirmed: `-StagingDirectory`, `-BaseMsiPath`, `-ResultPath`,
`-MarkerPath`, `-UpdateChannel` default `Preview`, `-OfferTimeoutSeconds` default
`90`) — its own description says it runs the decline and the accept steps back to
back in one worker invocation, because the accept half needs the exact
older-build starting point the decline half installed. `UpdateAcceptGate`
`DependsOn: ["REL-UPD-MSI-DECLINE-001"]`, matching the existing PS layer's
`DependsOn` on the same pair. Rather than caching a shared result file across two
separate `ScenarioContext.EvidenceDirectory`s (what the PowerShell layer does via
`checks/update-sandbox/result.json`), each gate runs its own full worker
invocation and reads only the steps its own required-step list names — the worker
is idempotent per run (it always starts from a fresh disposable machine), so this
is simpler than porting the cross-scenario cache and costs one extra worker run
per campaign. If a later measurement shows that duplicate run is too slow, revisit
by having `UpdateAcceptGate` reuse `UpdateDeclineGate`'s evidence directory
instead — do not build that caching now on a guess.

The base MSI (`EXOSNAP_UPDATE_FROM_MSI`, matching the environment variable name
`ReleaseScenarios.ps1` already reads for this pair) is an older official
installer; a campaign with none configured is `Unavailable`, mirroring
`PortableUpdateGate`'s own `EXOSNAP_UPDATE_FROM` handling in the same file family.

- [ ] **Step 1: Write the failing tests**

```csharp
// tools/release-verify/ExoSnap.Verify.Tests/DisposableOsGateTests.cs
using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>REL-UPD-MSI-DECLINE-001: UpdateDeclineGate.</summary>
public sealed class UpdateDeclineGateTests
{
    [Fact]
    public async Task IsUnavailableWhenNoBaseMsiIsConfigured()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001", configure: null, TestContext.Current.CancellationToken);

        var result = await new UpdateDeclineGate(() => null).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("EXOSNAP_UPDATE_FROM_MSI", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsUnavailableWhenNoTransportCanRun()
    {
        var baseMsi = Path.GetTempFileName();
        try
        {
            using var harness = await GateHarness.CreateAsync(
                "REL-UPD-MSI-DECLINE-001",
                fakes => fakes.DisposableOs.Run = DisposableOsRun.Unavailable("no transport"),
                TestContext.Current.CancellationToken);

            var result = await new UpdateDeclineGate(() => baseMsi).RunAsync(
                harness.Context, TestContext.Current.CancellationToken);

            Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        }
        finally
        {
            File.Delete(baseMsi);
        }
    }

    [Fact]
    public async Task AllRequiredStepsPassingIsPass()
    {
        var baseMsi = Path.GetTempFileName();
        try
        {
            using var harness = await GateHarness.CreateAsync(
                "REL-UPD-MSI-DECLINE-001",
                fakes => fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult(
                [
                    new("install-base", true, "installed"),
                    new("decline-offer", true, "offered"),
                    new("decline-apply", true, "applied"),
                    new("decline-state", true, "failureCase=uacDeclined, installState=intact"),
                ])),
                TestContext.Current.CancellationToken);

            var result = await new UpdateDeclineGate(() => baseMsi).RunAsync(
                harness.Context, TestContext.Current.CancellationToken);

            Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
            var request = Assert.Single(harness.Fakes.DisposableOs.Requests);
            Assert.Equal("sandbox-update-worker.ps1", request.WorkerFileName);
            Assert.Contains(baseMsi, request.SourceFiles);
        }
        finally
        {
            File.Delete(baseMsi);
        }
    }

    [Fact]
    public async Task AFailedStepIsFail()
    {
        var baseMsi = Path.GetTempFileName();
        try
        {
            using var harness = await GateHarness.CreateAsync(
                "REL-UPD-MSI-DECLINE-001",
                fakes => fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult(
                [
                    new("install-base", true, "installed"),
                    new("decline-offer", false, "update.check timed out"),
                ])),
                TestContext.Current.CancellationToken);

            var result = await new UpdateDeclineGate(() => baseMsi).RunAsync(
                harness.Context, TestContext.Current.CancellationToken);

            Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        }
        finally
        {
            File.Delete(baseMsi);
        }
    }
}

/// <summary>REL-UPD-MSI-001: UpdateAcceptGate.</summary>
public sealed class UpdateAcceptGateTests
{
    [Fact]
    public async Task AllRequiredStepsPassingIsPass()
    {
        var baseMsi = Path.GetTempFileName();
        try
        {
            using var harness = await GateHarness.CreateAsync(
                "REL-UPD-MSI-001",
                fakes => fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult(
                [
                    new("install-base", true, "installed"),
                    new("updater-gone-before-accept", true, "no stale updater"),
                    new("accept-offer", true, "offered"),
                    new("accept-apply", true, "applied"),
                    new("accept-installed", true, "product version advanced"),
                ])),
                TestContext.Current.CancellationToken);

            var result = await new UpdateAcceptGate(() => baseMsi).RunAsync(
                harness.Context, TestContext.Current.CancellationToken);

            Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        }
        finally
        {
            File.Delete(baseMsi);
        }
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter "UpdateDeclineGateTests|UpdateAcceptGateTests"`
Expected: FAIL to compile — `UpdateDeclineGate`/`UpdateAcceptGate` do not exist yet.

- [ ] **Step 3: Write the two gates**

```csharp
// tools/release-verify/ExoSnap.Verify/Gates/DisposableOsGates.cs
using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-UPD-MSI-DECLINE-001: declining the elevation prompt yields a truthful
/// cancel state.
/// </summary>
/// <remarks>
/// Runs on a disposable machine (Windows Sandbox today; a VM will be a second,
/// preferred transport later without this gate changing). The sandbox logon
/// token is already administrative, so no UAC prompt is ever raised — the
/// decline is produced by the updater's own fault-injection seam
/// (<c>EXOSNAP_UPDATER_FAULT=uacDeclined</c>) inside
/// <c>scripts/lib/sandbox-update-worker.ps1</c>, which returns
/// <c>ERROR_CANCELLED</c> from the elevation call the same way a real declined
/// prompt would. See ADR 0067 ("cancel is not failure"): the product assertion is
/// <c>failureCase == uacDeclined &amp;&amp; installState == intact</c>, and this
/// gate deliberately never asserts on <c>phase</c> — a decline legitimately
/// reports <c>phase: failed</c>.
/// </remarks>
public sealed class UpdateDeclineGate : IScenarioBody
{
    /// <summary>The guest worker script, staged from the repository root.</summary>
    public const string WorkerFileName = "sandbox-update-worker.ps1";

    /// <summary>Relative path to the worker, from the repository root.</summary>
    public const string WorkerScriptPath = "scripts/lib/" + WorkerFileName;

    /// <summary>The variable naming the older build both the decline and accept gates start from.</summary>
    public const string BaseMsiVariable = "EXOSNAP_UPDATE_FROM_MSI";

    private static readonly string[] RequiredSteps =
        ["install-base", "decline-offer", "decline-apply", "decline-state"];

    private readonly Func<string?> readBaseMsi;

    /// <summary>Creates the gate reading the real process environment.</summary>
    public UpdateDeclineGate()
        : this(() => Environment.GetEnvironmentVariable(BaseMsiVariable))
    {
    }

    /// <summary>Creates the gate with an injected environment reader.</summary>
    public UpdateDeclineGate(Func<string?> readBaseMsi)
    {
        ArgumentNullException.ThrowIfNull(readBaseMsi);
        this.readBaseMsi = readBaseMsi;
    }

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var baseMsi = this.readBaseMsi();
        if (string.IsNullOrWhiteSpace(baseMsi) || !File.Exists(baseMsi))
        {
            return ScenarioResult.Unavailable(
                $"set {BaseMsiVariable} to an older official ExoSnap MSI; the update gates need a starting point older than the bound artifact");
        }

        var worker = Path.Combine(services.Artifact.RepositoryRoot, WorkerScriptPath);
        if (!File.Exists(worker))
        {
            return ScenarioResult.Unavailable($"{WorkerScriptPath} is missing");
        }

        var request = new DisposableOsWorkerRequest(
            WorkerFileName,
            sourceFiles: [worker, baseMsi],
            workerArguments: ["-BaseMsiPath", Path.GetFileName(baseMsi)]);
        var run = await services.DisposableOs.RunAsync(request, cancellationToken).ConfigureAwait(false);

        return run.Kind switch
        {
            DisposableOsRunKind.Unavailable => ScenarioResult.Unavailable(run.Detail),
            DisposableOsRunKind.Faulted => ScenarioResult.InfrastructureError(run.Detail),
            DisposableOsRunKind.Completed => ToScenarioResult(DisposableOsVerdict.From(run.Result, RequiredSteps)),
            _ => ScenarioResult.InfrastructureError($"unrecognized disposable-OS run kind {run.Kind}"),
        };
    }

    internal static ScenarioResult ToScenarioResult(DisposableOsVerdict verdict) => verdict.Kind switch
    {
        DisposableOsVerdictKind.Pass => ScenarioResult.Pass(verdict.Message),
        DisposableOsVerdictKind.Fail => ScenarioResult.Fail(verdict.Message),
        DisposableOsVerdictKind.Unverified => ScenarioResult.InfrastructureError(verdict.Message),
        _ => ScenarioResult.InfrastructureError($"unrecognized disposable-OS verdict kind {verdict.Kind}"),
    };
}

/// <summary>REL-UPD-MSI-001: the MSI update elevates, installs and relaunches.</summary>
/// <remarks>
/// Same disposable machine and the same worker script as
/// <see cref="UpdateDeclineGate"/> (it runs the decline and accept halves back to
/// back, because the accept half needs the exact older-build starting point the
/// decline half installed) — this gate reads the accept-specific required steps
/// out of the same kind of result document. <c>DependsOn</c>
/// <c>REL-UPD-MSI-DECLINE-001</c> in the catalog for that reason.
/// </remarks>
public sealed class UpdateAcceptGate : IScenarioBody
{
    private static readonly string[] RequiredSteps =
        ["install-base", "updater-gone-before-accept", "accept-offer", "accept-apply", "accept-installed"];

    private readonly Func<string?> readBaseMsi;

    /// <summary>Creates the gate reading the real process environment.</summary>
    public UpdateAcceptGate()
        : this(() => Environment.GetEnvironmentVariable(UpdateDeclineGate.BaseMsiVariable))
    {
    }

    /// <summary>Creates the gate with an injected environment reader.</summary>
    public UpdateAcceptGate(Func<string?> readBaseMsi)
    {
        ArgumentNullException.ThrowIfNull(readBaseMsi);
        this.readBaseMsi = readBaseMsi;
    }

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var baseMsi = this.readBaseMsi();
        if (string.IsNullOrWhiteSpace(baseMsi) || !File.Exists(baseMsi))
        {
            return ScenarioResult.Unavailable(
                $"set {UpdateDeclineGate.BaseMsiVariable} to an older official ExoSnap MSI; the update gates need a starting point older than the bound artifact");
        }

        var worker = Path.Combine(services.Artifact.RepositoryRoot, UpdateDeclineGate.WorkerScriptPath);
        if (!File.Exists(worker))
        {
            return ScenarioResult.Unavailable($"{UpdateDeclineGate.WorkerScriptPath} is missing");
        }

        var request = new DisposableOsWorkerRequest(
            UpdateDeclineGate.WorkerFileName,
            sourceFiles: [worker, baseMsi],
            workerArguments: ["-BaseMsiPath", Path.GetFileName(baseMsi)]);
        var run = await services.DisposableOs.RunAsync(request, cancellationToken).ConfigureAwait(false);

        return run.Kind switch
        {
            DisposableOsRunKind.Unavailable => ScenarioResult.Unavailable(run.Detail),
            DisposableOsRunKind.Faulted => ScenarioResult.InfrastructureError(run.Detail),
            DisposableOsRunKind.Completed => UpdateDeclineGate.ToScenarioResult(DisposableOsVerdict.From(run.Result, RequiredSteps)),
            _ => ScenarioResult.InfrastructureError($"unrecognized disposable-OS run kind {run.Kind}"),
        };
    }
}
```

- [ ] **Step 4: Register both bodies in the catalog**

In `ReleaseCatalog.cs`'s `BodyFor` switch (`:47-69`), add:

```csharp
"REL-UPD-MSI-DECLINE-001" => new UpdateDeclineGate(),
"REL-UPD-MSI-001" => new UpdateAcceptGate(),
```

In the `Describe(...)` call for `REL-UPD-MSI-001` (`:359-369`), add
`dependsOn: ["REL-UPD-MSI-DECLINE-001"]` — the `Describe` helper already accepts
this parameter (`ReleaseCatalog.cs:422`); it is simply unset today.

- [ ] **Step 5: Run tests to verify they pass**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter "UpdateDeclineGateTests|UpdateAcceptGateTests" -v normal`
Expected: PASS, 6 tests.

- [ ] **Step 6: Commit**

```bash
git add tools/release-verify/ExoSnap.Verify/Gates/DisposableOsGates.cs \
        tools/release-verify/ExoSnap.Verify/Catalog/ReleaseCatalog.cs \
        tools/release-verify/ExoSnap.Verify.Tests/DisposableOsGateTests.cs
git commit -m "feat(verify): migrate REL-UPD-MSI-DECLINE-001 and REL-UPD-MSI-001 to C#"
```

## Task 6: ChocolateyRehearsalGate (REL-PKG-CHOCO-001)

**Files:**
- Modify: `tools/release-verify/ExoSnap.Verify/Gates/DisposableOsGates.cs`
- Modify: `tools/release-verify/ExoSnap.Verify/Catalog/ReleaseCatalog.cs`
- Modify: `tools/release-verify/ExoSnap.Verify.Tests/DisposableOsGateTests.cs`

**Interfaces:**
- Consumes: same as Task 5.
- Produces: `ChocolateyRehearsalGate : IScenarioBody`. `ReleaseCatalog.BodyFor` maps
  `"REL-PKG-CHOCO-001" => new ChocolateyRehearsalGate()`.

Stages `scripts/lib/sandbox-choco-worker.ps1` (param block: `-StagingDirectory`,
`-PackageSource`, `-MsiPath`, `-MsiSha256`, `-EvidenceDirectory`, `-ResultPath`,
`-MarkerPath`), which delegates to `scripts/lib/choco-rehearsal-worker.ps1` inside
the guest — both already exist and are not touched by this task. `PackageSource`
is the `packaging/chocolatey` directory (the nuspec plus supporting files);
`MsiPath`/`MsiSha256` name the release MSI under test and its digest, matching
what `scripts/validate-chocolatey-package.ps1` already computes elsewhere in the
pipeline (reuse that computation rather than re-implementing a SHA-256 read here —
check how the existing PowerShell `REL-PKG-CHOCO-001` `Run` block at
`ReleaseScenarios.ps1:3001-3165` obtains `MsiSha256` today and call the same
computation from the gate, or accept it as a constructor parameter read from
`ArtifactUnderTest` if the hash is already computed upstream in the campaign).
`DependsOn: ["REL-UPD-MSI-001"]`, matching the existing PS `DependsOn` (the
rehearsal's restore step needs the release MSI, and the campaign's other gates
that follow expect ExoSnap installed).

- [ ] **Step 1: Write the failing tests**

```csharp
// appended to DisposableOsGateTests.cs
/// <summary>REL-PKG-CHOCO-001: ChocolateyRehearsalGate.</summary>
public sealed class ChocolateyRehearsalGateTests
{
    [Fact]
    public async Task IsUnavailableWhenThePackageSourceIsMissing()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PKG-CHOCO-001", configure: null, TestContext.Current.CancellationToken);

        var result = await new ChocolateyRehearsalGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("packaging/chocolatey", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task AllRequiredStepsPassingIsPass()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PKG-CHOCO-001",
            fakes =>
            {
                Directory.CreateDirectory(Path.Combine(fakes.RepositoryRoot, "packaging", "chocolatey"));
                File.WriteAllText(Path.Combine(fakes.RepositoryRoot, "packaging", "chocolatey", "exosnap.nuspec"), "<xml/>");
                fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult(
                [
                    new("prepare", true, "nuspec rewritten"),
                    new("pack", true, "packed"),
                    new("removeExisting", true, "no prior install"),
                    new("install", true, "installed"),
                    new("uninstall", true, "uninstalled"),
                    new("restore", true, "release MSI reinstalled"),
                ]));
            },
            TestContext.Current.CancellationToken);
        var msi = Path.GetTempFileName();
        try
        {
            var result = await new ChocolateyRehearsalGate(() => msi).RunAsync(
                harness.Context, TestContext.Current.CancellationToken);

            Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        }
        finally
        {
            File.Delete(msi);
        }
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter ChocolateyRehearsalGateTests`
Expected: FAIL to compile.

- [ ] **Step 3: Write the gate**

```csharp
// appended to DisposableOsGates.cs
/// <summary>
/// REL-PKG-CHOCO-001: the Chocolatey package installs, uninstalls and leaves the
/// machine as it was.
/// </summary>
/// <remarks>
/// The only gate that installs software from a package manager, so it runs on a
/// disposable machine rather than the real one. The release MSI is reinstalled by
/// the worker's own <c>finally</c> block whatever happens; the Visual C++
/// redistributable a Chocolatey dependency may have upgraded is deliberately not
/// rolled back (downgrading a runtime is worse than the change), and its version
/// before and after is recorded in the worker's own evidence for a person to read.
/// </remarks>
public sealed class ChocolateyRehearsalGate : IScenarioBody
{
    /// <summary>The guest worker script, staged from the repository root.</summary>
    public const string WorkerFileName = "sandbox-choco-worker.ps1";

    /// <summary>Relative path to the worker, from the repository root.</summary>
    public const string WorkerScriptPath = "scripts/lib/" + WorkerFileName;

    /// <summary>Relative path to the Chocolatey package sources, from the repository root.</summary>
    public const string PackageSourcePath = "packaging/chocolatey";

    private static readonly string[] RequiredSteps =
        ["prepare", "pack", "removeExisting", "install", "uninstall", "restore"];

    private readonly Func<string?> readReleaseMsi;

    /// <summary>Creates the gate reading the release MSI path off the bound artifact.</summary>
    public ChocolateyRehearsalGate()
        : this(() => null)
    {
    }

    /// <summary>Creates the gate with an injected release-MSI reader, for tests.</summary>
    public ChocolateyRehearsalGate(Func<string?> readReleaseMsi)
    {
        ArgumentNullException.ThrowIfNull(readReleaseMsi);
        this.readReleaseMsi = readReleaseMsi;
    }

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var packageSource = Path.Combine(services.Artifact.RepositoryRoot, PackageSourcePath);
        if (!Directory.Exists(packageSource))
        {
            return ScenarioResult.Unavailable($"{PackageSourcePath} is missing");
        }

        var worker = Path.Combine(services.Artifact.RepositoryRoot, WorkerScriptPath);
        if (!File.Exists(worker))
        {
            return ScenarioResult.Unavailable($"{WorkerScriptPath} is missing");
        }

        var releaseMsi = this.readReleaseMsi();
        if (string.IsNullOrWhiteSpace(releaseMsi) || !File.Exists(releaseMsi))
        {
            return ScenarioResult.Unavailable("the release MSI under test could not be located for the restore step");
        }

        var sha256 = Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(await File.ReadAllBytesAsync(releaseMsi, cancellationToken).ConfigureAwait(false)));
        var packageFiles = Directory.GetFiles(packageSource, "*", SearchOption.AllDirectories);
        var request = new DisposableOsWorkerRequest(
            WorkerFileName,
            sourceFiles: [worker, releaseMsi, .. packageFiles],
            workerArguments: ["-PackageSource", Path.GetFileName(packageSource), "-MsiPath", Path.GetFileName(releaseMsi), "-MsiSha256", sha256]);
        var run = await services.DisposableOs.RunAsync(request, cancellationToken).ConfigureAwait(false);

        return run.Kind switch
        {
            DisposableOsRunKind.Unavailable => ScenarioResult.Unavailable(run.Detail),
            DisposableOsRunKind.Faulted => ScenarioResult.InfrastructureError(run.Detail),
            DisposableOsRunKind.Completed => UpdateDeclineGate.ToScenarioResult(DisposableOsVerdict.From(run.Result, RequiredSteps)),
            _ => ScenarioResult.InfrastructureError($"unrecognized disposable-OS run kind {run.Kind}"),
        };
    }
}
```

`sourceFiles: [worker, releaseMsi, .. packageFiles]` stages the nuspec and every
supporting file flat into one staging directory — check against
`sandbox-choco-worker.ps1`'s own reading of `-PackageSource` (it may expect a
subdirectory rather than a flattened copy; read the worker script's `prepare`-step
handling before trusting this flattening, and adjust `SandboxTransport.StageFiles`
in Task 3 to preserve subdirectory structure if the worker needs
`$PackageSource\exosnap.nuspec` rather than a same-directory sibling — this is a
detail Task 3 marked as staging "into the same directory" that this task's
integration test (Step 2 below) will catch if wrong).

- [ ] **Step 4: Register in the catalog**

In `ReleaseCatalog.cs`'s `BodyFor` switch, add `"REL-PKG-CHOCO-001" => new ChocolateyRehearsalGate(),`.
In its `Describe(...)` call (`:371-382`), add `dependsOn: ["REL-UPD-MSI-001"]`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter ChocolateyRehearsalGateTests -v normal`
Expected: PASS, 2 tests.

- [ ] **Step 6: Commit**

```bash
git add tools/release-verify/ExoSnap.Verify/Gates/DisposableOsGates.cs \
        tools/release-verify/ExoSnap.Verify/Catalog/ReleaseCatalog.cs \
        tools/release-verify/ExoSnap.Verify.Tests/DisposableOsGateTests.cs
git commit -m "feat(verify): migrate REL-PKG-CHOCO-001 to C#"
```

## Task 7: Retire the PowerShell scenario code for these three ids

**Files:**
- Modify: `scripts/lib/ReleaseScenarios.ps1`
- Modify or delete: `scripts/lib/ReleaseSandbox.ps1`
- Read-only check: everywhere else under `scripts/`

**Interfaces:** none (deletion only) — the C# `ReleaseCatalog.MigratedIds()` is
what the pre-push dispatch already reads to decide which ids the PowerShell
runner should skip. Confirm this dispatch mechanism before deleting anything: it
must already exist and already skip previously migrated ids (`REL-ENV-001`,
`REL-PRESENT-002`, `REL-UPD-PORTABLE-001`, ...), since those PS scenario bodies
were removed in Slice 1/2 without breaking the pre-push run.

- [ ] **Step 1: Find the dispatch mechanism**

Search for how `scripts/release-verify.ps1` (or wherever the pre-push hook enters)
decides not to run a PowerShell scenario whose id is already C#-migrated —
likely a call into the C# catalog (`dotnet run --project ExoSnap.Verify -- ...`)
or a hardcoded skip-list checked against `MigratedIds()`. Grep:
`rg -n "MigratedIds|migrated" scripts/release-verify.ps1 scripts/lib/*.ps1`

- [ ] **Step 2: Remove the three ids' PowerShell bodies**

In `scripts/lib/ReleaseScenarios.ps1`, delete the catalog entries and `Run` blocks
at `:2553-2772` (`REL-UPD-MSI-DECLINE-001`), `:2774-2984` (`REL-UPD-MSI-001`), and
`:3001-3165` (`REL-PKG-CHOCO-001`) — re-locate these exact line ranges first
(other work may have shifted them since this plan was written) with:
`rg -n "REL-UPD-MSI-DECLINE-001|REL-UPD-MSI-001|REL-PKG-CHOCO-001" scripts/lib/ReleaseScenarios.ps1`

Also delete `Invoke-ReleaseSandboxUpdateRehearsal` and
`Invoke-ReleaseSandboxChocolateyRehearsal` from the same file (they were only
called by the three `Run` blocks just removed) — confirm with:
`rg -n "Invoke-ReleaseSandboxUpdateRehearsal|Invoke-ReleaseSandboxChocolateyRehearsal" scripts/`
before deleting, to catch any other caller this plan did not find.

- [ ] **Step 3: Check whether ReleaseSandbox.ps1 has any remaining caller**

`rg -n "New-ReleaseSandboxStaging|New-ReleaseSandboxConfiguration|Start-ReleaseSandboxRun|Get-ReleaseSandboxStepVerdict|ReleaseSandbox\.ps1" scripts/`

If nothing outside `ReleaseSandbox.ps1` itself remains, delete the file and its
import line (`Import-Module ... ReleaseSandbox.ps1`) wherever it appears. If
something else still depends on it, leave it in place and record what, in the PR
description, rather than deleting a module another gate needs.

- [ ] **Step 4: Run the PowerShell surface's own tests, if any exist for this file**

`rg -n "ReleaseScenarios|ReleaseSandbox" scripts/**/*.Tests.ps1 tests/**/*.ps1 2>$null`
if a Pester or similar suite covers this file, run it; otherwise rely on the C#
tests (already green) and a manual `pwsh -File scripts/release-verify.ps1 -DryRun`
(or the project's equivalent dry-run flag) to confirm the scenario list still
parses with the three ids removed.

- [ ] **Step 5: Commit**

```bash
git add scripts/lib/ReleaseScenarios.ps1 scripts/lib/ReleaseSandbox.ps1
git commit -m "chore(verify): retire the PowerShell bodies REL-UPD-MSI-* and REL-PKG-CHOCO-001 replace"
```

(If `ReleaseSandbox.ps1` was deleted rather than modified, `git add` still stages
the deletion.)

## Task 8: Regenerate the catalog status page

**Files:**
- Modify (generated, not hand-edited): `docs/dev/release-verify-catalog.md`

**Interfaces:** none — this task only runs the existing generator.

- [ ] **Step 1: Regenerate**

```
dotnet run --project tools/release-verify/ExoSnap.Verify -- catalog --out docs/dev/release-verify-catalog.md
```

- [ ] **Step 2: Confirm the three rows now show Migrated=yes and the new DependsOn**

`rg -n "REL-UPD-MSI-DECLINE-001|REL-UPD-MSI-001|REL-PKG-CHOCO-001" docs/dev/release-verify-catalog.md`

Expected: `Migrated` column reads `yes` for all three; `REL-UPD-MSI-001` and
`REL-PKG-CHOCO-001` show their new `DependsOn` entries if the generated table
carries that column (check the table header the generator renders,
`Catalog/CatalogStatusPage.cs:71-76`, for its exact column set before assuming
`DependsOn` is shown — if it is not, this step just confirms `Migrated=yes`).

- [ ] **Step 3: Run the catalog status page's own test**

`dotnet test tools/release-verify/ExoSnap.Verify.Tests --filter CatalogStatusPageTests -v normal`
Expected: PASS — this is the exact test that went red after #375/#376 landed
without a regenerated page (see project history); regenerating first avoids
repeating that.

- [ ] **Step 4: Commit**

```bash
git add docs/dev/release-verify-catalog.md
git commit -m "docs(verify): regenerate the catalog status page after the disposable-OS migration"
```

## Final Validation

Once every task above is committed:

1. `dotnet build tools/release-verify/ExoSnap.Verify.sln` (Debug) — confirm the
   whole harness solution still builds clean.
2. `dotnet test tools/release-verify/ExoSnap.Verify.Tests` — full harness test
   project, no filter. Expected: PASS, zero regressions, including the
   `PlatformSmokeTests` (they skip, not fail, on a machine without Sandbox).
3. `pwsh scripts/verify.ps1` (or the repository's documented "run everything"
   entry point) once, per `AGENTS.md`'s "Final validation" rule — this exercises
   the pre-push gate exactly as CI will, including `check-source-hygiene.ps1` on
   every new file (English, ASCII punctuation, no development provenance in the
   new XML doc comments) and `git diff --check`.
4. Manually confirm the known-limitation trade-off (Non-Goals section) is stated
   in the PR description: machines without Windows Sandbox now get `Unavailable`
   from these three gates rather than the old human-driven real-machine path.

## Execution Handoff

Plan complete and saved to
`docs/superpowers/plans/2026-09-11-disposable-os-sandbox-transport.md`. Two
execution options:

1. **Subagent-Driven (recommended)** - dispatch a fresh subagent per task, review
   between tasks, fast iteration.
2. **Inline Execution** - execute tasks in this session using
   `superpowers:executing-plans`, batch execution with checkpoints.

Which approach?
