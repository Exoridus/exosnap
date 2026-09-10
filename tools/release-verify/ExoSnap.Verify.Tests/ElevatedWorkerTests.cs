using ExoSnap.Verify.Adapters.Elevation;
using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The elevated-worker contract: the argument list, the result document, and the
/// parent's wait for a result file. None of this elevates anything.
/// </summary>
public sealed class ElevatedWorkerContractTests
{
    [Fact]
    public void BuildArgumentsAndTryParseRoundTrip()
    {
        var arguments = ElevatedWorker.BuildArguments(
            "REL-PRESENT-002", @"C:\runs\r1\elevated-worker-result.json", @"C:\app\exosnap.exe", selfTest: true);

        var request = ElevatedWorker.TryParse(arguments, out var error);

        Assert.Equal(string.Empty, error);
        Assert.NotNull(request);
        Assert.Equal("REL-PRESENT-002", request!.TaskId);
        Assert.Equal(@"C:\runs\r1\elevated-worker-result.json", request.ResultPath);
        Assert.Equal(@"C:\app\exosnap.exe", request.TargetExe);
        Assert.True(request.SelfTest);
    }

    [Fact]
    public void TryParseWithoutATargetLeavesItEmpty()
    {
        var request = ElevatedWorker.TryParse(
            ElevatedWorker.BuildArguments("REL-PRESENT-002", @"C:\r\out.json", targetExe: string.Empty), out _);

        Assert.NotNull(request);
        Assert.Equal(string.Empty, request!.TargetExe);
        Assert.False(request.SelfTest);
    }

    [Fact]
    public void TryParseRejectsAnArgumentListWithNoResultPath()
    {
        var request = ElevatedWorker.TryParse(["elevated-worker", "REL-PRESENT-002", "--target", "x"], out var error);

        Assert.Null(request);
        Assert.Contains("--result", error, StringComparison.Ordinal);
    }

    [Fact]
    public void TryParseRejectsAnUnknownArgument()
    {
        var request = ElevatedWorker.TryParse(
            ["elevated-worker", "REL-PRESENT-002", "--result", "out.json", "--boom"], out var error);

        Assert.Null(request);
        Assert.Contains("--boom", error, StringComparison.Ordinal);
    }

    [Fact]
    public void ResultDocumentRoundTripsThroughTheFile()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-worker-result");
        var path = Path.Combine(directory.Path, ElevatedWorkerResult.FileName);
        var written = ElevatedWorkerResult.For(
            "REL-PRESENT-002",
            ElevatedWorkerOutcome.Pass,
            "elevated present diagnostics decoded 2400 present(s), mode independentFlip",
            elevated: true,
            presentCount: 2400,
            presentMode: "independentFlip",
            tearingAllowed: false,
            oracleNote: "xcheck is a different gate");

        written.Write(path);
        var read = ElevatedWorkerResult.Read(path);

        Assert.NotNull(read);
        Assert.Equal(ElevatedWorkerOutcome.Pass, read!.Outcome);
        Assert.Equal(2400, read.PresentCount);
        Assert.Equal("independentFlip", read.PresentMode);
        Assert.False(read.TearingAllowed);
        Assert.True(read.Elevated);
    }

    [Fact]
    public void ReadReturnsNullForAnAbsentFile() =>
        Assert.Null(ElevatedWorkerResult.Read(Path.Combine(Path.GetTempPath(), "no-such-worker-result.json")));

    [Fact]
    public void ReadReturnsNullForAMalformedDocument()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-worker-garbage");
        var path = Path.Combine(directory.Path, "result.json");
        File.WriteAllText(path, "{ this is not json");

        Assert.Null(ElevatedWorkerResult.Read(path));
    }
}

/// <summary>The parent's wait for a result file, exercised without a worker process.</summary>
public sealed class ElevatedWorkerWaitTests
{
    [Fact]
    public async Task CompletesWhenTheResultFileAppearsWhileTheWorkerIsStillRunning()
    {
        var cancellationToken = TestContext.Current.CancellationToken;
        using var directory = FixtureTool.NewTemporaryDirectory("-worker-wait");
        var path = Path.Combine(directory.Path, ElevatedWorkerResult.FileName);

        var writer = Task.Run(
            async () =>
            {
                await Task.Delay(300, cancellationToken);
                ElevatedWorkerResult.For(
                        "REL-PRESENT-002", ElevatedWorkerOutcome.Pass, "decoded presents", elevated: true)
                    .Write(path);
            },
            cancellationToken);

        var run = await ElevatedWorkerHost.AwaitResultAsync(
            path, workerExited: () => false, TimeSpan.FromSeconds(10), cancellationToken);
        await writer;

        Assert.Equal(ElevatedWorkerRunKind.Completed, run.Kind);
        Assert.Equal(ElevatedWorkerOutcome.Pass, run.Result!.Outcome);
    }

    [Fact]
    public async Task FaultsWhenTheWorkerExitsWithoutWritingAResult()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-worker-exit");
        var path = Path.Combine(directory.Path, ElevatedWorkerResult.FileName);

        var run = await ElevatedWorkerHost.AwaitResultAsync(
            path, workerExited: () => true, TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken);

        Assert.Equal(ElevatedWorkerRunKind.Faulted, run.Kind);
        Assert.Contains("without writing", run.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FaultsWhenNoResultIsWrittenWithinTheDeadline()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-worker-timeout");
        var path = Path.Combine(directory.Path, ElevatedWorkerResult.FileName);

        var run = await ElevatedWorkerHost.AwaitResultAsync(
            path, workerExited: () => false, TimeSpan.FromMilliseconds(400), TestContext.Current.CancellationToken);

        Assert.Equal(ElevatedWorkerRunKind.Faulted, run.Kind);
        Assert.Contains("did not write a result", run.Detail, StringComparison.Ordinal);
    }
}
