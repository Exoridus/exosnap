using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Json;

/// <summary>
/// Source-generated serialization contracts for every document the harness
/// writes.
/// </summary>
/// <remarks>
/// Source generation rather than reflection so a document shape that stopped
/// serializing is a build error rather than a runtime surprise on the release
/// machine. Numbers and dates are written invariantly regardless of the operator's
/// locale, which is why nothing here ever calls a culture-sensitive formatter.
/// </remarks>
[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    UseStringEnumConverter = true,
    WriteIndented = true,
    DefaultIgnoreCondition = JsonIgnoreCondition.Never)]
[JsonSerializable(typeof(QualificationRecord))]
[JsonSerializable(typeof(MachineCapabilityDocument))]
[JsonSerializable(typeof(ScenarioDescriptorDocument))]
[JsonSerializable(typeof(RunState))]
[JsonSerializable(typeof(CampaignDocument))]
[JsonSerializable(typeof(EnvironmentRestoreDocument))]
[JsonSerializable(typeof(ScenarioResult))]
[JsonSerializable(typeof(IReadOnlyList<ScenarioDescriptor>))]
public sealed partial class VerifyJsonContext : JsonSerializerContext;

/// <summary>The machine capability document, as written to disk.</summary>
/// <param name="SchemaVersion">Version of this document's shape.</param>
/// <param name="TimestampUtc">When the probes ran.</param>
/// <param name="Machine">Which machine was probed.</param>
/// <param name="Capabilities">Key to value; a value the probes could not determine is "unknown".</param>
/// <param name="Sources">Key to the mechanism that produced the value.</param>
public sealed record MachineCapabilityDocument(
    string SchemaVersion,
    DateTimeOffset TimestampUtc,
    MachineFingerprint Machine,
    IReadOnlyDictionary<string, string> Capabilities,
    IReadOnlyDictionary<string, string> Sources)
{
    /// <summary>The schema version this build of the harness writes.</summary>
    public const string CurrentSchemaVersion = "1";
}

/// <summary>The scenario catalog, as written to disk.</summary>
/// <param name="SchemaVersion">Version of this document's shape.</param>
/// <param name="CatalogVersion">Digest over the descriptors, used for staleness detection.</param>
/// <param name="Scenarios">Every descriptor, in catalog order.</param>
public sealed record ScenarioDescriptorDocument(
    string SchemaVersion,
    string CatalogVersion,
    IReadOnlyList<ScenarioDescriptor> Scenarios)
{
    /// <summary>The schema version this build of the harness writes.</summary>
    public const string CurrentSchemaVersion = "1";
}

/// <summary>Writes and reads the harness documents.</summary>
public static class VerifyJson
{
    /// <summary>Serializer options every document in the harness is written with.</summary>
    public static JsonSerializerOptions Options { get; } = new(VerifyJsonContext.Default.Options);

    /// <summary>Serializes a value using the source-generated contract for its type.</summary>
    public static string Serialize<T>(T value, JsonTypeInfo<T> typeInfo) =>
        JsonSerializer.Serialize(value, typeInfo);

    /// <summary>Writes a document to disk, creating the directory if needed.</summary>
    public static void WriteFile<T>(string path, T value, JsonTypeInfo<T> typeInfo)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var full = Path.GetFullPath(path);
        var directory = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        File.WriteAllText(full, JsonSerializer.Serialize(value, typeInfo) + Environment.NewLine);
    }

    /// <summary>Reads a document from disk, or returns null when it does not exist.</summary>
    public static T? ReadFile<T>(string path, JsonTypeInfo<T> typeInfo)
        where T : class
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var full = Path.GetFullPath(path);
        if (!File.Exists(full))
        {
            return null;
        }

        return JsonSerializer.Deserialize(File.ReadAllText(full), typeInfo);
    }
}
