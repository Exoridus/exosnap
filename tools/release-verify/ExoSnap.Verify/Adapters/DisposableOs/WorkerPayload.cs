using System.Text.RegularExpressions;

namespace ExoSnap.Verify.Adapters.DisposableOs;

/// <summary>
/// What a guest worker script needs staged alongside it, read out of the script
/// rather than listed by hand.
/// </summary>
/// <remarks>
/// <para>
/// A worker runs inside a disposable machine with nothing but its staging
/// directory, so a dependency the gate forgot to stage is a run that fails at the
/// first line that needs it -- and fails with whatever PowerShell says about a
/// missing module, which reads nothing like "the payload was incomplete". Two were
/// missing when this was written: the MSI worker imports
/// <c>LiveVerifyClient.psm1</c> and the Chocolatey worker invokes
/// <c>choco-rehearsal-worker.ps1</c>, which in turn dot-sources
/// <c>ReleaseScenarios.ps1</c> -- a transitive dependency no list mentioned.
/// </para>
/// <para>
/// Hand-maintained lists were how they went missing, so this derives them: the
/// references are found in the script text, and followed through the scripts it
/// pulls in. The contract test then compares what a gate stages against what the
/// scripts actually ask for, and the next relative import someone adds turns a
/// test red instead of a sandbox run months later.
/// </para>
/// <para>
/// Scope, stated so the test's guarantee is not overread: this finds the
/// references a worker resolves against its staging directory or its own script
/// root -- the forms the workers use. A path assembled at runtime from a variable
/// cannot be found by reading the text, and a worker that needs one has to stage
/// it explicitly.
/// </para>
/// </remarks>
public static class WorkerPayload
{
    // Both spellings the workers use, and only those:
    //   Import-Module (Join-Path $StagingDirectory 'LiveVerifyClient.psm1')
    //   & (Join-Path $StagingDirectory 'choco-rehearsal-worker.ps1')
    //   . (Join-Path $PSScriptRoot 'ReleaseScenarios.ps1')
    // The root variable is captured so a reference against something else is not
    // silently treated as a staged sibling.
    private static readonly Regex JoinPathReference = new(
        @"Join-Path\s+\$(?<root>StagingDirectory|PSScriptRoot)\s+'(?<file>[^']+\.(?:ps1|psm1))'",
        RegexOptions.Compiled | RegexOptions.IgnoreCase);

    /// <summary>
    /// Every script file the worker at <paramref name="workerPath"/> pulls in,
    /// directly or through another script, as file names relative to the directory
    /// the worker lives in.
    /// </summary>
    /// <remarks>
    /// The worker itself is not included: a caller stages that by its own path.
    /// Resolution is breadth-first with a visited set, so a pair of scripts that
    /// reference each other terminates instead of recursing.
    /// </remarks>
    /// <param name="workerPath">Host path of the worker script.</param>
    /// <param name="readText">
    /// Reads a script's text. Injected so the contract test can describe a
    /// dependency graph without writing files.
    /// </param>
    public static IReadOnlyList<string> TransitiveScriptDependencies(
        string workerPath,
        Func<string, string?> readText)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(workerPath);
        ArgumentNullException.ThrowIfNull(readText);

        var directory = Path.GetDirectoryName(workerPath) ?? string.Empty;
        var worker = Path.GetFileName(workerPath);

        var found = new List<string>();
        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { worker };
        var pending = new Queue<string>();
        pending.Enqueue(worker);

        while (pending.Count > 0)
        {
            var current = pending.Dequeue();
            var text = readText(Path.Combine(directory, current));
            if (text is null)
            {
                // A script that cannot be read contributes no references. The caller
                // finds out it is missing when it tries to stage it; inventing a
                // dependency here would be a guess.
                continue;
            }

            foreach (Match match in JoinPathReference.Matches(text))
            {
                var file = match.Groups["file"].Value;
                if (!visited.Add(file))
                {
                    continue;
                }

                found.Add(file);
                pending.Enqueue(file);
            }
        }

        return found;
    }

    /// <summary>
    /// The host paths a gate must stage for <paramref name="workerPath"/>: the
    /// worker and every script it pulls in.
    /// </summary>
    public static IReadOnlyList<string> StagingPathsFor(string workerPath, Func<string, string?> readText)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(workerPath);

        var directory = Path.GetDirectoryName(workerPath) ?? string.Empty;
        var paths = new List<string> { workerPath };
        foreach (var dependency in TransitiveScriptDependencies(workerPath, readText))
        {
            paths.Add(Path.Combine(directory, dependency));
        }

        return paths;
    }

    /// <summary>Reads a file, or null when it is not there. The production reader.</summary>
    public static string? ReadFileOrNull(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        return File.Exists(path) ? File.ReadAllText(path) : null;
    }

    /// <summary>
    /// The dependencies of <paramref name="workerPath"/> that are not present in
    /// <paramref name="staged"/>, compared by file name.
    /// </summary>
    /// <remarks>
    /// File names rather than full paths: a gate stages a directory by its own path
    /// and the transport flattens nothing, so what the guest sees alongside the
    /// worker is the leaf name -- which is exactly what the worker's Join-Path
    /// against its staging directory resolves.
    /// </remarks>
    public static IReadOnlyList<string> MissingFrom(
        string workerPath,
        IEnumerable<string> staged,
        Func<string, string?> readText)
    {
        ArgumentNullException.ThrowIfNull(staged);

        var stagedNames = new HashSet<string>(
            staged.Select(Path.GetFileName).Where(name => !string.IsNullOrEmpty(name))!,
            StringComparer.OrdinalIgnoreCase);

        return TransitiveScriptDependencies(workerPath, readText)
            .Where(dependency => !stagedNames.Contains(dependency))
            .ToList();
    }
}
