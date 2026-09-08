using ExoSnap.Verify.Json;

namespace ExoSnap.Verify.Engine;

/// <summary>
/// Where one run keeps its state and its evidence.
/// </summary>
/// <remarks>
/// One directory per run, never a shared one. Evidence from two runs in the same
/// place is evidence nobody can attribute, and a qualification record is only
/// worth as much as the files it can still point at.
/// </remarks>
public sealed class RunDirectory
{
    /// <summary>The subdirectory scenario evidence is written into.</summary>
    public const string EvidenceFolderName = "evidence";

    private RunDirectory(string root)
    {
        this.Root = root;
        this.EvidenceRoot = Path.Combine(root, EvidenceFolderName);
        this.StatePath = Path.Combine(root, RunState.FileName);
    }

    /// <summary>Absolute path of the run directory.</summary>
    public string Root { get; }

    /// <summary>Absolute path of the evidence directory.</summary>
    public string EvidenceRoot { get; }

    /// <summary>Absolute path of the run state document.</summary>
    public string StatePath { get; }

    /// <summary>Creates or opens a run directory.</summary>
    public static RunDirectory Open(string root)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(root);
        var full = Path.GetFullPath(root);
        Directory.CreateDirectory(full);
        var directory = new RunDirectory(full);
        Directory.CreateDirectory(directory.EvidenceRoot);
        return directory;
    }

    /// <summary>The evidence directory for one scenario, created on demand.</summary>
    public string EvidenceDirectoryFor(string scenarioId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(scenarioId);
        var path = Path.Combine(this.EvidenceRoot, scenarioId);
        Directory.CreateDirectory(path);
        return path;
    }

    /// <summary>Reads the recorded state, or null when this run has none yet.</summary>
    public RunState? ReadState() =>
        VerifyJson.ReadFile(this.StatePath, VerifyJsonContext.Default.RunState);

    /// <summary>Writes the run state.</summary>
    public void WriteState(RunState state)
    {
        ArgumentNullException.ThrowIfNull(state);
        VerifyJson.WriteFile(this.StatePath, state, VerifyJsonContext.Default.RunState);
    }
}
