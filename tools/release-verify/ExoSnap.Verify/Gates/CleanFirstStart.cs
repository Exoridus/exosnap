namespace ExoSnap.Verify.Gates;

/// <summary>
/// What a machine has of ExoSnap before the candidate is installed on it.
/// </summary>
/// <param name="InstalledProduct">A product entry, or empty when nothing is installed.</param>
/// <param name="UserConfigurationPresent">The per-user configuration directory exists.</param>
/// <param name="RecoveryStatePresent">A recovery manifest from a previous recording run exists.</param>
/// <param name="UpdateStatePresent">Update state from an earlier install exists.</param>
/// <param name="UserRegistryPresent">The per-user registry key exists.</param>
/// <remarks>
/// "Clean" is a state, not a screen. What a first start has to be measured against is
/// a machine with nothing of ExoSnap on it: no install, no configuration, no recovery
/// manifest inviting a recovery prompt, no update state, and no per-user registry
/// residue. Any one of those changes what the first start does, so a run on a machine
/// that has one is measuring something else -- and reporting that as a first-start
/// verdict would be a result nobody can repeat.
/// </remarks>
public sealed record CleanFirstStartState(
    string InstalledProduct,
    bool UserConfigurationPresent,
    bool RecoveryStatePresent,
    bool UpdateStatePresent,
    bool UserRegistryPresent)
{
    /// <summary>A machine with nothing of ExoSnap on it.</summary>
    public static CleanFirstStartState Clean { get; } = new(string.Empty, false, false, false, false);

    /// <summary>Whether a first start can be measured here.</summary>
    public bool IsClean => this.DescribeResidue().Length == 0;

    /// <summary>
    /// Everything of an earlier ExoSnap still on the machine, or an empty string.
    /// </summary>
    /// <remarks>
    /// All of it at once. Each of these has to be cleared by hand or by starting from
    /// a fresh image, and learning about them one campaign at a time is one image
    /// rebuild per residue.
    /// </remarks>
    public string DescribeResidue()
    {
        var found = new List<string>();
        if (!string.IsNullOrWhiteSpace(this.InstalledProduct))
        {
            found.Add($"an installed product ({this.InstalledProduct})");
        }

        if (this.UserConfigurationPresent)
        {
            found.Add("a per-user configuration directory");
        }

        if (this.RecoveryStatePresent)
        {
            // The one that changes what the first start LOOKS like: a recovery
            // manifest opens the recovery surface, and a gate asserting first-start
            // defaults behind it would be asserting against the wrong window.
            found.Add("recovery state from a previous recording run");
        }

        if (this.UpdateStatePresent)
        {
            found.Add("update state from an earlier install");
        }

        if (this.UserRegistryPresent)
        {
            found.Add("a per-user registry key");
        }

        return found.Count == 0 ? string.Empty : string.Join(", ", found);
    }
}
