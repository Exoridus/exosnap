using ExoSnap.Verify.Gates;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// A campaign binding has to identify one candidate before a gate asserts
/// anything about it.
/// </summary>
/// <remarks>
/// The update gate's whole proof used to be "the version is not what we started
/// from", which any newer build satisfies -- including another RC of the same base
/// version that nobody verified. Comparing against the bound candidate instead
/// needs the candidate's identity to be complete and internally consistent, and a
/// binding assembled from mismatched parts has to be caught BEFORE the guest is
/// started: found at the end of a long run it would read as a product failure,
/// when in fact the harness could not say what it expected.
/// </remarks>
public sealed class CandidateIdentityTests
{
    private static ArtifactUnderTest Bound(
        string version = "0.9.1-rc4",
        string tag = "v0.9.1-rc4",
        string commit = "1111111111111111111111111111111111111111",
        string sha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") =>
        new("c:/rc/exosnap.exe", version, "c:/repo", tag, commit, sha);

    [Fact]
    public void ACompleteConsistentBindingIdentifiesACandidate()
    {
        Assert.Empty(Bound().DescribeIncompleteCandidateIdentity());
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    public void AMissingVersionIsNamed(string version)
    {
        var reason = Bound(version: version).DescribeIncompleteCandidateIdentity();
        Assert.Contains("productVersion", reason, StringComparison.Ordinal);
    }

    [Fact]
    public void AMissingTagIsNamed()
    {
        Assert.Contains("rcTag", Bound(tag: "").DescribeIncompleteCandidateIdentity(), StringComparison.Ordinal);
    }

    [Fact]
    public void AMissingCommitIsNamed()
    {
        // The field that makes "this build" different from "a build with this
        // version number".
        Assert.Contains(
            "sourceCommit", Bound(commit: "").DescribeIncompleteCandidateIdentity(), StringComparison.Ordinal);
    }

    [Fact]
    public void AMissingExecutableDigestIsNamed()
    {
        Assert.Contains(
            "executableSha256", Bound(sha: "").DescribeIncompleteCandidateIdentity(), StringComparison.Ordinal);
    }

    [Fact]
    public void EveryMissingFieldIsNamedAtOnce()
    {
        // A harness told one missing field at a time fixes it, re-runs, and learns
        // the next -- across a campaign that takes minutes to reach this point.
        var reason = new ArtifactUnderTest("c:/rc/exosnap.exe", "", "c:/repo").DescribeIncompleteCandidateIdentity();

        Assert.Contains("productVersion", reason, StringComparison.Ordinal);
        Assert.Contains("rcTag", reason, StringComparison.Ordinal);
        Assert.Contains("sourceCommit", reason, StringComparison.Ordinal);
        Assert.Contains("executableSha256", reason, StringComparison.Ordinal);
    }

    [Fact]
    public void ABindingWhosePartsCameFromDifferentCandidatesIsRejected()
    {
        // The reviewer's case: rc4's version with rc5's tag. Every field is
        // present, so a completeness check alone would accept it -- and the gate
        // would then compare an installed build against an expectation nobody
        // holds.
        var reason = Bound(version: "0.9.1-rc4", tag: "v0.9.1-rc5").DescribeIncompleteCandidateIdentity();

        Assert.Contains("inconsistent", reason, StringComparison.Ordinal);
        Assert.Contains("v0.9.1-rc5", reason, StringComparison.Ordinal);
        Assert.Contains("0.9.1-rc4", reason, StringComparison.Ordinal);
    }

    [Fact]
    public void TheTagsLeadingVIsNotPartOfTheVersion()
    {
        // Both spellings are the same candidate. A check that missed this would
        // reject every real binding.
        Assert.Empty(Bound(version: "0.9.1-rc4", tag: "v0.9.1-rc4").DescribeIncompleteCandidateIdentity());
        Assert.Empty(Bound(version: "0.9.1-rc4", tag: "0.9.1-rc4").DescribeIncompleteCandidateIdentity());
    }

    [Fact]
    public void AFinalReleaseBindingIsAccepted()
    {
        // Not every candidate is an RC.
        Assert.Empty(Bound(version: "0.9.1", tag: "v0.9.1").DescribeIncompleteCandidateIdentity());
    }

    [Fact]
    public void ANewerRcOfTheSameBaseVersionIsADifferentCandidate()
    {
        // The property the gate rests on, stated here because it is the whole
        // reason the version alone is not enough: rc4 and rc5 share a base version
        // and are different builds, so "newer than what we started from" cannot
        // distinguish them.
        Assert.NotEqual("0.9.1-rc4", "0.9.1-rc5");
        Assert.Empty(Bound(version: "0.9.1-rc5", tag: "v0.9.1-rc5").DescribeIncompleteCandidateIdentity());
        Assert.NotEmpty(Bound(version: "0.9.1-rc5", tag: "v0.9.1-rc4").DescribeIncompleteCandidateIdentity());
    }
}
