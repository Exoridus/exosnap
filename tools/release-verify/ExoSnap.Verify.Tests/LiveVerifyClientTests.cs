using System.Globalization;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using ExoSnap.Verify.LiveVerify;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The control-channel contract, exercised against a fake server.
/// </summary>
/// <remarks>
/// No application is ever started here. The point is the protocol: the endpoint
/// name, the handshake, request and response correlation, and the rule that an
/// event arriving while a response is outstanding is kept rather than dropped.
/// </remarks>
public sealed class LiveVerifyClientTests
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public void TheEndpointNameIsDerivedFromTheRunIdAndTheRole()
    {
        Assert.Equal(
            @"\\.\pipe\ExoSnap.LiveVerify.lv-abc",
            LiveVerifyClient.PipeNameFor("lv-abc"));
        Assert.Equal(
            @"\\.\pipe\ExoSnap.Updater.lv-abc",
            LiveVerifyClient.PipeNameFor("lv-abc", LiveVerifyRole.Updater));
        Assert.Equal("ExoSnap.LiveVerify.lv-abc", LiveVerifyClient.ShortPipeNameFor("lv-abc"));
    }

    [Fact]
    public void ARunIdIsUnguessableRatherThanASequence()
    {
        var first = LiveVerifyClient.NewRunId();
        var second = LiveVerifyClient.NewRunId();

        Assert.StartsWith("lv-", first, StringComparison.Ordinal);
        Assert.Equal(35, first.Length);
        Assert.NotEqual(first, second);
    }

    [Fact]
    public async Task TheHandshakeAuthenticatesWithTheRunIdAndPinsTheProtocol()
    {
        var runId = LiveVerifyClient.NewRunId();
        var received = new List<JsonDocument>();
        using var server = new FakeServer(runId, received);
        var serving = server.ServeAsync(TestContext.Current.CancellationToken);

        await using var client = new LiveVerifyClient(runId);
        await client.ConnectAsync(Timeout, Timeout, TestContext.Current.CancellationToken);

        var response = await client.RequestAsync(
            "state.get",
            new Dictionary<string, object?> { ["scope"] = "record" },
            Timeout,
            TestContext.Current.CancellationToken);

        Assert.True(response.Ok);
        Assert.Equal(7, client.StateRevision);
        Assert.Equal(2, client.Identity.GetProperty("protocol").GetInt32());

        await serving;

        Assert.Equal("system.hello", received[0].RootElement.GetProperty("command").GetString());
        Assert.Equal(runId, received[0].RootElement.GetProperty("params").GetProperty("runId").GetString());
        Assert.Equal(2, received[0].RootElement.GetProperty("protocol").GetInt32());
        Assert.Equal("state.get", received[1].RootElement.GetProperty("command").GetString());
        Assert.Equal("record", received[1].RootElement.GetProperty("params").GetProperty("scope").GetString());

        foreach (var document in received)
        {
            document.Dispose();
        }
    }

    [Fact]
    public async Task AProtocolMismatchIsRefusedRatherThanTolerated()
    {
        var runId = LiveVerifyClient.NewRunId();
        using var server = new FakeServer(runId, [], helloProtocol: 1);
        var serving = server.ServeAsync(TestContext.Current.CancellationToken);

        await using var client = new LiveVerifyClient(runId);
        var thrown = await Assert.ThrowsAsync<LiveVerifyException>(() =>
            client.ConnectAsync(Timeout, Timeout, TestContext.Current.CancellationToken));

        Assert.Contains("Protocol mismatch", thrown.Message, StringComparison.Ordinal);
        await serving;
    }

    [Fact]
    public async Task ARefusedHandshakeThrowsRatherThanReturningAHalfOpenConnection()
    {
        var runId = LiveVerifyClient.NewRunId();
        using var server = new FakeServer(runId, [], refuseHello: true);
        var serving = server.ServeAsync(TestContext.Current.CancellationToken);

        await using var client = new LiveVerifyClient(runId);
        var thrown = await Assert.ThrowsAsync<LiveVerifyException>(() =>
            client.ConnectAsync(Timeout, Timeout, TestContext.Current.CancellationToken));

        Assert.Contains("Handshake refused", thrown.Message, StringComparison.Ordinal);
        Assert.Contains("unknown-run", thrown.Message, StringComparison.Ordinal);
        await serving;
    }

    [Fact]
    public async Task AnEventArrivingBeforeAResponseIsKeptNotDiscarded()
    {
        var runId = LiveVerifyClient.NewRunId();
        using var server = new FakeServer(runId, [], emitEventBeforeResponse: true);
        var serving = server.ServeAsync(TestContext.Current.CancellationToken);

        await using var client = new LiveVerifyClient(runId);
        await client.ConnectAsync(Timeout, Timeout, TestContext.Current.CancellationToken);

        var response = await client.RequestAsync(
            "record.start", null, Timeout, TestContext.Current.CancellationToken);

        Assert.True(response.Ok);
        Assert.Single(client.Events);
        Assert.Equal("record.stateChanged", client.Events[0].Name);
        Assert.Equal("recording", client.Events[0].Data.GetProperty("state").GetString());
        Assert.Equal(7, client.StateRevision);

        await serving;
    }

    [Fact]
    public async Task NoResponseWithinTheTimeoutIsReportedRatherThanWaitedOutForever()
    {
        var runId = LiveVerifyClient.NewRunId();
        using var server = new FakeServer(runId, [], answerRequests: false);
        var serving = server.ServeAsync(TestContext.Current.CancellationToken);

        await using var client = new LiveVerifyClient(runId);
        await client.ConnectAsync(Timeout, Timeout, TestContext.Current.CancellationToken);

        await Assert.ThrowsAsync<LiveVerifyException>(() => client.RequestAsync(
            "record.start", null, TimeSpan.FromMilliseconds(400), TestContext.Current.CancellationToken));

        await serving;
    }

    /// <summary>
    /// A one-connection server that speaks the protocol the client expects, and
    /// can be told to speak it badly.
    /// </summary>
    private sealed class FakeServer(
        string runId,
        List<JsonDocument> received,
        int helloProtocol = 2,
        bool refuseHello = false,
        bool emitEventBeforeResponse = false,
        bool answerRequests = true) : IDisposable
    {
        private readonly NamedPipeServerStream pipe = new(
            LiveVerifyClient.ShortPipeNameFor(runId),
            PipeDirection.InOut,
            1,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);

        public async Task ServeAsync(CancellationToken cancellationToken)
        {
            await Task.Yield();
            await this.pipe.WaitForConnectionAsync(cancellationToken);

            using var reader = new StreamReader(this.pipe, Encoding.UTF8, false, 4096, leaveOpen: true);
            using var writer = new StreamWriter(this.pipe, new UTF8Encoding(false), 4096, leaveOpen: true)
            {
                AutoFlush = true,
                NewLine = "\n",
            };

            var handled = 0;
            while (await reader.ReadLineAsync(cancellationToken) is { } line)
            {
                var request = JsonDocument.Parse(line);
                received.Add(request);
                var id = request.RootElement.GetProperty("id").GetString();
                var command = request.RootElement.GetProperty("command").GetString();

                if (command == "system.hello")
                {
                    await writer.WriteLineAsync(refuseHello
                        ? $"{{\"id\":\"{id}\",\"ok\":false," +
                          "\"error\":{\"code\":\"unknown-run\",\"message\":\"no such run\"}}"
                        : $"{{\"id\":\"{id}\",\"ok\":true,\"result\":{{\"protocol\":" +
                          helloProtocol.ToString(CultureInfo.InvariantCulture) +
                          ",\"pid\":1234},\"stateRevision\":3}");

                    if (refuseHello || helloProtocol != 2)
                    {
                        return;
                    }

                    continue;
                }

                if (!answerRequests)
                {
                    // Two requests is the whole conversation this server has: the
                    // handshake, then the one the client will time out on.
                    return;
                }

                if (emitEventBeforeResponse)
                {
                    await writer.WriteLineAsync(
                        "{\"event\":\"record.stateChanged\",\"data\":{\"state\":\"recording\"},\"stateRevision\":6}");
                }

                await writer.WriteLineAsync(
                    $"{{\"id\":\"{id}\",\"ok\":true,\"result\":{{\"acknowledged\":true}},\"stateRevision\":7}}");

                handled++;
                if (handled >= 1)
                {
                    return;
                }
            }
        }

        public void Dispose() => this.pipe.Dispose();
    }
}
