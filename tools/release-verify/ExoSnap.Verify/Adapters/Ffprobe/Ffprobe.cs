using System.Collections.ObjectModel;
using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Adapters.Ffprobe;

/// <summary>
/// The real ffprobe, driven through the containing process runner.
/// </summary>
/// <remarks>
/// Two invocations, because they answer different questions. <c>-show_streams</c>
/// plus <c>-show_format</c> is one JSON document about the container;
/// <c>-show_entries packet=pts_time</c> is a per-stream packet dump, which is the
/// only way to learn how much of a container a track actually covers.
///
/// Every invocation ends the argument list with <c>--</c>, so a path that begins
/// with a hyphen is a path rather than an option.
/// </remarks>
public sealed class Ffprobe : IFfprobe
{
    /// <summary>The name this adapter reports itself under in a contract violation.</summary>
    public const string ToolName = "ffprobe";

    private static readonly TimeSpan InspectTimeout = TimeSpan.FromSeconds(60);

    // A packet dump of a 30-minute soak is hundreds of thousands of lines, and the
    // process is doing real demuxing work for every one of them.
    private static readonly TimeSpan PacketTimeout = TimeSpan.FromMinutes(10);

    private readonly ProcessRunner runner;
    private readonly string? executablePath;

    /// <summary>Creates an adapter for a resolved ffprobe, or for none at all.</summary>
    /// <param name="runner">The runner every child process goes through.</param>
    /// <param name="executablePath">Absolute path of ffprobe, or null when it is not on this machine.</param>
    public Ffprobe(ProcessRunner runner, string? executablePath)
    {
        ArgumentNullException.ThrowIfNull(runner);
        this.runner = runner;
        this.executablePath = executablePath;
    }

    /// <inheritdoc/>
    public bool Available => this.executablePath is not null;

    /// <inheritdoc/>
    public async Task<FfprobeResult> InspectAsync(string path, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var tool = this.Require();

        var result = await this.runner.RunAsync(
            new ProcessRunRequest(
                tool,
                "-v", "error",
                "-print_format", "json",
                "-show_format",
                "-show_streams",
                "--", path)
            {
                Timeout = InspectTimeout,
            },
            cancellationToken).ConfigureAwait(false);

        using var document = ToolContract.ParseJson(result, ToolName);
        return Parse(document.RootElement, result.StandardOutput);
    }

    /// <inheritdoc/>
    public async Task<ReadOnlyCollection<double>> PacketSpanSecondsAsync(
        string path,
        IReadOnlyList<int> streamIndexes,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(streamIndexes);
        var tool = this.Require();

        var spans = new List<double>(streamIndexes.Count);
        foreach (var index in streamIndexes)
        {
            var result = await this.runner.RunAsync(
                new ProcessRunRequest(
                    tool,
                    "-v", "error",
                    "-select_streams", index.ToString(CultureInfo.InvariantCulture),
                    "-show_entries", "packet=pts_time",
                    "-of", "csv=p=0",
                    "--", path)
                {
                    Timeout = PacketTimeout,
                },
                cancellationToken).ConfigureAwait(false);

            if (result.TimedOut)
            {
                throw ToolContractException.ForTool(
                    ToolName,
                    $"the packet dump of stream {index.ToString(CultureInfo.InvariantCulture)} did not finish within its deadline");
            }

            spans.Add(SpanOf(result.StandardOutput, index));
        }

        return new ReadOnlyCollection<double>(spans);
    }

    /// <summary>
    /// Reads an ffprobe document that was captured earlier, so a contract test and
    /// the real adapter share one parser.
    /// </summary>
    /// <exception cref="ToolContractException">The text is not the document ffprobe promises.</exception>
    public static FfprobeResult ParseDocument(string json)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(json);
        try
        {
            using var document = JsonDocument.Parse(json);
            return Parse(document.RootElement, json);
        }
        catch (JsonException exception)
        {
            throw ToolContractException.ForTool(ToolName, "the document is not valid JSON", exception);
        }
    }

    private static FfprobeResult Parse(JsonElement root, string raw)
    {
        var streams = new List<FfprobeTrack>();
        if (root.TryGetProperty("streams", out var array) && array.ValueKind == JsonValueKind.Array)
        {
            foreach (var stream in array.EnumerateArray())
            {
                streams.Add(new FfprobeTrack(
                    Integer(stream, "index") ?? -1,
                    Text(stream, "codec_type"),
                    Text(stream, "codec_name"),
                    Integer(stream, "sample_rate"),
                    Integer(stream, "channels")));
            }
        }

        var format = root.TryGetProperty("format", out var formatElement) &&
                     formatElement.ValueKind == JsonValueKind.Object
            ? new FfprobeFormat(Text(formatElement, "format_name"), Number(formatElement, "duration"))
            : new FfprobeFormat(string.Empty, null);

        return new FfprobeResult(new ReadOnlyCollection<FfprobeTrack>(streams), format, raw);
    }

    // ffprobe writes numbers as JSON strings in the stream and format objects, so a
    // reader that only accepted JSON numbers would report every field as absent.
    private static int? Integer(JsonElement element, string name)
    {
        if (!element.TryGetProperty(name, out var value))
        {
            return null;
        }

        return value.ValueKind switch
        {
            JsonValueKind.Number when value.TryGetInt32(out var number) => number,
            JsonValueKind.String when int.TryParse(
                value.GetString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var number) => number,
            _ => null,
        };
    }

    private static double? Number(JsonElement element, string name)
    {
        if (!element.TryGetProperty(name, out var value))
        {
            return null;
        }

        return value.ValueKind switch
        {
            JsonValueKind.Number when value.TryGetDouble(out var number) => number,
            JsonValueKind.String when double.TryParse(
                value.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out var number) => number,
            _ => null,
        };
    }

    private static string Text(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;

    /// <summary>
    /// The span a CSV packet dump describes. Parsed invariantly and strictly: a
    /// timestamp that is not a number means the dump was not understood, and a span
    /// derived from a misread number is worse than no span at all.
    /// </summary>
    internal static double SpanOf(string csv, int streamIndex)
    {
        double? first = null;
        double? last = null;

        foreach (var line in csv.Split('\n'))
        {
            var text = line.Trim().TrimEnd(',');
            if (text.Length == 0)
            {
                continue;
            }

            // A packet whose presentation timestamp the container does not carry is
            // written as "N/A". It is not a broken dump, and it contributes no bound.
            if (string.Equals(text, "N/A", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            if (!double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var timestamp))
            {
                throw ToolContractException.ForTool(
                    ToolName,
                    $"stream {streamIndex.ToString(CultureInfo.InvariantCulture)} reported the packet timestamp '{text}', which is not a number");
            }

            first ??= timestamp;
            last = timestamp;
        }

        return first is null || last is null ? 0.0 : last.Value - first.Value;
    }

    private string Require() =>
        this.executablePath ??
        throw ToolContractException.ForTool(ToolName, "ffprobe is not resolvable on this machine");
}
