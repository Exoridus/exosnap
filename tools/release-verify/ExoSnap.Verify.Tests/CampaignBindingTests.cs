using ExoSnap.Verify.Catalog;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Json;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

public sealed class CampaignBindingTests
{
    [Theory]
    [InlineData("artifact")]
    [InlineData("state")]
    [InlineData("catalog")]
    public void ChangedCampaignInputsCannotReusePreparedVerdicts(string changed)
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-campaign-binding");
        var executable = Path.Combine(directory.Path, "candidate.exe");
        File.WriteAllText(executable, "candidate bytes");
        var run = RunDirectory.Open(Path.Combine(directory.Path, "run"));
        var catalog = ReleaseCatalog.Create();
        var binding = Campaign.Bind("run", "v0.9.1-rc1", "commit", executable);
        var campaign = new CampaignDocument("1", binding, [], directory.Path);
        Campaign.WriteDocument(run, campaign);
        var state = new RunState(RunState.CurrentSchemaVersion, "run", binding.RcTag, binding.SourceCommit,
            Qualification.ArtifactFingerprint([new ArtifactDigest("candidate.exe", binding.ExecutableSha256)]),
            catalog.Version, DateTimeOffset.UtcNow, []);
        run.WriteState(state);
        var catalogPath = Path.Combine(run.Root, Campaign.CatalogFileName);
        VerifyJson.WriteFile(catalogPath, catalog.ToDocument(), VerifyJsonContext.Default.ScenarioDescriptorDocument);
        Assert.Empty(Campaign.ReconciliationBlockers(run, campaign, catalog));

        switch (changed)
        {
            case "artifact":
                File.WriteAllText(executable, "replacement bytes");
                break;
            case "state":
                run.WriteState(state with { SourceCommitSha = "different" });
                break;
            case "catalog":
                File.Delete(catalogPath);
                break;
        }

        Assert.NotEmpty(Campaign.ReconciliationBlockers(run, campaign, catalog));
        var qualificationPath = Path.Combine(run.Root, ReleaseVerificationRecord.FileName);
        File.WriteAllText(qualificationPath, "previous qualification");
        Assert.NotEqual(0, ExoSnap.Verify.Program.Main(["qualify", "--run-dir", run.Root]));
        Assert.False(File.Exists(qualificationPath));
    }
}
