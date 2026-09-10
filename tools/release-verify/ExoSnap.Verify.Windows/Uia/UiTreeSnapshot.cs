using System.Collections.ObjectModel;

namespace ExoSnap.Verify.Windows.Uia;

/// <summary>One node of a process's UI Automation tree.</summary>
/// <param name="Name">The element's accessible name, as UI Automation reports it.</param>
/// <param name="ClassName">The native class name, or an empty string.</param>
/// <param name="ControlType">The control type's programmatic name (for example <c>ControlType.Window</c>).</param>
/// <param name="AutomationId">
/// The stable automation id, or an empty string. A gate matches on this rather than
/// on <see cref="Name"/> wherever it can: the id is set in QML and does not move
/// between locales, and the name is translated.
/// </param>
public sealed record UiElement(string Name, string ClassName, string ControlType, string AutomationId);

/// <summary>
/// The UI Automation tree of one process, or a statement that it could not be read.
/// </summary>
/// <remarks>
/// <see cref="Ok"/> false and an empty <see cref="Elements"/> are opposite verdicts.
/// A runtime that cannot load the automation client, a process that exited before it
/// was walked, and a timeout all leave the tree unreadable; a process that is simply
/// showing no windows leaves it readable and empty. A visual gate turns the first
/// into an infrastructure error and the second into a product finding, so the two
/// must not collapse into one value here.
/// </remarks>
/// <param name="Ok">Whether the tree was read at all.</param>
/// <param name="Detail">One sentence: the element count, or why it could not be read.</param>
/// <param name="Elements">Every element found, window nodes and their descendants, in tree order.</param>
public sealed record UiTreeSnapshot(bool Ok, string Detail, ReadOnlyCollection<UiElement> Elements)
{
    private static readonly ReadOnlyCollection<UiElement> None = new([]);

    /// <summary>The tree could not be read, for the given reason.</summary>
    public static UiTreeSnapshot Unreadable(string detail) =>
        new(false, string.IsNullOrWhiteSpace(detail) ? "the automation tree could not be read" : detail, None);

    /// <summary>The tree was read and holds these elements.</summary>
    public static UiTreeSnapshot Of(IEnumerable<UiElement> elements)
    {
        ArgumentNullException.ThrowIfNull(elements);
        var list = new ReadOnlyCollection<UiElement>([.. elements]);
        return new UiTreeSnapshot(true, $"{list.Count} automation element(s)", list);
    }

    /// <summary>
    /// The required strings that are not present anywhere in the tree, matched as a
    /// case-insensitive substring of an element's accessible name.
    /// </summary>
    /// <remarks>
    /// Substring, and against the name only: the assertion a visual gate makes is
    /// that the words reached the desktop, not that a particular control owns them.
    /// "the toast is not there" and "the toast is there with the wrong severity
    /// word" are different findings, so the missing strings are returned rather than
    /// a bare boolean.
    /// </remarks>
    public IReadOnlyList<string> MissingText(IReadOnlyCollection<string> required)
    {
        ArgumentNullException.ThrowIfNull(required);
        return
        [
            .. required.Where(needle => !this.Elements.Any(element =>
                element.Name.Contains(needle, StringComparison.OrdinalIgnoreCase))),
        ];
    }
}
