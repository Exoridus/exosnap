using System.Security.Cryptography;
using System.Text.RegularExpressions;

namespace ExoSnap.Verify.Gates;

/// <summary>The release MSI belonging to the bound artifact, or why there is none.</summary>
/// <param name="Path">The identified installer, or null when none was.</param>
/// <param name="Sha256">Its digest in lower-case hex, empty when nothing was identified.</param>
/// <param name="Detail">Why nothing was identified, empty on success.</param>
public sealed record ReleaseMsiLookup(string? Path, string Sha256, string Detail);

/// <summary>
/// Finds the release MSI a campaign's artifact belongs to.
/// </summary>
/// <remarks>
/// A campaign binds one file, the portable <c>exosnap.exe</c>, so the installer is
/// looked for beside it and then one directory up. Identity is established before
/// any gate hands the file to msiexec: "the single .msi that happened to be in
/// that folder" is not an identification, and the gate that consumes this answer
/// uninstalls whatever it finds before installing this file. Two candidates are
/// ambiguous rather than a guess.
///
/// The digest is computed from the local bytes, because that is what Chocolatey
/// verifies its download against. A published <c>.sha256</c> sidecar is compared
/// rather than trusted: a disagreement means the file on disk is not the one the
/// release published, and a rehearsal of the wrong bytes proves nothing.
/// </remarks>
public static partial class ReleaseMsiArtifact
{
    /// <summary>The variable naming the installer explicitly.</summary>
    public const string PathVariable = "EXOSNAP_RELEASE_MSI";

    /// <summary>Locates the installer for an artifact, or explains why it could not.</summary>
    /// <param name="executablePath">The bound portable executable to look beside.</param>
    /// <param name="readEnvironment">Reads an environment variable by name.</param>
    public static ReleaseMsiLookup Locate(string executablePath, Func<string, string?> readEnvironment)
    {
        ArgumentNullException.ThrowIfNull(readEnvironment);

        var pinned = readEnvironment(PathVariable);
        if (!string.IsNullOrWhiteSpace(pinned))
        {
            return File.Exists(pinned)
                ? Identify(pinned)
                : NotFound($"{PathVariable} names '{pinned}', which does not exist");
        }

        if (string.IsNullOrWhiteSpace(executablePath) || !File.Exists(executablePath))
        {
            return NotFound("the campaign artifact names no readable executable to look beside");
        }

        var directory = Path.GetDirectoryName(executablePath);
        foreach (var root in new[] { directory, Path.GetDirectoryName(directory) })
        {
            if (string.IsNullOrWhiteSpace(root) || !Directory.Exists(root))
            {
                continue;
            }

            var candidates = Directory.GetFiles(root, "*.msi")
                .Where(candidate => PublishedName().IsMatch(Path.GetFileName(candidate)))
                .ToArray();
            if (candidates.Length == 1)
            {
                return Identify(candidates[0]);
            }

            if (candidates.Length > 1)
            {
                return NotFound(
                    $"{candidates.Length} published .msi files sit beside the bound artifact " +
                    $"({string.Join(", ", candidates.Select(Path.GetFileName))}); name one with {PathVariable}");
            }
        }

        return NotFound(
            "no ExoSnap-<version>-windows-x64.msi was found beside the bound artifact; the campaign binds the " +
            $"portable exosnap.exe only. Put the published .msi next to it, or name it with {PathVariable}");
    }

    [GeneratedRegex(@"^ExoSnap-[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?-windows-x64\.msi$")]
    private static partial Regex PublishedName();

    private static ReleaseMsiLookup NotFound(string detail) => new(null, string.Empty, detail);

    private static ReleaseMsiLookup Identify(string path)
    {
        var name = Path.GetFileName(path);
        if (!PublishedName().IsMatch(name))
        {
            return NotFound(
                $"'{name}' is not named like a published release (ExoSnap-<version>-windows-x64.msi), " +
                "so it is not identified as the artifact under test");
        }

        using var stream = File.OpenRead(path);
        var sha256 = Convert.ToHexStringLower(SHA256.HashData(stream));

        var sidecar = path + ".sha256";
        if (File.Exists(sidecar))
        {
            var published = File.ReadAllText(sidecar)
                .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .FirstOrDefault(token => token.Length == 64 && token.All(Uri.IsHexDigit));
            if (published is not null && !string.Equals(published, sha256, StringComparison.OrdinalIgnoreCase))
            {
                return NotFound(
                    $"{name} hashes to {sha256} but its .sha256 sidecar publishes " +
                    $"{published.ToLowerInvariant()}; this is not the released file");
            }
        }

        return new ReleaseMsiLookup(path, sha256, string.Empty);
    }
}
