using System.Collections.ObjectModel;

namespace ExoSnap.Verify.Adapters.PresentMon;

/// <summary>
/// The presentation path a frame took, in the vocabulary ExoSnap reports.
/// </summary>
/// <remarks>
/// Deliberately the product's four values rather than PresentMon's eight. The
/// cross-check asks whether two independent observers agree about what the window
/// did, and a comparison that needed one of them translated at the call site would
/// be a comparison nobody could read.
/// </remarks>
public enum PresentMode
{
    /// <summary>No present has been observed, or the mode is one neither side names.</summary>
    Unknown,

    /// <summary>A DWM-composited window.</summary>
    Composed,

    /// <summary>Flip-model presentation on an overlay plane.</summary>
    IndependentFlip,

    /// <summary>Legacy exclusive fullscreen.</summary>
    ExclusiveFullscreen,
}

/// <summary>One present PresentMon observed.</summary>
/// <param name="Application">The executable image name PresentMon attributed the present to.</param>
/// <param name="ProcessId">The process that presented.</param>
/// <param name="SwapChainAddress">The swap chain, as PresentMon printed it.</param>
/// <param name="Mode">The presentation path, classified into the product's vocabulary.</param>
/// <param name="RawMode">The mode exactly as PresentMon named it, kept so an unknown value is legible.</param>
/// <param name="SyncInterval">The requested sync interval, or null when the column was absent.</param>
/// <param name="AllowsTearing">Whether the present allowed tearing, or null when the column was absent.</param>
/// <param name="CpuStartSeconds">Start time of the present, or null when the column was absent.</param>
/// <param name="FrameTimeMs">Time to the next present, or null when the column was absent.</param>
public sealed record PresentRecord(
    string Application,
    int ProcessId,
    string SwapChainAddress,
    PresentMode Mode,
    string RawMode,
    int? SyncInterval,
    bool? AllowsTearing,
    double? CpuStartSeconds,
    double? FrameTimeMs);

/// <summary>What one PresentMon capture contains.</summary>
/// <param name="Records">Every present, in file order.</param>
/// <param name="Columns">The header, in file order, so a missing column can be named.</param>
public sealed record PresentMonCapture(ReadOnlyCollection<PresentRecord> Records, ReadOnlyCollection<string> Columns)
{
    /// <summary>The presents attributed to one process.</summary>
    public ReadOnlyCollection<PresentRecord> For(int processId) =>
        new([.. this.Records.Where(record => record.ProcessId == processId)]);

    /// <summary>
    /// The mode a process presented in, or <see cref="PresentMode.Unknown"/> when it
    /// presented in more than one or not at all.
    /// </summary>
    /// <remarks>
    /// A window that changed mode mid-capture has no single answer, and returning its
    /// last one would let an agreement check pass on a coincidence.
    /// </remarks>
    public PresentMode DominantModeFor(int processId)
    {
        var modes = this.For(processId).Select(record => record.Mode).Distinct().ToList();
        return modes.Count == 1 ? modes[0] : PresentMode.Unknown;
    }
}

/// <summary>
/// The independent oracle for what a window actually did on screen.
/// </summary>
/// <remarks>
/// ExoSnap's diagnostics agreeing with ExoSnap's automation endpoint is the same
/// truth read twice. PresentMon reads the same ETW events through its own consumer,
/// so an agreement between the two is evidence and a disagreement names which of them
/// is wrong about the window in front of it.
/// </remarks>
public interface IPresentMon
{
    /// <summary>Whether a pinned PresentMon is resolvable on this machine.</summary>
    bool Available { get; }

    /// <summary>Why it is not usable here, or an empty string when it is.</summary>
    /// <remarks>
    /// A reason rather than a bare false: "not on PATH", "needs elevation" and "this
    /// is a virtual machine with no ETW present provider" send a reader to three
    /// different places, and a gate that reported only "unavailable" sent them
    /// nowhere.
    /// </remarks>
    string UnavailableReason { get; }

    /// <summary>Reads a capture PresentMon wrote.</summary>
    /// <exception cref="Processes.ToolContractException">
    /// The file is not the CSV PresentMon's contract promises: no header, or a header
    /// without the columns a verdict is read from.
    /// </exception>
    Task<PresentMonCapture> ReadCaptureAsync(string csvPath, CancellationToken cancellationToken);
}
