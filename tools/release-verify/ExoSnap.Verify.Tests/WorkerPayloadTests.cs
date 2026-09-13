using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Gates;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// What a guest worker needs staged, derived from the scripts instead of listed.
/// </summary>
/// <remarks>
/// A dependency the gate forgot to stage is a run that fails at the first line
/// needing it, with whatever PowerShell says about a missing module -- which reads
/// nothing like "the payload was incomplete". Two were missing when this was
/// written, one of them transitively, and a hand-maintained list is how both got
/// there. These cases hold the derivation; the last one holds it against the real
/// scripts, so the next relative import someone adds turns a test red instead of a
/// sandbox run months later.
/// </remarks>
public sealed class WorkerPayloadTests
{
    private static Func<string, string?> Scripts(Dictionary<string, string> byPath) =>
        path => byPath.TryGetValue(Path.GetFileName(path), out var text) ? text : null;

    [Fact]
    public void AnImportedModuleIsADependency()
    {
        var scripts = Scripts(new()
        {
            ["worker.ps1"] = "Import-Module (Join-Path $StagingDirectory 'LiveVerifyClient.psm1') -Force",
        });

        Assert.Equal(
            ["LiveVerifyClient.psm1"],
            WorkerPayload.TransitiveScriptDependencies("c:/repo/worker.ps1", scripts));
    }

    [Fact]
    public void AnInvokedScriptIsADependency()
    {
        var scripts = Scripts(new()
        {
            ["worker.ps1"] = "& (Join-Path $StagingDirectory 'choco-rehearsal-worker.ps1') -Foo bar",
            ["choco-rehearsal-worker.ps1"] = string.Empty,
        });

        Assert.Equal(
            ["choco-rehearsal-worker.ps1"],
            WorkerPayload.TransitiveScriptDependencies("c:/repo/worker.ps1", scripts));
    }

    [Fact]
    public void ADependencyOfADependencyIsFound()
    {
        // The one a list would miss, and did: the Chocolatey worker invokes the
        // rehearsal worker, which dot-sources the scenario catalog.
        var scripts = Scripts(new()
        {
            ["worker.ps1"] = "& (Join-Path $StagingDirectory 'rehearsal.ps1')",
            ["rehearsal.ps1"] = ". (Join-Path $PSScriptRoot 'ReleaseScenarios.ps1')",
            ["ReleaseScenarios.ps1"] = string.Empty,
        });

        var found = WorkerPayload.TransitiveScriptDependencies("c:/repo/worker.ps1", scripts);

        Assert.Contains("rehearsal.ps1", found);
        Assert.Contains("ReleaseScenarios.ps1", found);
    }

    [Fact]
    public void ACycleTerminates()
    {
        var scripts = Scripts(new()
        {
            ["a.ps1"] = ". (Join-Path $PSScriptRoot 'b.ps1')",
            ["b.ps1"] = ". (Join-Path $PSScriptRoot 'a.ps1')",
        });

        var found = WorkerPayload.TransitiveScriptDependencies("c:/repo/a.ps1", scripts);

        Assert.Equal(["b.ps1"], found);
    }

    [Fact]
    public void AReferenceAgainstSomethingElseIsNotTreatedAsAStagedSibling()
    {
        // A path built from another variable does not resolve against the staging
        // directory, so claiming it as a staged sibling would stage the wrong file
        // and still leave the real one missing.
        var scripts = Scripts(new()
        {
            ["worker.ps1"] = "Import-Module (Join-Path $env:ProgramFiles 'Something.psm1')",
        });

        Assert.Empty(WorkerPayload.TransitiveScriptDependencies("c:/repo/worker.ps1", scripts));
    }

    [Fact]
    public void AnUnreadableScriptContributesNothingRatherThanAGuess()
    {
        var scripts = Scripts(new()
        {
            ["worker.ps1"] = "& (Join-Path $StagingDirectory 'gone.ps1')",
        });

        // gone.ps1 is still a dependency -- the worker asks for it -- but nothing is
        // invented about what IT needs.
        Assert.Equal(["gone.ps1"], WorkerPayload.TransitiveScriptDependencies("c:/repo/worker.ps1", scripts));
    }

    [Fact]
    public void TheWorkerItselfIsStagedFirstAndOnlyOnce()
    {
        var scripts = Scripts(new()
        {
            ["worker.ps1"] = "Import-Module (Join-Path $StagingDirectory 'helper.psm1')",
            ["helper.psm1"] = string.Empty,
        });

        var paths = WorkerPayload.StagingPathsFor("c:/repo/worker.ps1", scripts);

        Assert.Equal(2, paths.Count);
        Assert.Equal("c:/repo/worker.ps1", paths[0]);
        Assert.EndsWith("helper.psm1", paths[1], StringComparison.Ordinal);
    }

    [Fact]
    public void MissingFromNamesWhatAGateDidNotStage()
    {
        var scripts = Scripts(new()
        {
            ["worker.ps1"] = "Import-Module (Join-Path $StagingDirectory 'needed.psm1')",
            ["needed.psm1"] = string.Empty,
        });

        var missing = WorkerPayload.MissingFrom("c:/repo/worker.ps1", ["c:/repo/worker.ps1"], scripts);
        Assert.Equal(["needed.psm1"], missing);

        var complete = WorkerPayload.MissingFrom(
            "c:/repo/worker.ps1", ["c:/repo/worker.ps1", "c:/repo/needed.psm1"], scripts);
        Assert.Empty(complete);
    }

    // ---- The contract, against the scripts that actually ship ----------------

    private static string RepositoryRoot()
    {
        var directory = AppContext.BaseDirectory;
        while (directory is not null && !Directory.Exists(Path.Combine(directory, "scripts", "lib")))
        {
            directory = Path.GetDirectoryName(directory);
        }

        return directory ?? throw new InvalidOperationException("could not locate the repository root");
    }

    [Theory]
    [InlineData(UpdateDeclineGate.WorkerFileName)]
    [InlineData(ChocolateyRehearsalGate.WorkerFileName)]
    public void EveryScriptTheRealWorkerPullsInExists(string workerFileName)
    {
        // Before asking whether a gate stages them: the derivation must be about
        // files that are there. A dependency naming a script nobody has is either a
        // typo in the worker or a stale reference, and either way the sandbox run
        // would be the place it was discovered.
        var worker = Path.Combine(RepositoryRoot(), "scripts", "lib", workerFileName);
        Assert.True(File.Exists(worker), $"{workerFileName} is not where the gate expects it");

        foreach (var path in WorkerPayload.StagingPathsFor(worker, WorkerPayload.ReadFileOrNull))
        {
            Assert.True(File.Exists(path), $"{workerFileName} pulls in {Path.GetFileName(path)}, which does not exist");
        }
    }

    [Fact]
    public void TheUpdateWorkerNeedsTheLiveVerifyClient()
    {
        // Pinned by name, not only by derivation: this is the dependency that was
        // missing, and a test that only said "whatever it imports is staged" would
        // also pass if the import were deleted.
        var worker = Path.Combine(RepositoryRoot(), "scripts", "lib", UpdateDeclineGate.WorkerFileName);
        Assert.Contains(
            "LiveVerifyClient.psm1",
            WorkerPayload.TransitiveScriptDependencies(worker, WorkerPayload.ReadFileOrNull));
    }

    /// <summary>
    /// Every step a shipping worker can emit declares a kind the host recognises.
    /// </summary>
    /// <remarks>
    /// The host refuses to draw a verdict from a step that does not say whether it
    /// asserts the product or builds the environment, which is the safe answer and
    /// not a useful one: a gate that reports "unverified, the steps do not say"
    /// tells nobody what happened. So the workers are held to declaring it, here,
    /// where a new step that forgets turns this red.
    ///
    /// Read out of the script text: these workers construct their steps through one
    /// helper each, and the helper's parameter is mandatory, so an undeclared step
    /// would not run at all -- but a step constructed some other way would, and this
    /// is what would catch it.
    /// </remarks>
    [Theory]
    [InlineData("sandbox-update-worker.ps1", "Add-Step -Name")]
    [InlineData("choco-rehearsal-worker.ps1", "New-Step -Name")]
    public void EveryStepAShippingWorkerEmitsDeclaresItsKind(string workerFileName, string constructor)
    {
        var path = Path.Combine(RepositoryRoot(), "scripts", "lib", workerFileName);
        var lines = File.ReadAllLines(path);

        var undeclared = new List<string>();
        for (var index = 0; index < lines.Length; index++)
        {
            var line = lines[index];
            if (!line.Contains(constructor, StringComparison.Ordinal))
            {
                continue;
            }

            // The definition of the helper itself is not a call site.
            if (line.Contains("function ", StringComparison.Ordinal))
            {
                continue;
            }

            // A call may be continued onto the next line with a backtick.
            var statement = line;
            var cursor = index;
            while (statement.TrimEnd().EndsWith('`') && cursor + 1 < lines.Length)
            {
                cursor++;
                statement += lines[cursor];
            }

            if (!statement.Contains("-Kind 'product'", StringComparison.Ordinal)
                && !statement.Contains("-Kind 'bootstrap'", StringComparison.Ordinal))
            {
                undeclared.Add($"{workerFileName}:{index + 1}: {line.Trim()}");
            }
        }

        Assert.Empty(undeclared);
    }

    [Fact]
    public void TheBootstrapOnlyWorkerDeclaresItsStepsToo()
    {
        // sandbox-choco-worker.ps1 writes its steps as a literal document rather
        // than through a helper -- every one of them a bootstrap failure -- so the
        // field is checked where it is written.
        var path = Path.Combine(RepositoryRoot(), "scripts", "lib", "sandbox-choco-worker.ps1");
        var text = File.ReadAllText(path);

        Assert.Contains("steps       = @([pscustomobject]@{ name = $Step; ok = $false; detail = $Detail; kind = 'bootstrap' })",
            text, StringComparison.Ordinal);
    }

    [Fact]
    public void TheChocolateyWorkerNeedsTheRehearsalWorkerAndItsOwnDependency()
    {
        var worker = Path.Combine(RepositoryRoot(), "scripts", "lib", ChocolateyRehearsalGate.WorkerFileName);
        var dependencies = WorkerPayload.TransitiveScriptDependencies(worker, WorkerPayload.ReadFileOrNull);

        Assert.Contains("choco-rehearsal-worker.ps1", dependencies);
        Assert.Contains("ReleaseScenarios.ps1", dependencies);
    }
}
