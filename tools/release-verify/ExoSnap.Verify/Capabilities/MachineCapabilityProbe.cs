using System.Collections.ObjectModel;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using ExoSnap.Verify.Json;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Capabilities;

/// <summary>
/// Measures what this machine can do, once, before any scenario runs.
/// </summary>
/// <remarks>
/// Every probe here is read-only and cheap. Anything that would need to change
/// the machine to find out, or that would need a tool to be started, is reported
/// as <c>unknown</c> instead: a capability document that guessed would send
/// scenarios at hardware that is not there and report the resulting mess as a
/// product defect.
/// </remarks>
public sealed class MachineCapabilityProbe
{
    private const string EnvPresentMon = "EXOSNAP_PRESENTMON";
    private const string EnvSoundVolumeView = "EXOSNAP_SOUNDVOLUMEVIEW";
    private const string EnvFfprobe = "EXOSNAP_FFPROBE";

    private readonly ToolResolver tools;

    /// <summary>Probes with the real process environment.</summary>
    public MachineCapabilityProbe()
        : this(new ToolResolver())
    {
    }

    /// <summary>Probes using an injected tool resolver.</summary>
    public MachineCapabilityProbe(ToolResolver tools)
    {
        ArgumentNullException.ThrowIfNull(tools);
        this.tools = tools;
    }

    /// <summary>Measures the machine and returns both the values and their provenance.</summary>
    public MachineCapabilityDocument Probe()
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var sources = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        void Record(string key, string? value, string source)
        {
            values[key] = value ?? CapabilityKeys.Unknown;
            sources[key] = source;
        }

        var osVersion = Environment.OSVersion.Version;
        Record(CapabilityKeys.OsVersion, Environment.OSVersion.VersionString, "System.Environment");

        // Windows 11 shares major version 10 with Windows 10 and is told apart
        // only by the build number, so the marketing major is derived rather than
        // read.
        Record(
            CapabilityKeys.OsWindows,
            OperatingSystem.IsWindows()
                ? (osVersion.Build >= 22000 ? "11" : "10")
                : null,
            "System.Environment");

        Record(CapabilityKeys.Elevated, Format(ProcessPrivilege.IsElevated()), "GetTokenInformation");
        Record(CapabilityKeys.InteractiveDesktop, Format(InteractiveDesktop.IsReachable()), "OpenInputDesktop");
        Record(CapabilityKeys.SandboxAvailable, Format(ProbeSandbox()), "filesystem");

        RecordTool(Record, CapabilityKeys.PresentMonAvailable, "PresentMon", EnvPresentMon);
        RecordTool(Record, CapabilityKeys.SoundVolumeViewAvailable, "SoundVolumeView", EnvSoundVolumeView);
        RecordTool(Record, CapabilityKeys.FfprobeAvailable, "ffprobe", EnvFfprobe);

        var adapters = GraphicsProbe.TryEnumerateAdapters();
        var hardwareAdapters = adapters?.Where(adapter => !adapter.IsSoftware).ToList();
        Record(
            CapabilityKeys.GpuVendor,
            hardwareAdapters is { Count: > 0 } ? hardwareAdapters[0].Vendor : null,
            "DXGI");
        Record(CapabilityKeys.GpuD3D11, Format(GraphicsProbe.TryCreateHardwareDevice()), "D3D11CreateDevice");

        var outputs = adapters?.SelectMany(adapter => adapter.Outputs).ToList();
        Record(
            CapabilityKeys.DisplayHdr,
            outputs is null ? null : Format(outputs.Any(output => output.IsHdr)),
            "IDXGIOutput6");

        foreach (var rate in DisplayModeProbe.TryEnumerateRefreshRates() ?? new ReadOnlyCollection<uint>([]))
        {
            Record(CapabilityKeys.DisplayRefresh((int)rate), "true", "EnumDisplaySettingsEx");
        }

        var endpoints = AudioEndpointProbe.TryEnumerateActiveEndpoints();
        if (endpoints is not null)
        {
            foreach (var sampleRate in endpoints
                         .Where(endpoint => endpoint.SampleRateHz is not null)
                         .Select(endpoint => endpoint.SampleRateHz!.Value)
                         .Distinct()
                         .Order())
            {
                Record(CapabilityKeys.AudioEndpoint((int)sampleRate), "true", "WASAPI");
            }
        }

        var gpuNames = new ReadOnlyCollection<string>(
            adapters?.Select(adapter => adapter.Description).ToList() ?? []);

        var machine = new MachineFingerprint(
            Fingerprint(osVersion.ToString(), gpuNames),
            Environment.OSVersion.VersionString,
            gpuNames);

        return new MachineCapabilityDocument(
            MachineCapabilityDocument.CurrentSchemaVersion,
            DateTimeOffset.UtcNow,
            machine,
            new ReadOnlyDictionary<string, string>(
                values.OrderBy(pair => pair.Key, StringComparer.Ordinal)
                    .ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal)),
            new ReadOnlyDictionary<string, string>(
                sources.OrderBy(pair => pair.Key, StringComparer.Ordinal)
                    .ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal)));
    }

    /// <summary>Measures the machine and returns the values as a comparable set.</summary>
    public CapabilitySet ProbeSet()
    {
        var document = this.Probe();
        return new CapabilitySet(document.Capabilities, document.Sources);
    }

    private void RecordTool(
        Action<string, string?, string> record,
        string key,
        string toolName,
        string environmentVariable)
    {
        var resolved = this.tools.Resolve(toolName, environmentVariable);
        record(key, Format(resolved.Available), resolved.Source);
    }

    private static bool? ProbeSandbox()
    {
        if (!OperatingSystem.IsWindows())
        {
            return null;
        }

        var system = Environment.GetFolderPath(Environment.SpecialFolder.System);
        return string.IsNullOrEmpty(system)
            ? null
            : File.Exists(Path.Combine(system, "WindowsSandbox.exe"));
    }

    private static string? Format(bool? value) => value switch
    {
        true => "true",
        false => "false",
        null => null,
    };

    // Identifies the machine without naming it: two runs on the same desk share a
    // fingerprint, and the fingerprint reveals nothing a report could not already
    // say out loud.
    private static string Fingerprint(string osVersion, IEnumerable<string> gpus)
    {
        var material = string.Join(
            '|',
            [osVersion, .. gpus.Order(StringComparer.Ordinal)]);
        var digest = SHA256.HashData(Encoding.UTF8.GetBytes(material));
        return Convert.ToHexString(digest)[..16].ToLower(CultureInfo.InvariantCulture);
    }
}
