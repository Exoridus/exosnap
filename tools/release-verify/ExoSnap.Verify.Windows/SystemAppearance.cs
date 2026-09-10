using Microsoft.Win32;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.UI.WindowsAndMessaging;

namespace ExoSnap.Verify.Windows;

/// <summary>Which appearance Windows applies to app surfaces.</summary>
public enum AppsAppearance
{
    /// <summary>The setting could not be read.</summary>
    Unknown,

    /// <summary>Apps are asked to use their light appearance.</summary>
    Light,

    /// <summary>Apps are asked to use their dark appearance.</summary>
    Dark,
}

/// <summary>Reads and sets the Windows apps-colour appearance.</summary>
/// <remarks>
/// A documented, restorable HKCU setting, so the harness drives it rather than
/// asking the developer to toggle Windows themselves. The one visual gate that uses
/// it pairs every set with a restore and confirms the restore landed.
/// </remarks>
public interface ISystemAppearance
{
    /// <summary>The appearance in effect now, or <see cref="AppsAppearance.Unknown"/> when it cannot be read.</summary>
    AppsAppearance Current { get; }

    /// <summary>
    /// Applies an appearance and tells running apps to repaint.
    /// </summary>
    /// <exception cref="ArgumentException"><paramref name="appearance"/> is <see cref="AppsAppearance.Unknown"/>.</exception>
    /// <exception cref="InvalidOperationException">The setting could not be written.</exception>
    void Apply(AppsAppearance appearance);
}

/// <summary>
/// The apps-colour appearance, backed by
/// <c>HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize</c>.
/// </summary>
public sealed class WindowsSystemAppearance : ISystemAppearance
{
    private const string PersonalizeKey =
        @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize";
    private const string AppsUseLightThemeValue = "AppsUseLightTheme";

    /// <inheritdoc/>
    public AppsAppearance Current
    {
        get
        {
            if (!NativeGate.IsWindows)
            {
                return AppsAppearance.Unknown;
            }

            using var key = Registry.CurrentUser.OpenSubKey(PersonalizeKey);
            return FromRegistryValue(key?.GetValue(AppsUseLightThemeValue));
        }
    }

    /// <summary>
    /// Maps the raw <c>AppsUseLightTheme</c> value: 1 is light, 0 is dark, and a
    /// missing or non-numeric value is unknown rather than a guessed default.
    /// </summary>
    public static AppsAppearance FromRegistryValue(object? raw) => raw switch
    {
        int and not 0 => AppsAppearance.Light,
        int => AppsAppearance.Dark,
        _ => AppsAppearance.Unknown,
    };

    /// <inheritdoc/>
    public void Apply(AppsAppearance appearance)
    {
        if (appearance == AppsAppearance.Unknown)
        {
            throw new ArgumentException("Cannot apply an unknown appearance.", nameof(appearance));
        }

        if (!NativeGate.IsWindows)
        {
            throw new InvalidOperationException("The apps-colour appearance is a Windows setting.");
        }

        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(PersonalizeKey);
            key.SetValue(AppsUseLightThemeValue, appearance == AppsAppearance.Light ? 1 : 0, RegistryValueKind.DWord);
        }
        catch (Exception exception) when (exception
            is UnauthorizedAccessException or System.Security.SecurityException or IOException)
        {
            throw new InvalidOperationException(
                $"the apps-colour appearance could not be set to {appearance}: {exception.Message}", exception);
        }

        // RegSetValueEx does not itself notify anyone: without this broadcast the
        // toast and any theme-following surface keep the old colours, and the
        // operator would be judging a change that never reached the product.
        Broadcast();
    }

    private static void Broadcast()
    {
        unsafe
        {
            fixed (char* immersive = "ImmersiveColorSet")
            {
                _ = PInvoke.SendMessageTimeout(
                    HWND.HWND_BROADCAST,
                    PInvoke.WM_SETTINGCHANGE,
                    default,
                    (nint)immersive,
                    SEND_MESSAGE_TIMEOUT_FLAGS.SMTO_ABORTIFHUNG,
                    1000);
            }
        }
    }
}
