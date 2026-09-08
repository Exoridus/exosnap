using System.Collections.ObjectModel;

namespace ExoSnap.Verify.Processes;

/// <summary>
/// Everything one child process produced.
/// </summary>
/// <param name="FileName">The executable that was started.</param>
/// <param name="Arguments">The arguments it was started with.</param>
/// <param name="ExitCode">The child's exit code; meaningless when it timed out.</param>
/// <param name="StandardOutput">Complete standard output.</param>
/// <param name="StandardError">Complete standard error.</param>
/// <param name="TimedOut">Whether the harness killed the child on its deadline.</param>
/// <param name="Duration">Wall-clock time from start to exit or kill.</param>
public sealed record ProcessRunResult(
    string FileName,
    ReadOnlyCollection<string> Arguments,
    int ExitCode,
    string StandardOutput,
    string StandardError,
    bool TimedOut,
    TimeSpan Duration)
{
    /// <summary>Whether the child exited normally with a zero exit code.</summary>
    public bool Succeeded => !this.TimedOut && this.ExitCode == 0;

    /// <summary>
    /// A one-line description of the invocation, for a message that has to say
    /// what was run without pasting an entire transcript.
    /// </summary>
    public string Invocation => $"{this.FileName} {string.Join(' ', this.Arguments)}".TrimEnd();
}
