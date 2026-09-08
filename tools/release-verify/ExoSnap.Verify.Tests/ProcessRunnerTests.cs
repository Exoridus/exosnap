using System.Diagnostics;
using System.Globalization;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The hostile-input contract for the process runner.
/// </summary>
/// <remarks>
/// Each case here is a way a release gate has been made to report the wrong thing
/// by a tool, a path, or a locale rather than by the product. A runner that has
/// not been shown to survive them cannot be trusted to say a release is broken.
/// </remarks>
public sealed class ProcessRunnerTests
{
    private static readonly TimeSpan ShortTimeout = TimeSpan.FromSeconds(30);

    [Fact]
    public async Task WritesStandardOutputAndStandardErrorSeparately()
    {
        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--stdout", "on-out", "--stderr", "on-err")
            {
                Timeout = ShortTimeout,
            },
            TestContext.Current.CancellationToken);

        Assert.True(result.Succeeded);
        Assert.Contains("on-out", result.StandardOutput, StringComparison.Ordinal);
        Assert.Contains("on-err", result.StandardError, StringComparison.Ordinal);
        Assert.DoesNotContain("on-err", result.StandardOutput, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ReportsNonZeroExitCodeWithoutThrowing()
    {
        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--exit", "7") { Timeout = ShortTimeout },
            TestContext.Current.CancellationToken);

        Assert.Equal(7, result.ExitCode);
        Assert.False(result.Succeeded);
        Assert.False(result.TimedOut);
    }

    [Fact]
    public async Task ToolThatExitsWithoutOutputIsAContractViolationNotAFailure()
    {
        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path) { Timeout = ShortTimeout },
            TestContext.Current.CancellationToken);

        Assert.True(result.Succeeded);
        Assert.Equal(string.Empty, result.StandardOutput.Trim());

        var thrown = Assert.Throws<ToolContractException>(() => ToolContract.ParseJson(result, "fixture"));
        Assert.Contains("without writing any output", thrown.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task MalformedJsonIsAContractViolationNotAFailure()
    {
        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--bad-json") { Timeout = ShortTimeout },
            TestContext.Current.CancellationToken);

        var thrown = Assert.Throws<ToolContractException>(() => ToolContract.ParseJson(result, "fixture"));
        Assert.Contains("not valid JSON", thrown.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task WellFormedJsonParses()
    {
        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--json") { Timeout = ShortTimeout },
            TestContext.Current.CancellationToken);

        using var document = ToolContract.ParseJson(result, "fixture");
        Assert.Equal(42, document.RootElement.GetProperty("value").GetInt32());
    }

    [Fact]
    public async Task ToolThatHangsIsKilledAndReportedAsTimedOut()
    {
        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--sleep", "60000")
            {
                Timeout = TimeSpan.FromMilliseconds(750),
            },
            TestContext.Current.CancellationToken);

        Assert.True(result.TimedOut);
        Assert.False(result.Succeeded);
        Assert.True(result.Duration < TimeSpan.FromSeconds(20));
    }

    [Fact]
    public async Task ToolFloodingStandardErrorDoesNotDeadlock()
    {
        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--flood-stderr", "400", "--stdout", "done")
            {
                Timeout = ShortTimeout,
            },
            TestContext.Current.CancellationToken);

        Assert.True(result.Succeeded);
        Assert.Contains("done", result.StandardOutput, StringComparison.Ordinal);
        Assert.True(result.StandardError.Length > 100_000);
    }

    [Theory]
    [InlineData(" with spaces")]
    [InlineData(" with & ampersand")]
    [InlineData(" with (parens) and 'apostrophe'")]
    [InlineData(" mit Umlauten und Zeichen")]
    public async Task RunsFromAHostilePath(string suffix)
    {
        using var directory = FixtureTool.NewTemporaryDirectory(suffix);
        var tool = FixtureTool.CopyTo(directory.Path);

        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(tool, "--stdout", "reached") { Timeout = ShortTimeout },
            TestContext.Current.CancellationToken);

        Assert.True(result.Succeeded, result.StandardError);
        Assert.Contains("reached", result.StandardOutput, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ArgumentsReachTheChildUnaltered()
    {
        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--print-args", "a b", "c&d", "e\"f", "g;h")
            {
                Timeout = ShortTimeout,
            },
            TestContext.Current.CancellationToken);

        var lines = result.StandardOutput.Split('\n').Select(line => line.TrimEnd('\r')).ToList();
        Assert.Contains("a b", lines);
        Assert.Contains("c&d", lines);
        Assert.Contains("e\"f", lines);
        Assert.Contains("g;h", lines);
    }

    [Fact]
    public async Task EnvironmentOverridesReachTheChild()
    {
        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--print-env", "EXOSNAP_VERIFY_TEST")
            {
                Timeout = ShortTimeout,
                Environment = new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase)
                {
                    ["EXOSNAP_VERIFY_TEST"] = "present",
                },
            },
            TestContext.Current.CancellationToken);

        Assert.Contains("present", result.StandardOutput, StringComparison.Ordinal);
    }

    [Fact]
    public async Task StandardInputIsWrittenAndClosed()
    {
        using var runner = new ProcessRunner();
        var result = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--echo-stdin")
            {
                Timeout = ShortTimeout,
                StandardInput = "hello",
            },
            TestContext.Current.CancellationToken);

        Assert.Contains("hello", result.StandardOutput, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ADecimalCommaIsRefusedRatherThanMisread()
    {
        using var runner = new ProcessRunner();
        var plain = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--decimal", "de-DE") { Timeout = ShortTimeout },
            TestContext.Current.CancellationToken);

        Assert.Contains("3,5", plain.StandardOutput, StringComparison.Ordinal);
        Assert.False(double.TryParse(
            plain.StandardOutput.Trim(),
            NumberStyles.Float,
            CultureInfo.InvariantCulture,
            out _));

        var asJson = await runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--decimal-json", "de-DE") { Timeout = ShortTimeout },
            TestContext.Current.CancellationToken);

        Assert.Throws<ToolContractException>(() => ToolContract.ParseJson(asJson, "fixture"));
    }

    [Fact]
    public async Task MissingExecutableThrowsAStartFailureRatherThanReturningAnExitCode()
    {
        using var runner = new ProcessRunner();
        var missing = Path.Combine(AppContext.BaseDirectory, "no-such-tool-9f3a.exe");

        await Assert.ThrowsAsync<ProcessStartFailedException>(() => runner.RunAsync(
            new ProcessRunRequest(missing) { Timeout = ShortTimeout },
            TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task CancellationPropagatesRatherThanReportingATimeout()
    {
        using var runner = new ProcessRunner();
        using var cancellation = new CancellationTokenSource(TimeSpan.FromMilliseconds(400));

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => runner.RunAsync(
            new ProcessRunRequest(FixtureTool.Path, "--sleep", "60000") { Timeout = TimeSpan.FromMinutes(5) },
            cancellation.Token));
    }

    [Fact]
    public async Task AGrandchildDoesNotSurviveTheRunner()
    {
        int grandchildId;
        var runner = new ProcessRunner();
        try
        {
            Assert.True(runner.ChildContainmentActive, "the platform refused to create a job object");

            var result = await runner.RunAsync(
                new ProcessRunRequest(FixtureTool.Path, "--spawn-child", "60000") { Timeout = ShortTimeout },
                TestContext.Current.CancellationToken);

            Assert.True(result.Succeeded, result.StandardError);
            grandchildId = int.Parse(result.StandardOutput.Trim(), CultureInfo.InvariantCulture);
            Assert.True(IsAlive(grandchildId), "the fixture did not leave a grandchild running");
        }
        finally
        {
            runner.Dispose();
        }

        // Closing the job handle terminates everything in it. The poll is bounded
        // because the kill is asynchronous, not because it is uncertain.
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
        while (IsAlive(grandchildId) && DateTime.UtcNow < deadline)
        {
            await Task.Delay(50, TestContext.Current.CancellationToken);
        }

        Assert.False(IsAlive(grandchildId), "the grandchild outlived the job object");
    }

    private static bool IsAlive(int processId)
    {
        try
        {
            using var process = Process.GetProcessById(processId);
            return !process.HasExited;
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
    }
}
