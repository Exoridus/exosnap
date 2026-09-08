namespace ExoSnap.Verify.Capabilities;

/// <summary>
/// The capability keys the catalog is allowed to be written against.
/// </summary>
/// <remarks>
/// A scenario declares what it needs by key, never by naming one desk's hardware.
/// Keys that describe a device are alias-shaped (<c>device.display.main-hdr</c>):
/// the alias resolves to a stable Windows id through a machine-local profile, so
/// the same catalog runs at a different desk without editing.
/// </remarks>
public static class CapabilityKeys
{
    /// <summary>Major Windows version, as a number ("10", "11").</summary>
    public const string OsWindows = "os.windows";

    /// <summary>Full operating system version string.</summary>
    public const string OsVersion = "os.version";

    /// <summary>Whether the harness process holds an elevated token.</summary>
    public const string Elevated = "elevated";

    /// <summary>Whether the input desktop can be opened at all.</summary>
    public const string InteractiveDesktop = "interactiveDesktop";

    /// <summary>Whether Windows Sandbox is installed on this machine.</summary>
    public const string SandboxAvailable = "sandbox.available";

    /// <summary>Whether a pinned PresentMon is resolvable.</summary>
    public const string PresentMonAvailable = "presentmon.available";

    /// <summary>Whether SoundVolumeView is resolvable.</summary>
    public const string SoundVolumeViewAvailable = "soundvolumeview.available";

    /// <summary>Whether ffprobe is resolvable.</summary>
    public const string FfprobeAvailable = "ffprobe.available";

    /// <summary>Vendor of the first hardware adapter DXGI enumerates.</summary>
    public const string GpuVendor = "gpu.vendor";

    /// <summary>Whether a hardware Direct3D 11 device can be created.</summary>
    public const string GpuD3D11 = "gpu.d3d11";

    /// <summary>Whether any output is presenting in an HDR colour space right now.</summary>
    public const string DisplayHdr = "display.hdr";

    /// <summary>Prefix for one refresh rate the displays offer, for example display.refresh.240.</summary>
    public const string DisplayRefreshPrefix = "display.refresh.";

    /// <summary>Prefix for one endpoint sample rate present on the machine, for example audio.endpoint.44100.</summary>
    public const string AudioEndpointPrefix = "audio.endpoint.";

    /// <summary>Prefix for a device alias binding, for example device.display.main-hdr.</summary>
    public const string DevicePrefix = "device.";

    /// <summary>The value a capability carries when the probes could not determine it.</summary>
    public const string Unknown = "unknown";

    /// <summary>The value a device alias carries when the alias profile resolves it.</summary>
    public const string Bound = "bound";

    /// <summary>The value a device alias carries when no profile binds it on this machine.</summary>
    public const string Unbound = "unbound";

    /// <summary>The capability key for one refresh rate.</summary>
    public static string DisplayRefresh(int hz) =>
        DisplayRefreshPrefix + hz.ToString(System.Globalization.CultureInfo.InvariantCulture);

    /// <summary>The capability key for one endpoint sample rate.</summary>
    public static string AudioEndpoint(int sampleRateHz) =>
        AudioEndpointPrefix + sampleRateHz.ToString(System.Globalization.CultureInfo.InvariantCulture);

    /// <summary>The capability key for one device alias.</summary>
    public static string Device(string alias) => DevicePrefix + alias;
}
