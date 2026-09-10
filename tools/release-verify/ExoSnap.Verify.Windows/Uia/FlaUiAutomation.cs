using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using FlaUI.Core.AutomationElements;
using FlaUI.UIA3;

namespace ExoSnap.Verify.Windows.Uia;

/// <summary>
/// Reads a process's UI Automation tree through the UIA3 client.
/// </summary>
/// <remarks>
/// Observation only. It never invokes a pattern, sets focus, or synthesises input:
/// a visual gate that drove the UI would be steering the very surface it is asked
/// to judge, and taking focus from whoever is at the machine is the thing the
/// harness must never do.
/// </remarks>
public sealed class FlaUiAutomation : IUiAutomation
{
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(250);

    /// <inheritdoc/>
    public UiTreeSnapshot SnapshotProcess(int processId, TimeSpan timeout)
    {
        if (!NativeGate.IsWindows)
        {
            return UiTreeSnapshot.Unreadable("UI Automation is a Windows facility and this is not Windows");
        }

        if (!ProcessIsRunning(processId))
        {
            return UiTreeSnapshot.Unreadable(
                $"process {processId.ToString(System.Globalization.CultureInfo.InvariantCulture)} is not running, " +
                "so it has no automation tree");
        }

        try
        {
            using var automation = new UIA3Automation();
            var desktop = automation.GetDesktop();
            var deadline = DateTime.UtcNow.Add(timeout < TimeSpan.Zero ? TimeSpan.Zero : timeout);

            while (true)
            {
                var windows = desktop.FindAllChildren(condition => condition.ByProcessId(processId));
                if (windows.Length > 0)
                {
                    return UiTreeSnapshot.Of(Walk(windows));
                }

                if (DateTime.UtcNow >= deadline || !ProcessIsRunning(processId))
                {
                    // The client answered; the process simply has no window on the
                    // desktop. That is a readable, empty tree and a legitimate finding
                    // for a gate that expected a surface to be up, not a failure to read.
                    return UiTreeSnapshot.Of([]);
                }

                Thread.Sleep(PollInterval);
            }
        }
        catch (Exception exception) when (IsAutomationClientFault(exception))
        {
            return UiTreeSnapshot.Unreadable(
                $"the UI Automation client could not be used ({exception.GetType().Name}: {exception.Message}); " +
                "the capture-excluded surfaces have no other reader");
        }
    }

    private static IEnumerable<UiElement> Walk(IEnumerable<AutomationElement> windows)
    {
        foreach (var window in windows)
        {
            yield return Describe(window);
            foreach (var descendant in window.FindAllDescendants())
            {
                yield return Describe(descendant);
            }
        }
    }

    // ValueOrDefault rather than the direct property: an element that does not
    // support a property throws on the direct accessor, and a half-read tree is
    // worse than a fully read one with blank fields.
    private static UiElement Describe(AutomationElement element) =>
        new(
            element.Properties.Name.ValueOrDefault ?? string.Empty,
            element.Properties.ClassName.ValueOrDefault ?? string.Empty,
            element.Properties.ControlType.ValueOrDefault.ToString(),
            element.Properties.AutomationId.ValueOrDefault ?? string.Empty);

    private static bool ProcessIsRunning(int processId)
    {
        try
        {
            using var process = Process.GetProcessById(processId);
            return !process.HasExited;
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
    }

    // A fault reading the tree, never a verdict about the product: the client
    // failing to load, COM refusing the call, or the platform not carrying UI
    // Automation at all.
    private static bool IsAutomationClientFault(Exception exception) => exception is
        COMException or
        TypeInitializationException or
        DllNotFoundException or
        EntryPointNotFoundException or
        PlatformNotSupportedException or
        MarshalDirectiveException or
        InvalidCastException or
        Win32Exception or
        TimeoutException;
}
