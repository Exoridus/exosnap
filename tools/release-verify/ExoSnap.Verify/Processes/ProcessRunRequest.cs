using System.Collections.ObjectModel;

namespace ExoSnap.Verify.Processes;

/// <summary>
/// One child process to start.
/// </summary>
/// <remarks>
/// Arguments are a list, never a command line. Windows re-parses a command line
/// in the child, so a single string turns a path containing a space, an
/// ampersand or a quote into a different invocation than the one that was
/// intended, and the failure looks like the tool misbehaving.
/// </remarks>
public sealed class ProcessRunRequest
{
    /// <summary>Builds a request for an executable and its arguments.</summary>
    public ProcessRunRequest(string fileName, params IReadOnlyList<string> arguments)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(fileName);
        ArgumentNullException.ThrowIfNull(arguments);
        this.FileName = fileName;
        this.Arguments = new ReadOnlyCollection<string>([.. arguments]);
    }

    /// <summary>The executable to start. Never a shell, never a batch file.</summary>
    public string FileName { get; }

    /// <summary>The arguments, passed through without shell interpretation.</summary>
    public ReadOnlyCollection<string> Arguments { get; }

    /// <summary>Working directory, or null for the harness's own.</summary>
    public string? WorkingDirectory { get; init; }

    /// <summary>
    /// How long the child may run before it is killed and the result is reported
    /// as timed out.
    /// </summary>
    public TimeSpan Timeout { get; init; } = TimeSpan.FromMinutes(2);

    /// <summary>Environment variables to add or override for this child.</summary>
    public IReadOnlyDictionary<string, string?> Environment { get; init; } =
        new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase);

    /// <summary>Text written to the child's standard input, then closed.</summary>
    public string? StandardInput { get; init; }
}
