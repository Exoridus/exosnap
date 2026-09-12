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
}
