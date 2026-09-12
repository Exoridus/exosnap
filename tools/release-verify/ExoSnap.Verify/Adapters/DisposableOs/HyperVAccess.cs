using System.Security.Principal;

namespace ExoSnap.Verify.Adapters.DisposableOs;

/// <summary>
/// What this machine and this session allow of Hyper-V.
/// </summary>
/// <param name="ModulePresent">The Hyper-V PowerShell module is installed.</param>
/// <param name="InAdministratorsGroup">The account is in the local Hyper-V Administrators group.</param>
/// <param name="Elevated">The session is running elevated.</param>
/// <param name="HostServiceRunning">The virtual machine management service is running.</param>
/// <remarks>
/// Hyper-V is administered through a group, not only through elevation. A member of
/// the local Hyper-V Administrators group creates, starts and removes machines with
/// no consent prompt, so a transport that demanded elevation anyway would put a
/// Secure Desktop prompt in front of every campaign -- something no automation can
/// answer -- on a machine where the rights were already granted.
/// </remarks>
public sealed record HyperVAccess(
    bool ModulePresent,
    bool InAdministratorsGroup,
    bool Elevated,
    bool HostServiceRunning)
{
    /// <summary>The well-known SID of the local Hyper-V Administrators group.</summary>
    /// <remarks>
    /// Matched by SID because the group name is localized and a machine is not
    /// necessarily en-US throughout.
    /// </remarks>
    public const string AdministratorsGroupSid = "S-1-5-32-578";

    /// <summary>Whether Hyper-V can be driven from here at all.</summary>
    public bool Usable =>
        this.ModulePresent && this.HostServiceRunning && (this.InAdministratorsGroup || this.Elevated);

    /// <summary>Reads what this machine and session actually allow.</summary>
    public static HyperVAccess Measure(
        Func<string, bool> moduleExists,
        Func<string, bool> serviceRunning)
    {
        ArgumentNullException.ThrowIfNull(moduleExists);
        ArgumentNullException.ThrowIfNull(serviceRunning);

        using var identity = WindowsIdentity.GetCurrent();
        var principal = new WindowsPrincipal(identity);
        return new HyperVAccess(
            ModulePresent: moduleExists("Hyper-V"),
            InAdministratorsGroup: principal.IsInRole(new SecurityIdentifier(AdministratorsGroupSid)),
            Elevated: principal.IsInRole(WindowsBuiltInRole.Administrator),
            HostServiceRunning: serviceRunning("vmms"));
    }

    /// <summary>
    /// Everything standing in the way, or an empty string when nothing is.
    /// </summary>
    /// <remarks>
    /// All of it at once, and the group before elevation: adding a group membership
    /// is the fix that does not need a prompt on every later run, so suggesting
    /// elevation instead would make the machine permanently worse.
    /// </remarks>
    public string DescribeUnavailable()
    {
        if (this.Usable)
        {
            return string.Empty;
        }

        var missing = new List<string>();
        if (!this.ModulePresent)
        {
            missing.Add("the Hyper-V feature is not installed (Enable-WindowsOptionalFeature -Online "
                + "-FeatureName Microsoft-Hyper-V -All)");
        }

        if (!this.HostServiceRunning)
        {
            missing.Add("the Hyper-V virtual machine management service (vmms) is not running");
        }

        if (!this.InAdministratorsGroup && !this.Elevated)
        {
            missing.Add("this account is not in the local Hyper-V Administrators group "
                + $"({AdministratorsGroupSid}); adding it needs elevation once and no prompt afterwards");
        }

        return string.Join("; ", missing);
    }
}
