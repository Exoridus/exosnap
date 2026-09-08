using System.Globalization;
using System.Security.Cryptography;

namespace ExoSnap.Verify.Models;

/// <summary>
/// A file a verdict rests on, bound to its content.
/// </summary>
/// <param name="Name">Short label naming what the file is, unique within a scenario.</param>
/// <param name="Path">Absolute path of the file at the time it was captured.</param>
/// <param name="Sha256">Lowercase hex SHA-256 of the file content.</param>
/// <remarks>
/// The digest is what makes evidence citable after the fact. A qualification
/// record that names a log without binding its content describes a file that may
/// since have been rewritten.
/// </remarks>
public sealed record Evidence(string Name, string Path, string Sha256)
{
    /// <summary>
    /// Captures an existing file as evidence, hashing its current content.
    /// </summary>
    /// <exception cref="FileNotFoundException">The file does not exist.</exception>
    public static Evidence ForFile(string name, string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        ArgumentException.ThrowIfNullOrWhiteSpace(path);

        var full = System.IO.Path.GetFullPath(path);
        if (!File.Exists(full))
        {
            throw new FileNotFoundException("Evidence file does not exist.", full);
        }

        using var stream = File.OpenRead(full);
        var digest = SHA256.HashData(stream);
        return new Evidence(name, full, Convert.ToHexString(digest).ToLower(CultureInfo.InvariantCulture));
    }
}
