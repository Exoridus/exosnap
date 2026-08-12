// Transport-level coverage for the Live Verify control server.
//
// The pure dispatcher tests next door prove what the server ANSWERS; these prove
// the endpoint itself: that a normal launch creates none, that the one it does
// create is a local named pipe and not a socket, that a malformed or hostile
// client cannot take the application down with it, that events reach a connected
// client, and that a second connection starts unauthenticated.
//
// A real pipe is used rather than a seam. The properties under test -- "no TCP
// listener", "a broken client does not crash the host", "the handshake is per
// connection" -- are properties of the transport, and a mocked transport would
// assert them against the mock.

#include "live_verify/LiveVerifyControlServer.h"
#include "live_verify/LiveVerifyOptions.h"
#include "live_verify/LiveVerifyProtocol.h"
#include "live_verify/LiveVerifySource.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

using namespace exosnap::live_verify;

namespace {

QCoreApplication& App() {
    static int argc = 1;
    static char arg0[] = "live_verify_server_tests";
    static char* argv[] = {arg0, nullptr};
    static QCoreApplication application(argc, argv);
    return application;
}

class StubSource final : public LiveVerifySource {
  public:
    std::atomic<int> record_starts{0};

    [[nodiscard]] QJsonObject Identity() const override {
        return QJsonObject{{QStringLiteral("productVersion"), QStringLiteral("0.9.0-test")},
                           {QStringLiteral("commit"), QStringLiteral("deadbeef")}};
    }
    [[nodiscard]] QJsonObject SystemSnapshot() const override {
        return {};
    }
    [[nodiscard]] QJsonObject AppSnapshot() const override {
        return {};
    }
    [[nodiscard]] QJsonObject WindowSnapshot() const override {
        return {};
    }
    [[nodiscard]] QJsonObject PreviewSnapshot() const override {
        return {};
    }
    [[nodiscard]] QJsonObject RecordSnapshot() const override {
        return QJsonObject{{QStringLiteral("stateText"), QStringLiteral("Ready")}};
    }
    [[nodiscard]] QJsonObject RecordResult() const override {
        return {};
    }
    [[nodiscard]] QJsonObject OverlaySnapshot() const override {
        return {};
    }
    [[nodiscard]] QJsonObject EditorSnapshot() const override {
        return {};
    }
    [[nodiscard]] QJsonObject DiagnosticsSnapshot() const override {
        return {};
    }
    bool MoveWindowToScreen(const QString&, QString*) override {
        return true;
    }
    bool SelectRecordTarget(const QString&, const QString&, QString*) override {
        return true;
    }
    bool RecordStart(QString*) override {
        ++record_starts;
        return true;
    }
    bool RecordPause(QString*) override {
        return true;
    }
    bool RecordResume(QString*) override {
        return true;
    }
    bool RecordStop(QString*) override {
        return true;
    }
    bool RecordSplit(QString*) override {
        return true;
    }
    bool RecordCaptureFrame(QString*) override {
        return true;
    }
};

// Synchronous client with a bounded reader. PeekNamedPipe before every ReadFile
// so a server that never answers fails the test on a deadline instead of wedging
// the worker thread and, with it, the whole CTest entry.
class PipeClient {
  public:
    ~PipeClient() {
        Close();
    }

    [[nodiscard]] bool Connect(const QString& pipe_name, int timeout_ms = 5000) {
        const std::wstring name = pipe_name.toStdWString();
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
        while (GetTickCount64() < deadline) {
            handle_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE)
                return true;
            if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND)
                return false;
            Sleep(10);
        }
        return false;
    }

    void Close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    [[nodiscard]] bool WriteRaw(const QByteArray& payload) {
        DWORD written = 0;
        return WriteFile(handle_, payload.constData(), static_cast<DWORD>(payload.size()), &written, nullptr) != FALSE;
    }

    [[nodiscard]] bool WriteRequest(const QString& command, const QJsonObject& params, const QString& id) {
        QJsonObject request;
        request.insert(QStringLiteral("protocol"), kProtocolVersion);
        request.insert(QStringLiteral("id"), id);
        request.insert(QStringLiteral("command"), command);
        request.insert(QStringLiteral("params"), params);
        return WriteRaw(SerializeLine(request));
    }

    // Returns a parsed line, or an empty object on timeout.
    [[nodiscard]] QJsonObject ReadObject(int timeout_ms = 8000) {
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
        while (GetTickCount64() < deadline) {
            const int newline = buffer_.indexOf('\n');
            if (newline >= 0) {
                const QByteArray line = buffer_.left(newline);
                buffer_.remove(0, newline + 1);
                return QJsonDocument::fromJson(line).object();
            }
            DWORD available = 0;
            if (PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr) == FALSE)
                return {};
            if (available == 0) {
                Sleep(5);
                continue;
            }
            QByteArray chunk(static_cast<int>(available), Qt::Uninitialized);
            DWORD read = 0;
            if (ReadFile(handle_, chunk.data(), available, &read, nullptr) == FALSE)
                return {};
            buffer_.append(chunk.left(static_cast<int>(read)));
        }
        return {};
    }

    // Skips events until a response with the given id arrives.
    [[nodiscard]] QJsonObject ReadResponse(const QString& id, int timeout_ms = 8000) {
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
        while (GetTickCount64() < deadline) {
            const QJsonObject object = ReadObject(static_cast<int>(deadline - GetTickCount64()));
            if (object.isEmpty())
                return {};
            if (object.value(QStringLiteral("id")).toString() == id)
                return object;
        }
        return {};
    }

    [[nodiscard]] bool Disconnected() const {
        DWORD available = 0;
        return PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr) == FALSE;
    }

  private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    QByteArray buffer_;
};

QString MakeRunId() {
    static std::atomic<int> counter{0};
    return QStringLiteral("test-run-%1-%2")
        .arg(static_cast<int>(GetCurrentProcessId()))
        .arg(counter.fetch_add(1), 4, 10, QLatin1Char('0'));
}

// Runs `body` on a worker thread while this thread pumps the Qt event loop --
// the server hands every request to the thread that owns the QObject, so a test
// that did not pump would deadlock on its own first command.
void WithClient(LiveVerifyControlServer& server, const std::function<void(PipeClient&)>& body, int timeout_ms = 30000) {
    std::atomic<bool> finished{false};
    std::thread worker([&]() {
        PipeClient client;
        if (client.Connect(server.pipeName()))
            body(client);
        else
            ADD_FAILURE() << "Could not connect to " << server.pipeName().toStdString();
        finished.store(true);
    });

    QElapsedTimer clock;
    clock.start();
    while (!finished.load() && clock.elapsed() < timeout_ms)
        App().processEvents(QEventLoop::AllEvents, 5);
    // One last drain so a queued dispatch posted just before the worker finished
    // cannot outlive this call and run against a destroyed fixture.
    App().processEvents(QEventLoop::AllEvents, 5);
    worker.join();
    ASSERT_TRUE(finished.load()) << "Client body did not finish within the deadline";
}

struct ServerFixture {
    StubSource source;
    QString run_id = MakeRunId();
    std::unique_ptr<LiveVerifyControlServer> server;

    ServerFixture() {
        App();
        server = std::make_unique<LiveVerifyControlServer>(&source, run_id);
    }
};

QJsonObject Hello(PipeClient& client, const QString& run_id) {
    EXPECT_TRUE(client.WriteRequest(QStringLiteral("system.hello"), QJsonObject{{QStringLiteral("runId"), run_id}},
                                    QStringLiteral("h")));
    return client.ReadResponse(QStringLiteral("h"));
}

} // namespace

TEST(LiveVerifyServer, NoEndpointExistsUntilTheServerIsStarted) {
    // The production equivalent of a normal launch: no server object is ever
    // constructed, so nothing can be opened. Proven here by asking for the name
    // a running server WOULD have and finding nothing there.
    const QString name = PipeNameForRunId(MakeRunId());
    const std::wstring wide = name.toStdWString();
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    EXPECT_EQ(handle, INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_FILE_NOT_FOUND));
}

TEST(LiveVerifyServer, EndpointIsALocalNamedPipeAndNothingElse) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();
    EXPECT_TRUE(fixture.server->pipeName().startsWith(QStringLiteral("\\\\.\\pipe\\ExoSnap.LiveVerify.")));
    EXPECT_TRUE(fixture.server->running());
    fixture.server->Stop();
    EXPECT_FALSE(fixture.server->running());
}

TEST(LiveVerifyServer, StoppedServerNoLongerAcceptsConnections) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();
    const QString name = fixture.server->pipeName();
    fixture.server->Stop();

    const std::wstring wide = name.toStdWString();
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    EXPECT_EQ(handle, INVALID_HANDLE_VALUE);
}

TEST(LiveVerifyServer, HandshakeThenCommandRoundTrips) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();

    WithClient(*fixture.server, [&](PipeClient& client) {
        const QJsonObject hello = Hello(client, fixture.run_id);
        ASSERT_TRUE(hello.value(QStringLiteral("ok")).toBool()) << QJsonDocument(hello).toJson().toStdString();
        EXPECT_EQ(hello.value(QStringLiteral("result")).toObject().value(QStringLiteral("commit")).toString(),
                  QStringLiteral("deadbeef"));

        ASSERT_TRUE(client.WriteRequest(QStringLiteral("record.start"), {}, QStringLiteral("1")));
        const QJsonObject response = client.ReadResponse(QStringLiteral("1"));
        EXPECT_TRUE(response.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("stateText")).toString(),
                  QStringLiteral("Ready"));
    });
    EXPECT_EQ(fixture.source.record_starts.load(), 1);
}

TEST(LiveVerifyServer, MalformedInputIsAnsweredAndTheApplicationSurvives) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();

    WithClient(*fixture.server, [&](PipeClient& client) {
        ASSERT_TRUE(Hello(client, fixture.run_id).value(QStringLiteral("ok")).toBool());

        ASSERT_TRUE(client.WriteRaw(QByteArray("{ this is not json\n")));
        const QJsonObject malformed = client.ReadObject();
        EXPECT_FALSE(malformed.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(malformed.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(),
                  QString::fromLatin1(error_code::kMalformedRequest));

        // Still alive and still authenticated afterwards.
        ASSERT_TRUE(client.WriteRequest(QStringLiteral("record.snapshot"), {}, QStringLiteral("2")));
        EXPECT_TRUE(client.ReadResponse(QStringLiteral("2")).value(QStringLiteral("ok")).toBool());
    });
    EXPECT_TRUE(fixture.server->running());
}

TEST(LiveVerifyServer, UnknownCommandFailsClosedWithoutClosingTheConnection) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();

    WithClient(*fixture.server, [&](PipeClient& client) {
        ASSERT_TRUE(Hello(client, fixture.run_id).value(QStringLiteral("ok")).toBool());
        ASSERT_TRUE(client.WriteRequest(QStringLiteral("shell.exec"),
                                        QJsonObject{{QStringLiteral("cmd"), QStringLiteral("calc.exe")}},
                                        QStringLiteral("3")));
        const QJsonObject response = client.ReadResponse(QStringLiteral("3"));
        EXPECT_EQ(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(),
                  QString::fromLatin1(error_code::kUnknownCommand));

        ASSERT_TRUE(client.WriteRequest(QStringLiteral("record.snapshot"), {}, QStringLiteral("4")));
        EXPECT_TRUE(client.ReadResponse(QStringLiteral("4")).value(QStringLiteral("ok")).toBool());
    });
}

TEST(LiveVerifyServer, CommandsBeforeTheHandshakeAreRefused) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();

    WithClient(*fixture.server, [&](PipeClient& client) {
        ASSERT_TRUE(client.WriteRequest(QStringLiteral("record.start"), {}, QStringLiteral("5")));
        const QJsonObject response = client.ReadResponse(QStringLiteral("5"));
        EXPECT_EQ(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(),
                  QString::fromLatin1(error_code::kHandshakeRequired));
    });
    EXPECT_EQ(fixture.source.record_starts.load(), 0);
}

TEST(LiveVerifyServer, WrongRunIdIsRejectedAndTheConnectionIsDropped) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();

    WithClient(*fixture.server, [&](PipeClient& client) {
        const QJsonObject response = Hello(client, QStringLiteral("not-the-right-run-id"));
        EXPECT_EQ(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(),
                  QString::fromLatin1(error_code::kRunIdMismatch));
    });

    // And the next connection starts unauthenticated rather than inheriting a
    // session from the one that was dropped.
    WithClient(*fixture.server, [&](PipeClient& client) {
        ASSERT_TRUE(client.WriteRequest(QStringLiteral("record.snapshot"), {}, QStringLiteral("6")));
        EXPECT_EQ(client.ReadResponse(QStringLiteral("6"))
                      .value(QStringLiteral("error"))
                      .toObject()
                      .value(QStringLiteral("code"))
                      .toString(),
                  QString::fromLatin1(error_code::kHandshakeRequired));
    });
}

TEST(LiveVerifyServer, ProtocolVersionMismatchIsReportedNotIgnored) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();

    WithClient(*fixture.server, [&](PipeClient& client) {
        ASSERT_TRUE(client.WriteRaw(QByteArray(R"({"protocol":99,"id":"9","command":"system.hello","params":{}})"
                                               "\n")));
        const QJsonObject response = client.ReadObject();
        EXPECT_EQ(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(),
                  QString::fromLatin1(error_code::kProtocolVersionMismatch));
    });
}

TEST(LiveVerifyServer, EventsReachAConnectedClient) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();

    WithClient(*fixture.server, [&](PipeClient& client) {
        ASSERT_TRUE(Hello(client, fixture.run_id).value(QStringLiteral("ok")).toBool());
        fixture.server->EmitEvent(QStringLiteral("record.stateChanged"),
                                  QJsonObject{{QStringLiteral("stateText"), QStringLiteral("Recording")}});
        const QJsonObject event = client.ReadObject();
        EXPECT_EQ(event.value(QStringLiteral("event")).toString(), QStringLiteral("record.stateChanged"));
        EXPECT_EQ(event.value(QStringLiteral("data")).toObject().value(QStringLiteral("stateText")).toString(),
                  QStringLiteral("Recording"));
    });
}

TEST(LiveVerifyServer, EventsWithNoClientAreDroppedRatherThanQueued) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();

    // Emitted while nobody is connected: a later client must not be handed a
    // past it never observed.
    fixture.server->EmitEvent(QStringLiteral("app.ready"), QJsonObject{});

    WithClient(*fixture.server, [&](PipeClient& client) {
        ASSERT_TRUE(Hello(client, fixture.run_id).value(QStringLiteral("ok")).toBool());
        ASSERT_TRUE(client.WriteRequest(QStringLiteral("record.snapshot"), {}, QStringLiteral("7")));
        // The very next line is the response, not a replayed event.
        const QJsonObject next = client.ReadObject();
        EXPECT_EQ(next.value(QStringLiteral("id")).toString(), QStringLiteral("7"));
    });
}

TEST(LiveVerifyServer, ReconnectingClientMustHandshakeAgainAndThenWorks) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();

    WithClient(*fixture.server, [&](PipeClient& client) {
        ASSERT_TRUE(Hello(client, fixture.run_id).value(QStringLiteral("ok")).toBool());
    });
    WithClient(*fixture.server, [&](PipeClient& client) {
        ASSERT_TRUE(Hello(client, fixture.run_id).value(QStringLiteral("ok")).toBool());
        ASSERT_TRUE(client.WriteRequest(QStringLiteral("record.snapshot"), {}, QStringLiteral("8")));
        EXPECT_TRUE(client.ReadResponse(QStringLiteral("8")).value(QStringLiteral("ok")).toBool());
    });
}

TEST(LiveVerifyServer, OversizedFrameIsRefusedWithoutUnboundedBuffering) {
    ServerFixture fixture;
    QString error;
    ASSERT_TRUE(fixture.server->Start(&error)) << error.toStdString();

    WithClient(*fixture.server, [&](PipeClient& client) {
        ASSERT_TRUE(Hello(client, fixture.run_id).value(QStringLiteral("ok")).toBool());
        // No newline: a frame that never terminates.
        ASSERT_TRUE(client.WriteRaw(QByteArray(kMaxRequestBytes + 16, 'x')));
        const QJsonObject response = client.ReadObject();
        EXPECT_EQ(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(),
                  QString::fromLatin1(error_code::kRequestTooLarge));
    });
    EXPECT_TRUE(fixture.server->running());
}

TEST(LiveVerifyServer, DestroyingTheServerWhileAClientIsConnectedDoesNotHang) {
    auto fixture = std::make_unique<ServerFixture>();
    QString error;
    ASSERT_TRUE(fixture->server->Start(&error)) << error.toStdString();

    std::atomic<bool> connected{false};
    std::atomic<bool> finished{false};
    const QString pipe_name = fixture->server->pipeName();
    std::thread worker([&]() {
        PipeClient client;
        if (client.Connect(pipe_name))
            connected.store(true);
        // Hold the connection open while the fixture goes away underneath it.
        while (!finished.load())
            Sleep(5);
    });

    QElapsedTimer clock;
    clock.start();
    while (!connected.load() && clock.elapsed() < 5000)
        App().processEvents(QEventLoop::AllEvents, 5);
    ASSERT_TRUE(connected.load());

    fixture.reset(); // Stop() + join, from the thread that owns the QObject.
    finished.store(true);
    worker.join();
    SUCCEED();
}
