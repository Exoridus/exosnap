using System.Text.Json;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// Writes the files a verdict rests on, and binds each to its content.
/// </summary>
/// <remarks>
/// Every gate saves what it read before it decides, not after: a verdict that cites a
/// document nobody kept is an assertion, and the point of the evidence model is that
/// a reader can check the assertion later.
/// </remarks>
public static class GateEvidence
{
    private static readonly JsonWriterOptions Indented = new() { Indented = true };

    /// <summary>Saves a control-channel document and captures it as evidence.</summary>
    public static Evidence SaveJson(ScenarioContext context, string name, JsonElement value)
    {
        ArgumentNullException.ThrowIfNull(context);
        ArgumentException.ThrowIfNullOrWhiteSpace(name);

        var path = Path.Combine(context.EvidenceDirectory, name);
        Directory.CreateDirectory(context.EvidenceDirectory);

        using (var stream = File.Create(path))
        using (var writer = new Utf8JsonWriter(stream, Indented))
        {
            if (value.ValueKind == JsonValueKind.Undefined)
            {
                writer.WriteNullValue();
            }
            else
            {
                value.WriteTo(writer);
            }
        }

        return Evidence.ForFile(Path.GetFileNameWithoutExtension(name), path);
    }

    /// <summary>Saves text a tool produced and captures it as evidence.</summary>
    public static Evidence SaveText(ScenarioContext context, string name, string content)
    {
        ArgumentNullException.ThrowIfNull(context);
        ArgumentException.ThrowIfNullOrWhiteSpace(name);

        var path = Path.Combine(context.EvidenceDirectory, name);
        Directory.CreateDirectory(context.EvidenceDirectory);
        File.WriteAllText(path, content ?? string.Empty);
        return Evidence.ForFile(Path.GetFileNameWithoutExtension(name), path);
    }
}
