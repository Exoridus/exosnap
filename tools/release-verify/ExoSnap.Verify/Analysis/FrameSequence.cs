using System.Collections.ObjectModel;
using System.Globalization;
using System.Text.RegularExpressions;

namespace ExoSnap.Verify.Analysis;

/// <summary>How one decoded frame sequence departs from a clean monotonic decode.</summary>
public enum FrameSequenceDefect
{
    /// <summary>Presentation timestamps went backwards between two consecutive frames.</summary>
    Backwards,

    /// <summary>Two consecutive frames carry the same presentation timestamp.</summary>
    Duplicate,

    /// <summary>A gap longer than the tolerated multiple of the nominal frame interval.</summary>
    Gap,

    /// <summary>The raw frame count and the timestamp count disagree.</summary>
    CountMismatch,

    /// <summary>The raw file is not a whole number of frames of the declared size.</summary>
    TruncatedFrame,

    /// <summary>The decoder reported errors on standard error.</summary>
    DecoderError,
}

/// <summary>One departure, with the numbers a reader needs to judge it.</summary>
/// <param name="Defect">What kind of departure this is.</param>
/// <param name="FrameIndex">Zero-based index of the frame it was observed at, or -1 when it is a whole-file property.</param>
/// <param name="Detail">The measured values, phrased for a verdict line.</param>
public sealed record FrameSequenceFinding(FrameSequenceDefect Defect, int FrameIndex, string Detail);

/// <summary>
/// The decoded frames of one recording, as timestamps plus whatever was wrong with them.
/// </summary>
/// <remarks>
/// <para>
/// Analysis of a recording reads two files the decoder produced: the raw frames and the
/// log it wrote while producing them. Those can disagree, and the disagreement is often
/// the finding -- a lossy decode that dropped frames, a container whose timestamps run
/// backwards across a split, a truncated write.
/// </para>
/// <para>
/// So none of that throws. A cursor analysis that aborts on a frame-count mismatch
/// reports nothing about the cursor, and the mismatch itself -- the interesting part --
/// reaches the reader as a stack trace. Every departure is a <see cref="FrameSequenceFinding"/>
/// the caller decides about: some are fatal to a particular measurement and harmless to
/// another.
/// </para>
/// </remarks>
public sealed class FrameSequence
{
    // showinfo writes one line per frame; pts_time is the only field read here, because
    // it is the only one that survives every filter chain the harness uses.
    private static readonly Regex PtsTimePattern = new(
        @"pts_time:(?<value>-?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    private static readonly Regex DecoderErrorPattern = new(
        @"(?<![A-Za-z_])(error|corrupt|invalid data found|missing picture)(?![A-Za-z_])",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);

    private FrameSequence(
        ReadOnlyCollection<double> presentationTimes,
        int rawFrameCount,
        ReadOnlyCollection<FrameSequenceFinding> findings)
    {
        this.PresentationTimes = presentationTimes;
        this.RawFrameCount = rawFrameCount;
        this.Findings = findings;
    }

    /// <summary>Presentation timestamps in seconds, in decode order, one per logged frame.</summary>
    public ReadOnlyCollection<double> PresentationTimes { get; }

    /// <summary>Whole frames present in the raw file, from its length and the declared frame size.</summary>
    public int RawFrameCount { get; }

    /// <summary>Everything that departs from a clean monotonic decode.</summary>
    public ReadOnlyCollection<FrameSequenceFinding> Findings { get; }

    /// <summary>Frames both files agree exist, which is what any per-frame measurement may use.</summary>
    public int UsableFrameCount => Math.Min(this.RawFrameCount, this.PresentationTimes.Count);

    /// <summary>Whether the sequence is clean enough for a timing measurement.</summary>
    /// <remarks>
    /// A gap or a duplicate does not disqualify a measurement that reads a frame's own
    /// timestamp -- it disqualifies one that assumes a constant interval. Backwards
    /// timestamps and a truncated frame do disqualify both: neither has a defined
    /// answer to "which frame was showing at time t".
    /// </remarks>
    public bool IsTimebaseTrustworthy =>
        !this.Findings.Any(finding =>
            finding.Defect is FrameSequenceDefect.Backwards or FrameSequenceDefect.TruncatedFrame);

    /// <summary>
    /// Reads a decode's frames and log, and reports how they depart from a clean decode.
    /// </summary>
    /// <param name="rawFrameBytes">Length in bytes of the raw frame file.</param>
    /// <param name="frameSizeBytes">Bytes per frame, from the decoded width, height and pixel format.</param>
    /// <param name="decoderLogLines">Every line the decoder wrote to standard error.</param>
    /// <param name="nominalFrameIntervalSeconds">
    /// The expected interval between frames. A gap is reported at more than
    /// <paramref name="gapTolerance"/> times this. Pass null to derive it from the median
    /// observed interval, which is what a variable-frame-rate capture needs.
    /// </param>
    /// <param name="gapTolerance">Multiples of the nominal interval that count as a gap. Must exceed 1.</param>
    public static FrameSequence Read(
        long rawFrameBytes,
        long frameSizeBytes,
        IEnumerable<string> decoderLogLines,
        double? nominalFrameIntervalSeconds = null,
        double gapTolerance = 1.5)
    {
        ArgumentNullException.ThrowIfNull(decoderLogLines);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(frameSizeBytes);
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(gapTolerance, 1.0);

        var times = new List<double>();
        var findings = new List<FrameSequenceFinding>();
        var decoderErrors = 0;

        foreach (var line in decoderLogLines)
        {
            if (line is null)
            {
                continue;
            }

            var match = PtsTimePattern.Match(line);
            if (match.Success &&
                double.TryParse(
                    match.Groups["value"].Value,
                    NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out var seconds))
            {
                times.Add(seconds);
                continue;
            }

            if (DecoderErrorPattern.IsMatch(line))
            {
                decoderErrors++;
            }
        }

        if (decoderErrors > 0)
        {
            findings.Add(new FrameSequenceFinding(
                FrameSequenceDefect.DecoderError,
                -1,
                $"the decoder reported {decoderErrors.ToString(CultureInfo.InvariantCulture)} error line(s)"));
        }

        var rawFrames = (int)(rawFrameBytes / frameSizeBytes);
        if (rawFrameBytes % frameSizeBytes != 0)
        {
            findings.Add(new FrameSequenceFinding(
                FrameSequenceDefect.TruncatedFrame,
                rawFrames,
                $"{rawFrameBytes.ToString(CultureInfo.InvariantCulture)} bytes is {(rawFrameBytes % frameSizeBytes).ToString(CultureInfo.InvariantCulture)} short of a whole frame of {frameSizeBytes.ToString(CultureInfo.InvariantCulture)}"));
        }

        if (rawFrames != times.Count)
        {
            findings.Add(new FrameSequenceFinding(
                FrameSequenceDefect.CountMismatch,
                -1,
                $"{rawFrames.ToString(CultureInfo.InvariantCulture)} raw frame(s) against {times.Count.ToString(CultureInfo.InvariantCulture)} logged timestamp(s)"));
        }

        var interval = nominalFrameIntervalSeconds ?? MedianInterval(times);

        for (var index = 1; index < times.Count; index++)
        {
            var delta = times[index] - times[index - 1];
            if (delta < 0)
            {
                findings.Add(new FrameSequenceFinding(
                    FrameSequenceDefect.Backwards,
                    index,
                    $"pts went from {times[index - 1].ToString("F6", CultureInfo.InvariantCulture)} to {times[index].ToString("F6", CultureInfo.InvariantCulture)}"));
                continue;
            }

            if (delta == 0)
            {
                findings.Add(new FrameSequenceFinding(
                    FrameSequenceDefect.Duplicate,
                    index,
                    $"pts {times[index].ToString("F6", CultureInfo.InvariantCulture)} repeats"));
                continue;
            }

            if (interval > 0 && delta > interval * gapTolerance)
            {
                var missing = (int)Math.Round((delta / interval) - 1);
                findings.Add(new FrameSequenceFinding(
                    FrameSequenceDefect.Gap,
                    index,
                    $"{(delta * 1000).ToString("F1", CultureInfo.InvariantCulture)} ms since the previous frame, about {missing.ToString(CultureInfo.InvariantCulture)} frame(s) missing"));
            }
        }

        return new FrameSequence(
            new ReadOnlyCollection<double>(times),
            rawFrames,
            new ReadOnlyCollection<FrameSequenceFinding>(findings));
    }

    private static double MedianInterval(List<double> times)
    {
        if (times.Count < 2)
        {
            return 0;
        }

        var deltas = new List<double>(times.Count - 1);
        for (var index = 1; index < times.Count; index++)
        {
            var delta = times[index] - times[index - 1];
            if (delta > 0)
            {
                deltas.Add(delta);
            }
        }

        if (deltas.Count == 0)
        {
            return 0;
        }

        // Median rather than mean: one long gap would drag a mean far enough to hide
        // every other gap behind it, which is the failure this measurement exists to see.
        deltas.Sort();
        return deltas[deltas.Count / 2];
    }
}
