using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace ExoSnap.Verify.Engine;

/// <summary>
/// What a verdict was measured with, beyond the artifact and the catalog.
/// </summary>
/// <param name="OsBuild">The Windows build the run was carried out on.</param>
/// <param name="GpuDriver">The display driver version the capture path ran through.</param>
/// <param name="Ffprobe">The independent media oracle, by version.</param>
/// <param name="PresentMon">The independent present oracle, by version.</param>
/// <remarks>
/// <para>
/// A verdict is a statement about bytes measured by something, on something. The
/// bytes are already bound -- a changed executable invalidates a run -- and the rest
/// was not: the same artifact measured by a different ffprobe, or recorded through a
/// different display driver, is a different measurement, and reusing the older
/// verdict would let a local pass from last month qualify a release nobody re-ran.
/// </para>
/// <para>
/// Every field is a version string read from the machine, never a path: two machines
/// with the same tool at the same version produce the same fingerprint, which is what
/// makes a verdict transferable at all. A field nothing could read is
/// <c>unknown</c>, and <c>unknown</c> never equals <c>unknown</c> for the purpose of
/// reuse -- the safe direction, and the same rule the present confirmation follows.
/// </para>
/// </remarks>
public sealed record ToolingFingerprint(string OsBuild, string GpuDriver, string Ffprobe, string PresentMon)
{
    /// <summary>The value a field takes when nothing could read it.</summary>
    public const string Unknown = "unknown";

    /// <summary>A fingerprint in which nothing was measured.</summary>
    public static ToolingFingerprint Nothing { get; } = new(Unknown, Unknown, Unknown, Unknown);

    /// <summary>
    /// A short stable digest over the four fields, or an empty string when any of them
    /// is unknown.
    /// </summary>
    /// <remarks>
    /// Empty rather than a digest over the word "unknown": an empty fingerprint is
    /// what a reuse check refuses, so a run that could not describe what it measured
    /// with cannot hand its verdicts to another one. A digest would compare equal to
    /// the next equally ignorant run and let exactly that happen.
    /// </remarks>
    public string Digest
    {
        get
        {
            // Not named `field`: inside a property accessor that is a contextual
            // keyword and binds to a synthesized backing store.
            var measured = new[] { this.OsBuild, this.GpuDriver, this.Ffprobe, this.PresentMon };
            if (measured.Any(IsUnknown))
            {
                return string.Empty;
            }

            var material = string.Join(
                '\n',
                new[]
                {
                    "os:" + this.OsBuild,
                    "gpu:" + this.GpuDriver,
                    "ffprobe:" + this.Ffprobe,
                    "presentmon:" + this.PresentMon,
                }.Order(StringComparer.Ordinal));

            return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(material)))[..16]
                .ToLower(CultureInfo.InvariantCulture);
        }
    }

    /// <summary>
    /// Whether a verdict recorded under <paramref name="recorded"/> may be reused by a
    /// run measuring under this fingerprint.
    /// </summary>
    /// <remarks>
    /// Never when either side is empty. A run that did not record what it measured
    /// with has not shown that it measured under the same conditions, and the absence
    /// of a record is not evidence that the two agree -- an older state file simply
    /// carries no fingerprint at all, and reading that as "unchanged" is how a local
    /// pass from before a driver update would qualify a release.
    /// </remarks>
    public bool Accepts(string recorded) =>
        this.Digest.Length > 0 && string.Equals(this.Digest, recorded, StringComparison.OrdinalIgnoreCase);

    /// <summary>Why a recorded fingerprint is not this one, for a verdict message.</summary>
    public string DescribeMismatch(string recorded)
    {
        if (this.Digest.Length == 0)
        {
            return "this run cannot say what it is measuring with (" + this.DescribeUnknownFields() + ")";
        }

        return recorded.Length == 0
            ? "the recorded verdicts do not say what they were measured with"
            : "the oracles, the operating system or the display driver changed since this verdict was recorded";
    }

    private string DescribeUnknownFields()
    {
        var unknown = new List<string>();
        if (IsUnknown(this.OsBuild)) { unknown.Add("the Windows build"); }
        if (IsUnknown(this.GpuDriver)) { unknown.Add("the display driver"); }
        if (IsUnknown(this.Ffprobe)) { unknown.Add("ffprobe"); }
        if (IsUnknown(this.PresentMon)) { unknown.Add("PresentMon"); }
        return string.Join(", ", unknown);
    }

    private static bool IsUnknown(string field) =>
        string.IsNullOrWhiteSpace(field) || string.Equals(field, Unknown, StringComparison.OrdinalIgnoreCase);
}
