namespace ExoSnap.Verify.Capabilities;

/// <summary>Where an external tool was found, if it was found at all.</summary>
/// <param name="Name">The tool's executable name without an extension.</param>
/// <param name="Path">Absolute path, or null when the tool is not present.</param>
/// <param name="Source">How it was located: the environment override, or PATH.</param>
public sealed record ResolvedTool(string Name, string? Path, string Source)
{
    /// <summary>Whether the tool is present on this machine.</summary>
    public bool Available => this.Path is not null;
}

/// <summary>
/// Locates the external tools scenarios use as independent oracles.
/// </summary>
/// <remarks>
/// An explicit environment variable wins over PATH so a run can pin an exact
/// build of a tool whose output it parses. Nothing here executes what it finds:
/// resolution is a capability question, and starting PresentMon or ffprobe to
/// answer it would make a read-only probe into a side effect.
/// </remarks>
public sealed class ToolResolver
{
    private readonly Func<string, string?> readEnvironment;
    private readonly Func<string, bool> fileExists;
    private readonly Func<string?> readPath;

    /// <summary>Resolves against the real process environment.</summary>
    public ToolResolver()
        : this(Environment.GetEnvironmentVariable, File.Exists, () => Environment.GetEnvironmentVariable("PATH"))
    {
    }

    /// <summary>Resolves against injected environment and filesystem readers.</summary>
    public ToolResolver(
        Func<string, string?> readEnvironment,
        Func<string, bool> fileExists,
        Func<string?> readPath)
    {
        this.readEnvironment = readEnvironment;
        this.fileExists = fileExists;
        this.readPath = readPath;
    }

    /// <summary>
    /// Looks for a tool, preferring the named environment override and falling
    /// back to a PATH search for the executable name.
    /// </summary>
    /// <param name="name">Executable name without an extension.</param>
    /// <param name="environmentVariable">Override variable holding a full path.</param>
    public ResolvedTool Resolve(string name, string environmentVariable)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        ArgumentException.ThrowIfNullOrWhiteSpace(environmentVariable);

        var overridePath = this.readEnvironment(environmentVariable);
        if (!string.IsNullOrWhiteSpace(overridePath) && this.fileExists(overridePath))
        {
            return new ResolvedTool(name, Path.GetFullPath(overridePath), environmentVariable);
        }

        var searchPath = this.readPath();
        if (string.IsNullOrWhiteSpace(searchPath))
        {
            return new ResolvedTool(name, null, "PATH");
        }

        foreach (var directory in searchPath.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            foreach (var extension in new[] { ".exe", ".com", string.Empty })
            {
                string candidate;
                try
                {
                    candidate = Path.Combine(directory.Trim(), name + extension);
                }
                catch (ArgumentException)
                {
                    // A malformed PATH entry is a fact about the machine, not a
                    // reason to abandon the remaining entries.
                    continue;
                }

                if (this.fileExists(candidate))
                {
                    return new ResolvedTool(name, Path.GetFullPath(candidate), "PATH");
                }
            }
        }

        return new ResolvedTool(name, null, "PATH");
    }
}
