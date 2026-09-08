namespace ExoSnap.Verify.Adapters.PresentMon;

/// <summary>
/// Reads PresentMon captures from disk.
/// </summary>
/// <remarks>
/// Reading only. Starting PresentMon needs an elevated ETW session, and a harness
/// that opened one on the developer's desktop would be measuring whatever else is
/// presenting there. The capture is produced in the disposable guest that carries the
/// present gates; this side turns it into typed records.
/// </remarks>
public sealed class PresentMonReader : IPresentMon
{
    private readonly string? executablePath;

    /// <summary>Creates a reader for a resolved PresentMon, or for none at all.</summary>
    public PresentMonReader(string? executablePath) => this.executablePath = executablePath;

    /// <inheritdoc/>
    public bool Available => this.executablePath is not null;

    /// <inheritdoc/>
    public string UnavailableReason => this.executablePath is null
        ? "PresentMon is not resolvable on this machine; set EXOSNAP_PRESENTMON to a pinned build"
        : string.Empty;

    /// <inheritdoc/>
    public async Task<PresentMonCapture> ReadCaptureAsync(string csvPath, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(csvPath);
        var text = await File.ReadAllTextAsync(csvPath, cancellationToken).ConfigureAwait(false);
        return PresentMonCsv.Parse(text);
    }
}

/// <summary>What comparing two independent observers of the same window concluded.</summary>
/// <param name="Agreed">Whether both named the same presentation path.</param>
/// <param name="Product">What ExoSnap reported.</param>
/// <param name="Observed">What PresentMon reported.</param>
/// <param name="PresentCount">How many presents PresentMon attributed to the process.</param>
/// <param name="Message">One sentence, in the terms the gate is written in.</param>
public sealed record PresentModeAgreement(
    bool Agreed,
    PresentMode Product,
    PresentMode Observed,
    int PresentCount,
    string Message);

/// <summary>Compares ExoSnap's present diagnostics against PresentMon's own reading.</summary>
public static class PresentModeCrossCheck
{
    /// <summary>
    /// Whether the two observers agree about one process.
    /// </summary>
    /// <remarks>
    /// Three answers, not two. Agreement is evidence; disagreement names a defect in
    /// one of the two readings; and a capture with no present attributed to the
    /// process at all is neither, because nothing was compared. The caller reports
    /// that third case as an infrastructure error rather than a product verdict: a
    /// PresentMon session that saw nothing measured nothing.
    /// </remarks>
    public static PresentModeAgreement Compare(PresentMonCapture capture, int processId, string productMode)
    {
        ArgumentNullException.ThrowIfNull(capture);

        var product = PresentMonCsv.FromProductName(productMode);
        var presents = capture.For(processId);
        var observed = capture.DominantModeFor(processId);

        if (presents.Count == 0)
        {
            return new PresentModeAgreement(
                false,
                product,
                PresentMode.Unknown,
                0,
                $"the capture attributes no present to process {processId.ToString(System.Globalization.CultureInfo.InvariantCulture)}, so the two readings were never compared");
        }

        if (observed == PresentMode.Unknown)
        {
            var named = string.Join(
                ", ",
                presents.Select(record => record.RawMode).Distinct().Order(StringComparer.Ordinal));
            return new PresentModeAgreement(
                false,
                product,
                PresentMode.Unknown,
                presents.Count,
                $"the process presented in more than one mode during the capture ({named}), so it has no single mode to compare");
        }

        if (product == observed)
        {
            return new PresentModeAgreement(
                true,
                product,
                observed,
                presents.Count,
                $"both observers report {PresentMonCsv.ProductName(observed)} across {presents.Count.ToString(System.Globalization.CultureInfo.InvariantCulture)} present(s)");
        }

        return new PresentModeAgreement(
            false,
            product,
            observed,
            presents.Count,
            $"ExoSnap reports {PresentMonCsv.ProductName(product)} and PresentMon reports {PresentMonCsv.ProductName(observed)} for the same window");
    }
}
