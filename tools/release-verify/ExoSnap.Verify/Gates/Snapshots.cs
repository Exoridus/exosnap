using System.Globalization;
using System.Text.Json;

namespace ExoSnap.Verify.Gates;

/// <summary>Whether a declared field path exists in a document.</summary>
public enum FieldPresence
{
    /// <summary>The path resolves all the way down.</summary>
    Present,

    /// <summary>A segment on the way is not emitted at all.</summary>
    Missing,

    /// <summary>
    /// A collection on the path is there and empty, so its name is proven and its
    /// element shape is not.
    /// </summary>
    Empty,
}

/// <summary>Where a field path stopped, and why.</summary>
/// <param name="Presence">Whether it resolved.</param>
/// <param name="At">The segment the walk stopped at, or the whole path when it resolved.</param>
/// <param name="Value">The value found, or the default element.</param>
public sealed record FieldResolution(FieldPresence Presence, string At, JsonElement Value);

/// <summary>
/// Reads dotted field paths out of control-channel snapshots.
/// </summary>
/// <remarks>
/// Whole measurement groups are legitimately absent: a pipeline snapshot with
/// <c>valid: false</c> carries only its summary, because emitting the groups would
/// hand a reader a complete, entirely zero pipeline that reads as a healthy recording
/// rather than an idle process. So "not measured" has to be answerable without an
/// exception, and every reader here returns an absent value rather than a default a
/// caller could mistake for a reading.
/// </remarks>
public static class Snapshots
{
    /// <summary>The value at a dotted path, or the default element when it is not emitted.</summary>
    public static JsonElement Value(JsonElement root, string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var current = root;
        foreach (var segment in path.Split('.'))
        {
            if (current.ValueKind != JsonValueKind.Object || !current.TryGetProperty(segment, out var next))
            {
                return default;
            }

            current = next;
        }

        return current;
    }

    /// <summary>The string at a dotted path, or an empty string.</summary>
    public static string Text(JsonElement root, string path)
    {
        var value = Value(root, path);
        return value.ValueKind switch
        {
            JsonValueKind.String => value.GetString() ?? string.Empty,
            JsonValueKind.Number => value.GetRawText(),
            JsonValueKind.True => "true",
            JsonValueKind.False => "false",
            _ => string.Empty,
        };
    }

    /// <summary>The number at a dotted path, or null when it is not emitted or not a number.</summary>
    public static double? Number(JsonElement root, string path)
    {
        var value = Value(root, path);
        return value.ValueKind switch
        {
            JsonValueKind.Number when value.TryGetDouble(out var number) => number,
            JsonValueKind.String when double.TryParse(
                value.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out var number) => number,
            _ => null,
        };
    }

    /// <summary>Whether the path carries the boolean true. An absent path is not true.</summary>
    public static bool IsTrue(JsonElement root, string path) => Value(root, path).ValueKind == JsonValueKind.True;

    /// <summary>Whether the path carries the boolean false. An absent path is not false.</summary>
    public static bool IsFalse(JsonElement root, string path) => Value(root, path).ValueKind == JsonValueKind.False;

    /// <summary>The elements of an array at a dotted path, or an empty list.</summary>
    public static IReadOnlyList<JsonElement> Items(JsonElement root, string path)
    {
        var value = Value(root, path);
        return value.ValueKind == JsonValueKind.Array ? [.. value.EnumerateArray()] : [];
    }

    /// <summary>
    /// Walks a field path and says whether it exists, treating a trailing <c>[]</c> as
    /// "and then its first element".
    /// </summary>
    /// <remarks>
    /// <see cref="FieldPresence.Empty"/> is its own answer rather than a failure. A
    /// collection with no elements right now proves its own name is right and proves
    /// nothing about the shape of its elements, and collapsing that into either
    /// verdict would be a false statement in one direction or the other.
    /// </remarks>
    public static FieldResolution Resolve(JsonElement root, string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var current = root;

        foreach (var segment in path.Split('.'))
        {
            var indexed = segment.EndsWith("[]", StringComparison.Ordinal);
            var name = indexed ? segment[..^2] : segment;

            if (current.ValueKind != JsonValueKind.Object || !current.TryGetProperty(name, out var next))
            {
                return new FieldResolution(FieldPresence.Missing, name, default);
            }

            current = next;
            if (!indexed)
            {
                continue;
            }

            if (current.ValueKind != JsonValueKind.Array)
            {
                return new FieldResolution(FieldPresence.Missing, name, default);
            }

            var items = current.EnumerateArray().ToList();
            if (items.Count == 0)
            {
                return new FieldResolution(FieldPresence.Empty, name, default);
            }

            current = items[0];
        }

        return new FieldResolution(FieldPresence.Present, path, current);
    }

    /// <summary>
    /// The notification hub entries out of a <c>notifications.snapshot</c>.
    /// </summary>
    /// <remarks>
    /// The array is <c>entries</c>, and each entry carries sequence, title, body,
    /// severity, unread and actions. There is no id and no detail: the
    /// manager-assigned <c>sequence</c> is the hub's own stable identity and the only
    /// thing a client may address an entry by.
    /// </remarks>
    public static IReadOnlyList<JsonElement> NotificationEntries(JsonElement snapshot) => Items(snapshot, "entries");
}
