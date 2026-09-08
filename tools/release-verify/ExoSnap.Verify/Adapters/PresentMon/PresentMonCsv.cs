using System.Collections.ObjectModel;
using System.Globalization;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Adapters.PresentMon;

/// <summary>
/// The PresentMon 2.x CSV column contract, and the parser that reads it.
/// </summary>
/// <remarks>
/// Column names are matched by header rather than by position. PresentMon 2.x
/// changes which optional metrics it emits with its command line - display tracking,
/// input latency, GPU metrics - so a positional reader is correct for exactly one
/// invocation and silently wrong for every other. Two columns are required, because
/// no verdict can be reached without them: <c>ProcessID</c> and <c>PresentMode</c>.
/// Everything else is read when present and reported as null when it is not.
///
/// The mode strings are PresentMon's own, and the classification into the product's
/// four values follows the same rule the product's ETW consumer applies: a legacy
/// flip or a legacy copy to the front buffer is exclusive fullscreen, a hardware or
/// hardware-composed independent flip is independent flip, and every composed variant
/// is composed.
/// </remarks>
public static class PresentMonCsv
{
    /// <summary>The name this parser reports itself under in a contract violation.</summary>
    public const string ToolName = "PresentMon";

    /// <summary>The process id column, without which no present can be attributed.</summary>
    public const string ProcessIdColumn = "ProcessID";

    /// <summary>The presentation path column, without which there is no verdict.</summary>
    public const string PresentModeColumn = "PresentMode";

    private static readonly Dictionary<string, PresentMode> Modes = new(StringComparer.OrdinalIgnoreCase)
    {
        ["Hardware: Legacy Flip"] = PresentMode.ExclusiveFullscreen,
        ["Hardware: Legacy Copy to front buffer"] = PresentMode.ExclusiveFullscreen,
        ["Hardware: Independent Flip"] = PresentMode.IndependentFlip,
        ["Hardware Composed: Independent Flip"] = PresentMode.IndependentFlip,
        ["Composed: Flip"] = PresentMode.Composed,
        ["Composed: Copy with GPU GDI"] = PresentMode.Composed,
        ["Composed: Copy with CPU GDI"] = PresentMode.Composed,
        ["Composed: Composition Atlas"] = PresentMode.Composed,
        ["Unknown"] = PresentMode.Unknown,
    };

    /// <summary>Classifies one PresentMon mode string.</summary>
    /// <remarks>
    /// A mode nobody recognises is <see cref="PresentMode.Unknown"/>, never a guess.
    /// An agreement check that fell back to the nearest match would report agreement
    /// about a mode neither observer named.
    /// </remarks>
    public static PresentMode Classify(string rawMode) =>
        Modes.TryGetValue((rawMode ?? string.Empty).Trim(), out var mode) ? mode : PresentMode.Unknown;

    /// <summary>The product's name for a mode, as its control channel reports it.</summary>
    public static string ProductName(PresentMode mode) => mode switch
    {
        PresentMode.Composed => "composed",
        PresentMode.IndependentFlip => "independentFlip",
        PresentMode.ExclusiveFullscreen => "exclusiveFullscreen",
        _ => "unknown",
    };

    /// <summary>The mode a product report names, or <see cref="PresentMode.Unknown"/>.</summary>
    public static PresentMode FromProductName(string name) => (name ?? string.Empty).Trim() switch
    {
        "composed" => PresentMode.Composed,
        "independentFlip" => PresentMode.IndependentFlip,
        "exclusiveFullscreen" => PresentMode.ExclusiveFullscreen,
        _ => PresentMode.Unknown,
    };

    /// <summary>Reads a capture from the text PresentMon wrote.</summary>
    /// <exception cref="ToolContractException">
    /// The text carries no header, or no header row with the required columns.
    /// </exception>
    public static PresentMonCapture Parse(string csv)
    {
        ArgumentNullException.ThrowIfNull(csv);

        var lines = csv.Split('\n')
            .Select(line => line.TrimEnd('\r'))
            .Where(line => line.Trim().Length > 0)
            .ToList();

        if (lines.Count == 0)
        {
            throw ToolContractException.ForTool(ToolName, "the capture is empty; it carries not even a header");
        }

        var header = Split(lines[0]);
        var index = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        for (var column = 0; column < header.Count; column++)
        {
            index.TryAdd(header[column].Trim(), column);
        }

        foreach (var required in new[] { ProcessIdColumn, PresentModeColumn })
        {
            if (!index.ContainsKey(required))
            {
                throw ToolContractException.ForTool(
                    ToolName,
                    $"the capture has no '{required}' column, so no present can be read from it");
            }
        }

        var records = new List<PresentRecord>(lines.Count - 1);
        for (var row = 1; row < lines.Count; row++)
        {
            var fields = Split(lines[row]);

            // A row shorter than the header is a truncated capture, which happens when
            // PresentMon is killed mid-write. Skipped rather than refused: the rows
            // before it are real observations, and the last partial line is not.
            if (fields.Count < header.Count)
            {
                continue;
            }

            var processId = Integer(fields, index, ProcessIdColumn);
            if (processId is null)
            {
                throw ToolContractException.ForTool(
                    ToolName,
                    $"row {row.ToString(CultureInfo.InvariantCulture)} reports the process id '{Field(fields, index, ProcessIdColumn)}', which is not a number");
            }

            var rawMode = Field(fields, index, PresentModeColumn);
            records.Add(new PresentRecord(
                Field(fields, index, "Application"),
                processId.Value,
                Field(fields, index, "SwapChainAddress"),
                Classify(rawMode),
                rawMode,
                Integer(fields, index, "SyncInterval"),
                Flag(fields, index, "AllowsTearing"),
                Number(fields, index, "CPUStartTime") ?? Number(fields, index, "TimeInSeconds"),
                Number(fields, index, "FrameTime") ?? Number(fields, index, "msBetweenPresents")));
        }

        return new PresentMonCapture(
            new ReadOnlyCollection<PresentRecord>(records),
            new ReadOnlyCollection<string>([.. header.Select(column => column.Trim())]));
    }

    // PresentMon quotes a field only when it contains a comma, which the application
    // name can. A full RFC 4180 reader is not needed, but ignoring quotes entirely
    // would split one application into two columns and shift every field after it.
    private static List<string> Split(string line)
    {
        var fields = new List<string>();
        var current = new System.Text.StringBuilder();
        var quoted = false;

        for (var position = 0; position < line.Length; position++)
        {
            var character = line[position];
            if (character == '"')
            {
                if (quoted && position + 1 < line.Length && line[position + 1] == '"')
                {
                    current.Append('"');
                    position++;
                    continue;
                }

                quoted = !quoted;
                continue;
            }

            if (character == ',' && !quoted)
            {
                fields.Add(current.ToString());
                current.Clear();
                continue;
            }

            current.Append(character);
        }

        fields.Add(current.ToString());
        return fields;
    }

    private static string Field(IReadOnlyList<string> fields, IReadOnlyDictionary<string, int> index, string name) =>
        index.TryGetValue(name, out var column) && column < fields.Count ? fields[column].Trim() : string.Empty;

    private static int? Integer(IReadOnlyList<string> fields, IReadOnlyDictionary<string, int> index, string name) =>
        int.TryParse(Field(fields, index, name), NumberStyles.Integer, CultureInfo.InvariantCulture, out var value)
            ? value
            : null;

    // Invariant on purpose, and strictly. PresentMon writes a period whatever the
    // operator's locale is, so a value with a comma is a corrupted capture rather
    // than a German decimal, and reading it as one would move a frame time by three
    // orders of magnitude.
    private static double? Number(IReadOnlyList<string> fields, IReadOnlyDictionary<string, int> index, string name) =>
        double.TryParse(Field(fields, index, name), NumberStyles.Float, CultureInfo.InvariantCulture, out var value)
            ? value
            : null;

    private static bool? Flag(IReadOnlyList<string> fields, IReadOnlyDictionary<string, int> index, string name)
    {
        var text = Field(fields, index, name);
        return text switch
        {
            "1" => true,
            "0" => false,
            _ when string.Equals(text, "true", StringComparison.OrdinalIgnoreCase) => true,
            _ when string.Equals(text, "false", StringComparison.OrdinalIgnoreCase) => false,
            _ => null,
        };
    }
}
