using System.Collections.ObjectModel;

namespace ExoSnap.Verify.Adapters.Ffprobe;

/// <summary>One elementary stream of a container.</summary>
/// <param name="Index">Stream index within the container.</param>
/// <param name="CodecType">"video", "audio", "subtitle", or whatever ffprobe reported.</param>
/// <param name="CodecName">Short codec name, or an empty string when ffprobe named none.</param>
/// <param name="SampleRateHz">Audio sample rate, or null for a stream that has none.</param>
/// <param name="Channels">Audio channel count, or null for a stream that has none.</param>
public sealed record FfprobeTrack(
    int Index,
    string CodecType,
    string CodecName,
    int? SampleRateHz,
    int? Channels)
{
    /// <summary>Whether this stream carries video.</summary>
    public bool IsVideo => string.Equals(this.CodecType, "video", StringComparison.OrdinalIgnoreCase);

    /// <summary>Whether this stream carries audio.</summary>
    public bool IsAudio => string.Equals(this.CodecType, "audio", StringComparison.OrdinalIgnoreCase);
}

/// <summary>Container-level facts.</summary>
/// <param name="FormatName">Container short name, or an empty string.</param>
/// <param name="DurationSeconds">
/// Container duration. Null when the container carries no duration tag at all, which
/// a live-muxed MKV legitimately does not.
/// </param>
public sealed record FfprobeFormat(string FormatName, double? DurationSeconds);

/// <summary>Everything one ffprobe inspection reported about a file.</summary>
/// <param name="Streams">Every stream, in container order.</param>
/// <param name="Format">The container-level facts.</param>
/// <param name="RawJson">The exact document ffprobe wrote, kept so a verdict can cite it.</param>
public sealed record FfprobeResult(
    ReadOnlyCollection<FfprobeTrack> Streams,
    FfprobeFormat Format,
    string RawJson)
{
    /// <summary>The video streams, in container order.</summary>
    public ReadOnlyCollection<FfprobeTrack> VideoStreams => new([.. this.Streams.Where(stream => stream.IsVideo)]);

    /// <summary>The audio streams, in container order.</summary>
    public ReadOnlyCollection<FfprobeTrack> AudioStreams => new([.. this.Streams.Where(stream => stream.IsAudio)]);
}

/// <summary>
/// The independent oracle for a recording ExoSnap says it produced.
/// </summary>
/// <remarks>
/// An interface rather than a class so a scenario's logic can be exercised against a
/// probe that reports a defect on demand. ExoSnap's own report confirming ExoSnap's
/// own recording proves only that it is self-consistent, which is why every capture
/// gate ends here.
/// </remarks>
public interface IFfprobe
{
    /// <summary>Whether ffprobe is resolvable on this machine at all.</summary>
    bool Available { get; }

    /// <summary>Reads the streams and container facts of one media file.</summary>
    /// <exception cref="Processes.ToolContractException">
    /// ffprobe did not start, timed out, or wrote something that is not the JSON its
    /// contract promises. Never a statement about the product.
    /// </exception>
    Task<FfprobeResult> InspectAsync(string path, CancellationToken cancellationToken);

    /// <summary>
    /// First-to-last packet span, in seconds, of each named stream, in the order the
    /// indexes were given.
    /// </summary>
    /// <remarks>
    /// Measured from the packets rather than from a container duration tag: a
    /// live-muxed MKV carries no per-stream DURATION, so a caller reading the tag
    /// would see every stream as full length whatever it actually contains. A stream
    /// with no packets at all spans zero seconds.
    /// </remarks>
    /// <exception cref="Processes.ToolContractException">
    /// ffprobe did not start, timed out, or wrote packet timestamps that are not
    /// numbers.
    /// </exception>
    Task<ReadOnlyCollection<double>> PacketSpanSecondsAsync(
        string path,
        IReadOnlyList<int> streamIndexes,
        CancellationToken cancellationToken);
}
