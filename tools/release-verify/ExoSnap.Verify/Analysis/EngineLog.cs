using System.Collections.ObjectModel;
using System.Globalization;
using System.Text.Json;

namespace ExoSnap.Verify.Analysis;

/// <summary>Which reading the recorder opened its video timeline with.</summary>
public enum VideoEpochSource
{
    /// <summary>
    /// The counter reading taken when the capture path observed the first frame. It
    /// trails the frame's actual present by up to one acquire interval.
    /// </summary>
    CaptureObserved,

    /// <summary>The first frame's own present timestamp, an instant on the counter axis.</summary>
    FrameTimestamp,

    /// <summary>
    /// The session start, used because the first frame carried a present timestamp from
    /// before recording began. A bound below the first frame, not the instant of one.
    /// </summary>
    SessionStartFloor,
}

/// <summary>
/// The instant a recording's presentation timestamp 0 sits on, as the recorder published it.
/// </summary>
/// <param name="EpochQpc100ns">The epoch in 100 ns units on the performance-counter axis.</param>
/// <param name="QpcFrequencyHz">The counter frequency the recorder read, for cross-checking a conversion.</param>
/// <param name="Source">Which reading opened the timeline, which is what its precision depends on.</param>
/// <param name="SessionId">The launch this record belongs to, when the log carries one.</param>
public sealed record VideoEpochRecord(long EpochQpc100ns, long QpcFrequencyHz, VideoEpochSource Source, string? SessionId)
{
    /// <summary>100 ns units, so a conversion through the counter frequency never enters the comparison.</summary>
    public const long TicksPerSecond = 10_000_000;

    /// <summary>
    /// Turns this epoch into an offset estimate against a stimulus that logged its own
    /// counter reading.
    /// </summary>
    /// <param name="stimulusEpochQpc100ns">The instant the stimulus calls its own t = 0, on the same axis.</param>
    /// <param name="acquireIntervalSeconds">
    /// How long the capture path may take to observe a frame that has presented. Only read for a
    /// <see cref="VideoEpochSource.CaptureObserved"/> epoch, whose reading is corrected by half of
    /// it and carries the other half as its uncertainty.
    /// </param>
    /// <returns>
    /// The estimate, or null when the epoch cannot carry one -- a floored epoch is a bound below
    /// the first frame, and an offset measured against it would read as a measurement.
    /// </returns>
    public TimelineAnchorEstimate? ToAnchorEstimate(long stimulusEpochQpc100ns, double acquireIntervalSeconds)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(acquireIntervalSeconds);

        if (this.Source == VideoEpochSource.SessionStartFloor)
        {
            return null;
        }

        if (this.Source == VideoEpochSource.FrameTimestamp)
        {
            // The frame's timestamp is the instant itself; only the 100 ns quantisation
            // separates the published value from it.
            return TimelineAnchor.FromPerformanceCounter(
                stimulusEpochQpc100ns, this.EpochQpc100ns, TicksPerSecond, 0.5 / TicksPerSecond);
        }

        // The observation is later than the present by somewhere in [0, interval], so the
        // midpoint is the estimate and half the interval is what it can be wrong by. Taking
        // the observation itself would bias every offset late by half an interval, which is
        // the size of an in-band marker's whole uncertainty.
        var halfInterval = acquireIntervalSeconds / 2.0;
        var correctedTicks = this.EpochQpc100ns - (long)Math.Round(halfInterval * TicksPerSecond);
        return TimelineAnchor.FromPerformanceCounter(
            stimulusEpochQpc100ns, correctedTicks, TicksPerSecond, Math.Max(halfInterval, 1.0 / TicksPerSecond));
    }
}

/// <summary>What one read of an engine log found, and what it could not read.</summary>
/// <param name="VideoEpochs">Every video-epoch record in the file, in the order they were written.</param>
/// <param name="Rejections">Records that announced an epoch but could not be read, with the reason.</param>
/// <param name="UnreadableLines">Lines that are not JSON objects at all.</param>
public sealed record EngineLogRead(
    ReadOnlyCollection<VideoEpochRecord> VideoEpochs,
    ReadOnlyCollection<string> Rejections,
    int UnreadableLines);

/// <summary>
/// The recorder's structured log, read for the facts an external measurement needs.
/// </summary>
/// <remarks>
/// <para>
/// The engine appends to one JSONL file across launches and rotates it by size, so a file
/// routinely holds several sessions and a run being tailed ends in a partial line. Neither is
/// an error, and neither may be silently resolved: taking the last epoch record from a file
/// that holds four sessions reads a previous run's timeline onto this run's frames. Callers
/// select by session id, or refuse.
/// </para>
/// <para>
/// Nothing here throws on content. A malformed record is a rejection the caller sees, because a
/// measurement that aborts on one unreadable line reports nothing about the recording.
/// </para>
/// </remarks>
public static class EngineLog
{
    private const string VideoEpochMessage = "video epoch established";

    /// <summary>Reads every video-epoch record from an engine JSONL file.</summary>
    /// <param name="path">Path to the engine log. A missing file is an empty read, not a throw.</param>
    public static EngineLogRead ReadVideoEpochs(string path)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);

        var epochs = new List<VideoEpochRecord>();
        var rejections = new List<string>();
        var unreadable = 0;

        if (!File.Exists(path))
        {
            return new EngineLogRead(epochs.AsReadOnly(), rejections.AsReadOnly(), unreadable);
        }

        var lineNumber = 0;
        foreach (var line in File.ReadLines(path))
        {
            lineNumber++;
            if (string.IsNullOrWhiteSpace(line))
            {
                continue;
            }

            JsonElement root;
            try
            {
                using var document = JsonDocument.Parse(line);
                root = document.RootElement.Clone();
            }
            catch (JsonException)
            {
                unreadable++;
                continue;
            }

            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("message", out var message) ||
                message.ValueKind != JsonValueKind.String ||
                message.GetString() != VideoEpochMessage)
            {
                continue;
            }

            if (!root.TryGetProperty("fields", out var fields) || fields.ValueKind != JsonValueKind.Object)
            {
                rejections.Add($"line {lineNumber}: the epoch record carries no fields object");
                continue;
            }

            if (!TryReadLong(fields, "video_epoch_qpc_100ns", out var epoch))
            {
                rejections.Add($"line {lineNumber}: video_epoch_qpc_100ns is missing or not an integer");
                continue;
            }

            if (!TryReadLong(fields, "qpc_frequency_hz", out var frequency))
            {
                rejections.Add($"line {lineNumber}: qpc_frequency_hz is missing or not an integer");
                continue;
            }

            var sourceText = ReadString(fields, "epoch_source");
            VideoEpochSource? source = sourceText switch
            {
                "frame_timestamp" => VideoEpochSource.FrameTimestamp,
                "session_start_floor" => VideoEpochSource.SessionStartFloor,
                "capture_observed" => VideoEpochSource.CaptureObserved,
                _ => null,
            };

            if (source is null)
            {
                // Defaulting an unknown source to the precise one would read a future
                // recorder's coarser epoch as exact.
                var named = sourceText ?? "(absent)";
                rejections.Add($"line {lineNumber}: epoch_source '{named}' is not one this reader knows");
                continue;
            }

            epochs.Add(new VideoEpochRecord(epoch, frequency, source.Value, ReadString(fields, "session")));
        }

        return new EngineLogRead(epochs.AsReadOnly(), rejections.AsReadOnly(), unreadable);
    }

    private static string? ReadString(JsonElement fields, string name) =>
        fields.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;

    // Every engine log field is written as a string, including the numeric ones.
    private static bool TryReadLong(JsonElement fields, string name, out long parsed)
    {
        parsed = 0;
        if (!fields.TryGetProperty(name, out var value))
        {
            return false;
        }

        return value.ValueKind switch
        {
            JsonValueKind.String => long.TryParse(
                value.GetString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out parsed),
            JsonValueKind.Number => value.TryGetInt64(out parsed),
            _ => false,
        };
    }
}
