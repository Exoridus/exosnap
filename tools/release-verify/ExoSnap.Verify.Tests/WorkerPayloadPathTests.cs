using ExoSnap.Verify.Gates;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Every worker resolves a staged payload against the directory it was staged into.
/// </summary>
/// <remarks>
/// The gates name payloads by file name, because only the transport knows where it
/// put them. A worker that uses such a name verbatim resolves it against its working
/// directory, and the sandbox sets that to the staging directory -- so the bug is
/// invisible there and fatal under the virtual-machine recipe, which sets it to the
/// guest root. It cost a machine build and an msiexec 1619 to find once.
///
/// Read as source rather than executed: running these workers needs a guest. What is
/// checked is the one line that has to exist, for every payload parameter a gate
/// passes as a bare file name.
/// </remarks>
public sealed class WorkerPayloadPathTests
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

    /// <summary>Each entry worker, with the payload parameters its gate passes by name.</summary>
    public static TheoryData<string, string[]> StagedPayloads() => new()
    {
        { CleanFirstStartGate.WorkerFileName, ["MsiPath"] },
        { UpdateDeclineGate.WorkerFileName, ["BaseMsiPath"] },
        { ChocolateyRehearsalGate.WorkerFileName, ["MsiPath", "PackageSource"] },
    };

    [Theory]
    [MemberData(nameof(StagedPayloads))]
    public void EveryStagedPayloadIsResolvedAgainstTheStagingDirectory(string workerFileName, string[] parameters)
    {
        var worker = File.ReadAllText(Path.Combine(RepositoryRoot(), "scripts", "lib", workerFileName));

        Assert.Contains("$StagingDirectory", worker, StringComparison.Ordinal);
        foreach (var parameter in parameters)
        {
            Assert.Contains($"[string] ${parameter},", worker, StringComparison.Ordinal);

            // The rooted test and the join, on that parameter. An absolute path the
            // caller supplies has to survive, so it is a test rather than a blind
            // Join-Path.
            var resolution = System.Text.RegularExpressions.Regex.Match(
                worker,
                @"if \(-not \[System\.IO\.Path\]::IsPathRooted\(\$" + parameter + @"\)\) \{\s*\r?\n?\s*\$"
                + parameter + @" = Join-Path \$StagingDirectory \$" + parameter + @"\s*\r?\n?\s*\}");
            Assert.True(
                resolution.Success,
                $"{workerFileName} does not resolve ${parameter} against $StagingDirectory, so it resolves "
                + "against the working directory -- which is the staging directory only under the sandbox");
        }
    }

    [Fact]
    public void TheGatesPassPayloadsByNameSoTheWorkersHaveToResolveThem()
    {
        // The other half of the contract. If a gate ever passed a full host path
        // instead, the resolution above would be dead code and this pairing would
        // silently stop meaning anything -- so the bare-name form is pinned too.
        var gates = File.ReadAllText(Path.Combine(
            RepositoryRoot(), "tools", "release-verify", "ExoSnap.Verify", "Gates", "DisposableOsGates.cs"));

        foreach (var argument in new[] { "-MsiPath", "-BaseMsiPath", "-PackageSource" })
        {
            var passed = System.Text.RegularExpressions.Regex.Matches(
                gates, @"""" + argument + @""", ([^,\r\n]+)");
            Assert.True(passed.Count > 0, $"no gate passes {argument}");
            Assert.All(
                passed,
                match => Assert.Contains(
                    "Path.GetFileName", match.Groups[1].Value, StringComparison.Ordinal));
        }
    }

    [Fact]
    public void AWorkingDirectoryThatIsNotTheStagingDirectoryStillFindsThePayload()
    {
        // The resolution itself, exercised the way the recipe runs it: the process
        // working directory is the guest root and the payload is one level below, in
        // the harness directory. Running it under the sandbox's own layout would
        // reproduce the coincidence that hid the defect.
        var root = Path.Combine(Path.GetTempPath(), "exosnap-payload-" + Guid.NewGuid().ToString("N"));
        var staging = Path.Combine(root, "harness");
        Directory.CreateDirectory(staging);
        var payload = "ExoSnap-0.9.0-windows-x64.msi";
        File.WriteAllText(Path.Combine(staging, payload), "msi");

        try
        {
            // What every worker does, run from the guest root rather than from staging.
            var script =
                $"Set-Location -LiteralPath '{root}'; " +
                $"$MsiPath = '{payload}'; $StagingDirectory = '{staging}'; " +
                "if (-not [System.IO.Path]::IsPathRooted($MsiPath)) { $MsiPath = Join-Path $StagingDirectory $MsiPath }; " +
                "if (Test-Path -LiteralPath $MsiPath) { 'found' } else { 'missing: ' + $MsiPath }";

            using var process = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = "pwsh",
                ArgumentList = { "-NoProfile", "-NonInteractive", "-Command", script },
                RedirectStandardOutput = true,
                UseShellExecute = false,
            })!;
            var output = process.StandardOutput.ReadToEnd().Trim();
            process.WaitForExit();

            Assert.Equal("found", output);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }
}
