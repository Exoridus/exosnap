using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Gates;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// What has to be true of a machine before a first start can be measured on it.
/// </summary>
/// <remarks>
/// Clean is a state, not a screen. A machine carrying an earlier install, an earlier
/// configuration, a recovery manifest, update state or a per-user registry key does
/// something different on the first start than a machine with none of it -- so a run
/// on such a machine measures something else, and calling the result a first-start
/// verdict would make it unrepeatable.
///
/// The residue is the harness's problem, never the product's: a machine that was not
/// clean is a setup failure.
/// </remarks>
public sealed class CleanFirstStartTests
{
    [Fact]
    public void AMachineWithNothingOfExoSnapIsClean()
    {
        Assert.True(CleanFirstStartState.Clean.IsClean);
        Assert.Empty(CleanFirstStartState.Clean.DescribeResidue());
    }

    [Fact]
    public void AnInstalledProductIsNamedWithItsVersion()
    {
        var residue = (CleanFirstStartState.Clean with { InstalledProduct = "ExoSnap 0.9.0" }).DescribeResidue();

        Assert.Contains("ExoSnap 0.9.0", residue, StringComparison.Ordinal);
    }

    [Fact]
    public void RecoveryStateIsNamedAsWhatItChanges()
    {
        // The residue that changes what the first start looks like: a recovery
        // manifest opens the recovery surface, and a gate asserting first-start
        // defaults behind it asserts against the wrong window.
        var residue = (CleanFirstStartState.Clean with { RecoveryStatePresent = true }).DescribeResidue();

        Assert.Contains("recovery", residue, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData("UserConfigurationPresent", "configuration")]
    [InlineData("UpdateStatePresent", "update state")]
    [InlineData("UserRegistryPresent", "registry")]
    public void EveryKindOfResidueMakesTheMachineUnclean(string field, string expected)
    {
        var state = field switch
        {
            "UserConfigurationPresent" => CleanFirstStartState.Clean with { UserConfigurationPresent = true },
            "UpdateStatePresent" => CleanFirstStartState.Clean with { UpdateStatePresent = true },
            _ => CleanFirstStartState.Clean with { UserRegistryPresent = true },
        };

        Assert.False(state.IsClean);
        Assert.Contains(expected, state.DescribeResidue(), StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void EveryResidueIsNamedAtOnce()
    {
        // Each of these is cleared by hand or by starting from a fresh image, so
        // learning about them one campaign at a time is one image rebuild per residue.
        var residue = new CleanFirstStartState("ExoSnap 0.9.0", true, true, true, true).DescribeResidue();

        Assert.Contains("ExoSnap 0.9.0", residue, StringComparison.Ordinal);
        Assert.Contains("configuration", residue, StringComparison.Ordinal);
        Assert.Contains("recovery", residue, StringComparison.Ordinal);
        Assert.Contains("update state", residue, StringComparison.Ordinal);
        Assert.Contains("registry", residue, StringComparison.Ordinal);
    }

    [Fact]
    public void AWhitespaceProductNameIsNotAnInstall()
    {
        // The worker reports an empty string for "nothing installed", and a value read
        // out of a registry query can arrive padded.
        Assert.True((CleanFirstStartState.Clean with { InstalledProduct = "   " }).IsClean);
    }
}

/// <summary>
/// What the clean-first-start worker has to declare about its own steps.
/// </summary>
/// <remarks>
/// The precondition, the install and the cleanup build the environment; only what the
/// started application does is a product assertion. A worker that declared the
/// precondition as a product step would report "this machine already had ExoSnap on
/// it" as an ExoSnap defect.
/// </remarks>
public sealed class CleanFirstStartWorkerTests
{
    private static string RepositoryRoot()
    {
        var directory = AppContext.BaseDirectory;
        while (directory is not null && !Directory.Exists(Path.Combine(directory, "scripts", "lib")))
        {
            directory = Path.GetDirectoryName(directory);
        }

        return directory ?? throw new InvalidOperationException("could not locate the repository root");
    }

    private static string WorkerPath =>
        Path.Combine(RepositoryRoot(), "scripts", "lib", CleanFirstStartGate.WorkerFileName);

    [Fact]
    public void TheWorkerIsWhereTheGateExpectsIt()
    {
        Assert.True(File.Exists(WorkerPath), $"{CleanFirstStartGate.WorkerFileName} is missing");
    }

    [Fact]
    public void EveryStepTheWorkerEmitsDeclaresItsKind()
    {
        var lines = File.ReadAllLines(WorkerPath);
        var undeclared = new List<string>();

        for (var index = 0; index < lines.Length; index++)
        {
            var line = lines[index];
            if (!line.Contains("Add-Step -Name", StringComparison.Ordinal)
                || line.Contains("function ", StringComparison.Ordinal))
            {
                continue;
            }

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
                undeclared.Add($"{CleanFirstStartGate.WorkerFileName}:{index + 1}: {line.Trim()}");
            }
        }

        Assert.Empty(undeclared);
    }

    [Fact]
    public void ThePreconditionIsBootstrapAndTheAssertionsAreProduct()
    {
        // Statements, not lines: an Add-Step call is written over two lines with a
        // backtick continuation, and the file is checked out with LF endings.
        var statements = File.ReadAllText(WorkerPath)
            .ReplaceLineEndings("\n")
            .Replace("`\n", string.Empty, StringComparison.Ordinal)
            .Split('\n');

        // Every one of them, not the first that matches: the precondition is recorded
        // twice, once for a clean machine and once for a dirty one, and the failing
        // call is the one that would accuse the product.
        var precondition = statements
            .Where(line => line.Contains("Add-Step -Name 'clean-precondition'", StringComparison.Ordinal))
            .ToList();

        Assert.Equal(2, precondition.Count);
        Assert.All(precondition, line => Assert.Contains("-Kind 'bootstrap'", line, StringComparison.Ordinal));

        // What the started application did is the only thing that can be a defect.
        foreach (var step in CleanFirstStartGate.ProductSteps)
        {
            var emitted = statements
                .Where(line => line.Contains($"Add-Step -Name '{step}'", StringComparison.Ordinal))
                .ToList();

            Assert.NotEmpty(emitted);
            Assert.All(emitted, line => Assert.Contains("-Kind 'product'", line, StringComparison.Ordinal));
        }
    }

    [Fact]
    public void EveryScriptTheWorkerPullsInExists()
    {
        foreach (var path in WorkerPayload.StagingPathsFor(WorkerPath, WorkerPayload.ReadFileOrNull))
        {
            Assert.True(File.Exists(path), $"the worker pulls in {Path.GetFileName(path)}, which does not exist");
        }
    }

    [Fact]
    public void AnEmptyDifferenceListIsTheHealthyFirstStart()
    {
        // Measured on a real first start: settings.snapshot answers seven sections,
        // and 'differences' -- the settings whose effective value had to depart from
        // what was requested -- is empty, because nothing had to be reconciled. The
        // first assertion swept every section and required each to be non-empty, so
        // it reported the healthy state as a product failure. The gate must assert
        // the opposite for that one section.
        var worker = File.ReadAllText(WorkerPath);

        var start = worker.IndexOf("# The sections that carry the settings themselves", StringComparison.Ordinal);
        Assert.True(start >= 0, "the first-start-defaults assertion was not found");
        var end = worker.IndexOf("Add-Step -Name 'first-start-shutdown'", start, StringComparison.Ordinal);
        Assert.True(end > start, "the first-start-defaults assertion has no end");
        var step = worker[start..end];

        // The sweep that produced the false accusation, gone.
        Assert.DoesNotContain("$snapshot.result.PSObject.Properties |", step, StringComparison.Ordinal);

        // A difference on a machine with no stored settings means the defaults could
        // not be applied as written, which IS a first-start defect.
        Assert.Contains("$differences.Count -gt 0", step, StringComparison.Ordinal);

        // The sections that carry settings are named, so a renamed one is noticed
        // rather than swept over.
        foreach (var section in new[] { "requested", "effective", "app", "constraints", "persistence" })
        {
            Assert.Contains($"'{section}'", step, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void EveryIdentityFieldTheWorkerReadsIsOneTheProductAnswersWith()
    {
        // Measured the expensive way once: the worker read `.version` from
        // app.identity, which answers `productVersion`. Under StrictMode a field the
        // product does not carry is an error, so the run ended before a single
        // product step was recorded -- a whole machine build to find a misspelling.
        // The identity object is built in one place, so the contract can be read from
        // the source rather than restated here.
        var source = Path.Combine(
            RepositoryRoot(), "app", "quick", "ExoSnap", "Quick", "QuickLiveVerifySource.cpp");
        Assert.True(File.Exists(source), $"{source} is missing");

        var identity = System.Text.RegularExpressions.Regex.Match(
            File.ReadAllText(source),
            @"QJsonObject QuickLiveVerifySource::Identity\(\) const \{.*?
\}",
            System.Text.RegularExpressions.RegexOptions.Singleline);
        Assert.True(identity.Success, "QuickLiveVerifySource::Identity() was not found");

        var answered = System.Text.RegularExpressions.Regex
            .Matches(identity.Value, @"json\.insert\(QStringLiteral\(""(?<field>[A-Za-z]+)""\)")
            .Select(match => match.Groups["field"].Value)
            .ToHashSet(StringComparer.Ordinal);
        Assert.Contains("productVersion", answered);

        var read = System.Text.RegularExpressions.Regex
            .Matches(File.ReadAllText(WorkerPath), @"\$identity\.result\.(?<field>[A-Za-z]+)")
            .Select(match => match.Groups["field"].Value)
            .Distinct(StringComparer.Ordinal)
            .ToList();
        Assert.NotEmpty(read);

        var unanswered = read.Where(field => !answered.Contains(field)).ToList();
        Assert.True(
            unanswered.Count == 0,
            $"the worker reads {string.Join(", ", unanswered)} from app.identity, which answers "
            + string.Join(", ", answered.OrderBy(field => field, StringComparer.Ordinal)));
    }

    [Fact]
    public async Task TheGateAsksForAnInteractiveGuest()
    {
        // Measured on a real guest: started from a session that owns no desktop, the
        // application exits without opening its control channel, so a run there
        // reports a product that failed to start from a context nothing claims it
        // supports. The request field is what keeps such a transport from carrying
        // this gate at all.
        using var harness = await GateHarness.CreateAsync(
            "REL-INSTALL-CLEAN-001",
            fakes =>
            {
                DisposableOsGateFixture.StageCleanFirstStartWorker(fakes);
                fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult([]));
            },
            TestContext.Current.CancellationToken);
        // The lookup is injected because a real one reads the MSI Property table, and
        // what this case is about is the request the gate composes.
        var msi = DisposableOsGateFixture.StageReleaseMsi(harness);

        var result = await new CleanFirstStartGate(_ => new ReleaseMsiLookup(msi, new string('a', 64), string.Empty))
            .RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.True(
            harness.Fakes.DisposableOs.Requests.Count == 1,
            $"the gate never reached a transport: {result.Outcome} -- {result.Message}");
        var request = Assert.Single(harness.Fakes.DisposableOs.Requests);
        Assert.True(
            request.RequiresInteractiveGuest,
            "the gate starts the application and talks to its control channel, which needs a desktop");
    }

    [Fact]
    public async Task ATransportThatCannotProveADesktopDoesNotCarryTheGate()
    {
        // The meaning of the field, not just its value: a transport that cannot prove
        // the session is filtered out before it runs, so the run is reported as
        // unavailable rather than as a product that would not start.
        var sessionless = new FixedTransport("sandbox", available: true, DisposableOsRun.Completed(
            new DisposableOsRunResult([]))) { ProvesInteractiveGuest = false };
        var runner = new DisposableOsRunner([sessionless]);
        var request = new DisposableOsWorkerRequest("worker.ps1", [], []) { RequiresInteractiveGuest = true };

        var run = await runner.RunAsync(request, TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Unavailable, run.Kind);
        Assert.Empty(sessionless.Requests);
        Assert.NotEqual(DisposableOsRunKind.Faulted, run.Kind);
    }
}
