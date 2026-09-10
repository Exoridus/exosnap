using System.Text.Json;
using System.Text.Json.Serialization;

namespace ExoSnap.Verify.Windows;

/// <summary>What the elevated worker concluded, in the vocabulary the parent maps to a verdict.</summary>
public enum ElevatedWorkerOutcome
{
    /// <summary>The worker could not classify a result; treat as an infrastructure error.</summary>
    Unknown,

    /// <summary>The elevated observation held: what the scenario requires was true.</summary>
    Pass,

    /// <summary>The elevated observation contradicted what the scenario requires. ExoSnap is wrong.</summary>
    Fail,

    /// <summary>The worker itself could not carry the observation out. Never a product verdict.</summary>
    InfrastructureError,

    /// <summary>The observation needs a condition this machine did not present (nothing was drawing).</summary>
    Deferred,
}

/// <summary>
/// The document the elevated worker writes and the parent reads. The whole of the
/// IPC boundary: no shared memory, no pipe the parent listens on, no console.
/// </summary>
/// <param name="SchemaVersion">Shape of this document.</param>
/// <param name="TaskId">The scenario the worker carried out.</param>
/// <param name="Outcome">The worker's verdict.</param>
/// <param name="Message">One sentence, in the scenario's own terms.</param>
/// <param name="Elevated">Whether the worker process actually held an elevated token.</param>
/// <param name="PresentCount">Presents the elevated diagnostics decoded, or 0.</param>
/// <param name="PresentMode">The classified presentation path, or an empty string.</param>
/// <param name="TearingAllowed">Whether tearing was allowed, or null when not observed.</param>
/// <param name="OracleNote">What the independent PresentMon read said, or an empty string.</param>
/// <param name="FinishedUtc">When the worker finished.</param>
public sealed record ElevatedWorkerResult(
    string SchemaVersion,
    string TaskId,
    ElevatedWorkerOutcome Outcome,
    string Message,
    bool Elevated,
    long PresentCount,
    string PresentMode,
    bool? TearingAllowed,
    string OracleNote,
    DateTimeOffset FinishedUtc)
{
    /// <summary>The schema version this build writes.</summary>
    public const string CurrentSchemaVersion = "1";

    /// <summary>The file name the worker result is conventionally written under.</summary>
    public const string FileName = "elevated-worker-result.json";

    /// <summary>Builds a result for one task, stamped now.</summary>
    public static ElevatedWorkerResult For(
        string taskId,
        ElevatedWorkerOutcome outcome,
        string message,
        bool elevated = false,
        long presentCount = 0,
        string presentMode = "",
        bool? tearingAllowed = null,
        string oracleNote = "") =>
        new(
            CurrentSchemaVersion,
            taskId,
            outcome,
            message,
            elevated,
            presentCount,
            presentMode,
            tearingAllowed,
            oracleNote,
            DateTimeOffset.UtcNow);

    /// <summary>Writes the result to <paramref name="path"/>, creating its directory.</summary>
    public void Write(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var directory = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        File.WriteAllText(path, JsonSerializer.Serialize(this, ElevatedWorkerJson.Default.ElevatedWorkerResult));
    }

    /// <summary>Reads a result the worker wrote, or null when the file is absent or unreadable.</summary>
    public static ElevatedWorkerResult? Read(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        if (!File.Exists(path))
        {
            return null;
        }

        try
        {
            var text = File.ReadAllText(path);
            return string.IsNullOrWhiteSpace(text)
                ? null
                : JsonSerializer.Deserialize(text, ElevatedWorkerJson.Default.ElevatedWorkerResult);
        }
        catch (JsonException)
        {
            return null;
        }
        catch (IOException)
        {
            return null;
        }
    }
}

/// <summary>Source-generated contract for the worker result document.</summary>
[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    UseStringEnumConverter = true,
    WriteIndented = true,
    DefaultIgnoreCondition = JsonIgnoreCondition.Never)]
[JsonSerializable(typeof(ElevatedWorkerResult))]
public sealed partial class ElevatedWorkerJson : JsonSerializerContext;
