namespace ExoSnap.Verify.Windows.Uia;

/// <summary>
/// Reads a process's UI Automation tree.
/// </summary>
/// <remarks>
/// The five capture-excluded overlays and the toast window carry
/// <c>WDA_EXCLUDEFROMCAPTURE</c>, which defeats screenshots, screen recording and
/// <c>PrintWindow</c> by design. It does not touch the automation tree, so this is
/// the one reader that can say a surface really reached the desktop with really
/// that text on it. It cannot say what colour anything is; that is why the visual
/// gates still end in a person's judgement.
/// </remarks>
public interface IUiAutomation
{
    /// <summary>
    /// The automation tree of one process: its top-level windows and their
    /// descendants.
    /// </summary>
    /// <param name="processId">The process to walk.</param>
    /// <param name="timeout">
    /// How long to keep retrying while the process has no window yet. A freshly
    /// launched process creates its native windows asynchronously, so a single
    /// query would race the surface it is meant to observe.
    /// </param>
    /// <returns>
    /// The tree, or <see cref="UiTreeSnapshot.Unreadable"/> when the automation
    /// client itself could not be used. A live process that showed no window
    /// before the deadline returns a readable, empty tree, not an unreadable one.
    /// </returns>
    UiTreeSnapshot SnapshotProcess(int processId, TimeSpan timeout);
}
