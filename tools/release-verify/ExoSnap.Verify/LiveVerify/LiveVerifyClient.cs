using System.Collections.ObjectModel;
using System.Globalization;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace ExoSnap.Verify.LiveVerify;

/// <summary>Which endpoint of a run to attach to.</summary>
/// <remarks>
/// One run id reaches two processes: the application answers at
/// <see cref="Application"/>, and the updater it launches answers at
/// <see cref="Updater"/>. The child's endpoint name is derived from the id the
/// runner already holds, so a runner never discovers a pipe and never mints a
/// second credential.
/// </remarks>
public enum LiveVerifyRole
{
    /// <summary>The application's own endpoint.</summary>
    Application,

    /// <summary>The endpoint of the updater the application launched.</summary>
    Updater,
}

/// <summary>One response to a request.</summary>
/// <param name="Id">The request id this answers.</param>
/// <param name="Ok">Whether the command was accepted.</param>
/// <param name="Result">The result payload; an empty object on a refusal.</param>
/// <param name="ErrorCode">Structured refusal cause, or empty on success.</param>
/// <param name="ErrorMessage">Human-readable refusal cause, or empty on success.</param>
/// <param name="StateRevision">The revision the server was at, or -1 when it sent none.</param>
public sealed record LiveVerifyResponse(
    string Id,
    bool Ok,
    JsonElement Result,
    string ErrorCode,
    string ErrorMessage,
    long StateRevision);

/// <summary>One event the server pushed.</summary>
/// <param name="Name">The event name.</param>
/// <param name="Data">The event payload.</param>
/// <param name="StateRevision">The revision the server was at, or -1 when it sent none.</param>
public sealed record LiveVerifyEvent(string Name, JsonElement Data, long StateRevision);

/// <summary>The control channel refused, timed out, or answered something unreadable.</summary>
public sealed class LiveVerifyException : Exception
{
    /// <summary>Creates the exception with a default message.</summary>
    public LiveVerifyException()
        : base("The Live Verify control channel did not answer as its protocol requires.")
    {
    }

    /// <summary>Creates the exception with a message.</summary>
    public LiveVerifyException(string message)
        : base(message)
    {
    }

    /// <summary>Creates the exception with a message and a cause.</summary>
    public LiveVerifyException(string message, Exception? innerException)
        : base(message, innerException)
    {
    }
}

/// <summary>
/// Client for the ExoSnap Live Verify control channel.
/// </summary>
/// <remarks>
/// One JSON request per line over the local named pipe an ExoSnap process opened
/// with its run id. Protocol 2 stamps every response and every event with a state
/// revision, which is what lets a check assert a postcondition instead of waiting
/// a fixed time.
///
/// Deliberately absent: retries. A reconnect that happened quietly would turn "the
/// application restarted under us", the single most important thing an updater
/// check has to notice, into a green result. Reconnecting is the caller's explicit
/// decision.
///
/// Events that arrive while a response is outstanding are buffered rather than
/// discarded, so a caller that waits for an event caused by the command it just
/// sent cannot race its own request.
/// </remarks>
public sealed class LiveVerifyClient : IAsyncDisposable
{
    /// <summary>The protocol version this client speaks by default.</summary>
    public const int DefaultProtocol = 2;

    private readonly List<LiveVerifyEvent> events = [];
    private readonly Queue<string> lines = new();
    private readonly byte[] buffer = new byte[8192];
    private readonly StringBuilder partial = new();
    private readonly Decoder decoder = new UTF8Encoding(false).GetDecoder();
    private readonly NamedPipeClientStream pipe;

    private int nextId = 1;

    /// <summary>Creates a client for one run id and role.</summary>
    public LiveVerifyClient(string runId, LiveVerifyRole role = LiveVerifyRole.Application, int protocol = DefaultProtocol)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(runId);
        ArgumentOutOfRangeException.ThrowIfLessThan(protocol, 1);

        this.RunId = runId;
        this.Role = role;
        this.Protocol = protocol;
        this.PipeName = PipeNameFor(runId, role);
        this.pipe = new NamedPipeClientStream(
            ".",
            ShortPipeNameFor(runId, role),
            PipeDirection.InOut,
            PipeOptions.Asynchronous);
    }

    /// <summary>The run id, which doubles as the connection credential.</summary>
    public string RunId { get; }

    /// <summary>Which endpoint of the run this client attaches to.</summary>
    public LiveVerifyRole Role { get; }

    /// <summary>The protocol version every request on this connection carries.</summary>
    public int Protocol { get; }

    /// <summary>The full endpoint name, including the local pipe prefix.</summary>
    public string PipeName { get; }

    /// <summary>The newest state revision seen on a response or an event.</summary>
    public long StateRevision { get; private set; } = -1;

    /// <summary>What the server reported about itself during the handshake.</summary>
    public JsonElement Identity { get; private set; }

    /// <summary>Events received so far, oldest first.</summary>
    public ReadOnlyCollection<LiveVerifyEvent> Events => new([.. this.events]);

    /// <summary>The full endpoint name for a run id and role.</summary>
    public static string PipeNameFor(string runId, LiveVerifyRole role = LiveVerifyRole.Application)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(runId);
        return @"\\.\pipe\" + ShortPipeNameFor(runId, role);
    }

    /// <summary>The endpoint name without the local pipe prefix, as .NET wants it.</summary>
    public static string ShortPipeNameFor(string runId, LiveVerifyRole role = LiveVerifyRole.Application)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(runId);
        var roleName = role == LiveVerifyRole.Updater ? "Updater" : "LiveVerify";
        return string.Format(CultureInfo.InvariantCulture, "ExoSnap.{0}.{1}", roleName, runId);
    }

    /// <summary>
    /// A fresh run id. It is the connection credential as well as the name, so it
    /// is unguessable rather than a timestamp.
    /// </summary>
    public static string NewRunId() => "lv-" + Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture);

    /// <summary>Connects and performs the mandatory handshake.</summary>
    /// <exception cref="LiveVerifyException">
    /// The endpoint refused, answered a different protocol, or did not answer in
    /// time. The caller gets an authenticated connection or an error, never a
    /// half-open one.
    /// </exception>
    public async Task ConnectAsync(TimeSpan connectTimeout, TimeSpan requestTimeout, CancellationToken cancellationToken)
    {
        try
        {
            await this.pipe.ConnectAsync((int)connectTimeout.TotalMilliseconds, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (TimeoutException exception)
        {
            throw new LiveVerifyException(
                $"Could not connect to the Live Verify endpoint for run '{this.RunId}'.", exception);
        }
        catch (IOException exception)
        {
            throw new LiveVerifyException(
                $"Could not connect to the Live Verify endpoint for run '{this.RunId}'.", exception);
        }

        var hello = await this.RequestAsync(
            "system.hello",
            new Dictionary<string, object?> { ["runId"] = this.RunId },
            requestTimeout,
            cancellationToken).ConfigureAwait(false);

        if (!hello.Ok)
        {
            throw new LiveVerifyException($"Handshake refused: {hello.ErrorCode} - {hello.ErrorMessage}");
        }

        if (!hello.Result.TryGetProperty("protocol", out var protocol) ||
            protocol.ValueKind != JsonValueKind.Number ||
            protocol.GetInt32() != this.Protocol)
        {
            throw new LiveVerifyException(
                $"Protocol mismatch: the endpoint does not speak protocol {this.Protocol}.");
        }

        this.Identity = hello.Result.Clone();
    }

    /// <summary>Sends a command and waits for the matching response.</summary>
    /// <exception cref="LiveVerifyException">No response arrived within the timeout.</exception>
    public async Task<LiveVerifyResponse> RequestAsync(
        string command,
        IReadOnlyDictionary<string, object?>? parameters,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(command);

        var id = this.nextId.ToString(CultureInfo.InvariantCulture);
        this.nextId++;

        await this.SendAsync(id, command, parameters, cancellationToken).ConfigureAwait(false);

        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            var message = await this.ReadObjectAsync(deadline - DateTime.UtcNow, cancellationToken)
                .ConfigureAwait(false);
            if (message is null)
            {
                break;
            }

            using (message)
            {
                var root = message.RootElement;
                if (root.TryGetProperty("event", out var name) && name.ValueKind == JsonValueKind.String)
                {
                    this.events.Add(new LiveVerifyEvent(
                        name.GetString() ?? string.Empty,
                        root.TryGetProperty("data", out var data) ? data.Clone() : default,
                        Revision(root)));
                    continue;
                }

                if (!root.TryGetProperty("id", out var responseId) ||
                    !string.Equals(responseId.GetString(), id, StringComparison.Ordinal))
                {
                    continue;
                }

                return new LiveVerifyResponse(
                    id,
                    !root.TryGetProperty("ok", out var ok) || ok.ValueKind != JsonValueKind.False,
                    root.TryGetProperty("result", out var result) ? result.Clone() : default,
                    ErrorField(root, "code"),
                    ErrorField(root, "message"),
                    Revision(root));
            }
        }

        throw new LiveVerifyException(
            $"No response to '{command}' within {timeout.TotalMilliseconds.ToString("F0", CultureInfo.InvariantCulture)} ms.");
    }

    /// <inheritdoc/>
    public async ValueTask DisposeAsync()
    {
        await this.pipe.DisposeAsync().ConfigureAwait(false);
    }

    private static string ErrorField(JsonElement root, string field) =>
        root.TryGetProperty("error", out var error) &&
        error.ValueKind == JsonValueKind.Object &&
        error.TryGetProperty(field, out var value) &&
        value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;

    private static long Revision(JsonElement root) =>
        root.TryGetProperty("stateRevision", out var revision) && revision.ValueKind == JsonValueKind.Number
            ? revision.GetInt64()
            : -1;

    private async Task SendAsync(
        string id,
        string command,
        IReadOnlyDictionary<string, object?>? parameters,
        CancellationToken cancellationToken)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject();
            writer.WriteNumber("protocol", this.Protocol);
            writer.WriteString("id", id);
            writer.WriteString("command", command);
            writer.WritePropertyName("params");
            writer.WriteStartObject();
            foreach (var (key, value) in parameters ?? new Dictionary<string, object?>())
            {
                writer.WritePropertyName(key);
                WriteValue(writer, value);
            }

            writer.WriteEndObject();
            writer.WriteEndObject();
        }

        var payload = stream.ToArray();
        await this.pipe.WriteAsync(payload, cancellationToken).ConfigureAwait(false);
        await this.pipe.WriteAsync("\n"u8.ToArray(), cancellationToken).ConfigureAwait(false);
        await this.pipe.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    private static void WriteValue(Utf8JsonWriter writer, object? value)
    {
        switch (value)
        {
            case null:
                writer.WriteNullValue();
                break;
            case string text:
                writer.WriteStringValue(text);
                break;
            case bool flag:
                writer.WriteBooleanValue(flag);
                break;
            case int number:
                writer.WriteNumberValue(number);
                break;
            case long number:
                writer.WriteNumberValue(number);
                break;
            case double number:
                writer.WriteNumberValue(number);
                break;
            default:
                writer.WriteStringValue(Convert.ToString(value, CultureInfo.InvariantCulture));
                break;
        }
    }

    private async Task<JsonDocument?> ReadObjectAsync(TimeSpan timeout, CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (true)
        {
            if (this.lines.Count > 0)
            {
                var line = this.lines.Dequeue();
                JsonDocument document;
                try
                {
                    document = JsonDocument.Parse(line);
                }
                catch (JsonException)
                {
                    // A line that is not JSON is not a message. Skipping it keeps
                    // one malformed frame from ending an otherwise usable session.
                    continue;
                }

                this.StateRevision = Math.Max(this.StateRevision, Revision(document.RootElement));
                return document;
            }

            if (DateTime.UtcNow >= deadline || !this.pipe.IsConnected)
            {
                return null;
            }

            using var readDeadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            readDeadline.CancelAfter(deadline - DateTime.UtcNow);

            int read;
            try
            {
                read = await this.pipe.ReadAsync(this.buffer, readDeadline.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                return null;
            }

            if (read <= 0)
            {
                return null;
            }

            var chars = new char[this.decoder.GetCharCount(this.buffer, 0, read)];
            this.decoder.GetChars(this.buffer, 0, read, chars, 0);
            this.partial.Append(chars);
            this.DrainPartial();
        }
    }

    private void DrainPartial()
    {
        var text = this.partial.ToString();
        var index = text.IndexOf('\n', StringComparison.Ordinal);
        while (index >= 0)
        {
            var line = text[..index].TrimEnd('\r');
            if (line.Trim().Length > 0)
            {
                this.lines.Enqueue(line);
            }

            text = text[(index + 1)..];
            index = text.IndexOf('\n', StringComparison.Ordinal);
        }

        this.partial.Clear();
        this.partial.Append(text);
    }
}
