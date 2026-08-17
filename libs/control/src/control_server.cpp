#include <control/control_server.h>

#include <control/options.h>

#include <QDebug>
#include <QMetaObject>

#include <windows.h>
// After windows.h, by requirement.
#include <sddl.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace exosnap::control {
namespace {

// Only the creating user may open the endpoint. Built from the process token's
// own SID rather than a well-known alias, so an elevated ExoSnap and a
// non-elevated one do not accidentally share an endpoint.
PSECURITY_DESCRIPTOR BuildCurrentUserOnlyDescriptor() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
        return nullptr;

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return nullptr;
    }
    std::vector<unsigned char> buffer(needed);
    if (GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed) == FALSE) {
        CloseHandle(token);
        return nullptr;
    }
    CloseHandle(token);

    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid_text = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &sid_text) == FALSE)
        return nullptr;

    // D: -- discretionary ACL. P -- protected, so nothing is inherited in.
    // A;;GA -- allow generic-all, to this SID and no other.
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid_text + L")";
    LocalFree(sid_text);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) ==
        FALSE) {
        return nullptr;
    }
    return descriptor;
}

// Only the first characters of the run id ever reach the application log. The
// full value is the connection credential, and the log ships inside support
// bundles.
QString RedactRunId(const QString& run_id) {
    return run_id.left(4) + QStringLiteral("…");
}

struct OverlappedEvent {
    // Declared before `overlapped` and initialized in place: the OVERLAPPED has
    // to point at this handle, so the handle has to exist first.
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED overlapped{};

    OverlappedEvent() {
        overlapped.hEvent = event;
    }
    ~OverlappedEvent() {
        if (event != nullptr)
            CloseHandle(event);
    }
    OverlappedEvent(const OverlappedEvent&) = delete;
    OverlappedEvent& operator=(const OverlappedEvent&) = delete;

    void Rearm() {
        ResetEvent(event);
        overlapped = OVERLAPPED{};
        overlapped.hEvent = event;
    }
};

} // namespace

ControlServer::ControlServer(ControlDispatcher* dispatcher, QString role, QString run_id, QString log_tag,
                             QObject* parent)
    : QObject(parent), dispatcher_(dispatcher), run_id_(std::move(run_id)), pipe_name_(PipeName(role, run_id_)),
      log_tag_(std::move(log_tag)) {
}

ControlServer::~ControlServer() {
    Stop();
    if (stop_event_ != nullptr)
        CloseHandle(static_cast<HANDLE>(stop_event_));
    if (outbound_event_ != nullptr)
        CloseHandle(static_cast<HANDLE>(outbound_event_));
}

bool ControlServer::Start(QString* error) {
    if (running_.load(std::memory_order_acquire))
        return true;

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    outbound_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr || outbound_event_ == nullptr) {
        if (error != nullptr)
            *error = QStringLiteral("Could not create control-server events");
        return false;
    }

    // Prove the endpoint can be created BEFORE reporting success, so a launch
    // that cannot be driven fails at startup instead of at first connect.
    PSECURITY_DESCRIPTOR descriptor = BuildCurrentUserOnlyDescriptor();
    if (descriptor == nullptr) {
        if (error != nullptr)
            *error = QStringLiteral("Could not build the control endpoint security descriptor");
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    const std::wstring name = pipe_name_.toStdWString();
    HANDLE first =
        CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024,
                         64 * 1024, 0, &attributes);
    LocalFree(descriptor);
    if (first == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not create the control endpoint (Win32 error %1)")
                         .arg(static_cast<qulonglong>(GetLastError()));
        }
        return false;
    }
    first_pipe_ = first;

    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this]() { ServeLoop(); });

    qInfo().noquote() << QStringLiteral("[%1] control endpoint ready (run %2)").arg(log_tag_, RedactRunId(run_id_));
    return true;
}

void ControlServer::Stop() {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (stop_event_ != nullptr)
        SetEvent(static_cast<HANDLE>(stop_event_));
    if (worker_.joinable())
        worker_.join();

    // AFTER the join, and here rather than only in the destructor. A Stop() that
    // lands before the worker's first loop iteration leaves the pre-created
    // first instance unconsumed, and an endpoint that outlives Stop() is exactly
    // the thing "a stopped server accepts nothing" has to mean.
    if (first_pipe_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(first_pipe_));
        first_pipe_ = nullptr;
    }
    if (was_running)
        qInfo().noquote() << QStringLiteral("[%1] control endpoint closed").arg(log_tag_);
}

void ControlServer::EmitEvent(const QString& name, QJsonObject data) {
    if (!client_connected_.load(std::memory_order_acquire))
        return;
    // GUI thread, like every caller: the dispatcher's negotiated version and the
    // source's revision counter are both GUI-thread state, and an event written
    // in a version the connected client did not agree to is worse than no event.
    const int protocol = dispatcher_ != nullptr ? dispatcher_->negotiatedProtocol() : kMinimumProtocolVersion;
    const std::optional<std::uint64_t> revision = dispatcher_ != nullptr ? dispatcher_->StateRevision() : std::nullopt;
    const QByteArray line = SerializeLine(MakeEvent(protocol, name, std::move(data), revision));
    {
        const std::lock_guard<std::mutex> guard(outbound_mutex_);
        outbound_.append(line);
    }
    if (outbound_event_ != nullptr)
        SetEvent(static_cast<HANDLE>(outbound_event_));
}

QList<QByteArray> ControlServer::TakeOutbound() {
    const std::lock_guard<std::mutex> guard(outbound_mutex_);
    QList<QByteArray> taken;
    taken.swap(outbound_);
    if (outbound_event_ != nullptr)
        ResetEvent(static_cast<HANDLE>(outbound_event_));
    return taken;
}

void ControlServer::RequestSessionReset() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
            if (dispatcher_ != nullptr)
                dispatcher_->ResetSession();
        },
        Qt::QueuedConnection);
}

QByteArray ControlServer::DispatchOnOwningThread(const ParsedRequest& request, bool* close_connection) {
    // shared_ptr, not a stack promise: the queued lambda may still run after this
    // function abandoned the wait on a timeout, and a promise destroyed under it
    // would be a use-after-free at exactly the moment the machine is already
    // misbehaving.
    auto promise = std::make_shared<std::promise<QJsonObject>>();
    auto poisoned = std::make_shared<std::atomic<bool>>(false);
    std::future<QJsonObject> future = promise->get_future();

    QMetaObject::invokeMethod(
        this,
        [this, request, promise, poisoned]() {
            promise->set_value(dispatcher_->Dispatch(request));
            poisoned->store(dispatcher_->connectionPoisoned(), std::memory_order_release);
        },
        Qt::QueuedConnection);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDispatchTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (future.wait_for(std::chrono::milliseconds(25)) == std::future_status::ready) {
            *close_connection = poisoned->load(std::memory_order_acquire);
            return SerializeLine(future.get());
        }
        if (WaitForSingleObject(static_cast<HANDLE>(stop_event_), 0) == WAIT_OBJECT_0) {
            *close_connection = true;
            return SerializeLine(
                MakeErrorResponse({request.protocol, request.id, QString::fromLatin1(error_code::kUnavailable),
                                   QStringLiteral("The application is shutting down")}));
        }
    }

    *close_connection = false;
    return SerializeLine(
        MakeErrorResponse({request.protocol, request.id, QString::fromLatin1(error_code::kDispatchTimeout),
                           QStringLiteral("The application did not answer within %1 ms").arg(kDispatchTimeoutMs)}));
}

void ControlServer::ServeLoop() {
    const std::wstring name = pipe_name_.toStdWString();
    HANDLE stop = static_cast<HANDLE>(stop_event_);
    HANDLE outbound_ready = static_cast<HANDLE>(outbound_event_);

    const auto write_all = [&](HANDLE pipe, const QByteArray& payload) {
        OverlappedEvent write_ov;
        DWORD written = 0;
        if (WriteFile(pipe, payload.constData(), static_cast<DWORD>(payload.size()), &written, &write_ov.overlapped) !=
            FALSE) {
            return true;
        }
        if (GetLastError() != ERROR_IO_PENDING)
            return false;
        HANDLE waits[] = {write_ov.event, stop};
        const DWORD outcome = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (outcome != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &write_ov.overlapped);
            return false;
        }
        return GetOverlappedResult(pipe, &write_ov.overlapped, &written, FALSE) != FALSE;
    };

    while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        if (first_pipe_ != nullptr) {
            pipe = static_cast<HANDLE>(first_pipe_);
            first_pipe_ = nullptr;
        } else {
            PSECURITY_DESCRIPTOR descriptor = BuildCurrentUserOnlyDescriptor();
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.lpSecurityDescriptor = descriptor;
            attributes.bInheritHandle = FALSE;

            pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                    64 * 1024, 64 * 1024, 0, descriptor != nullptr ? &attributes : nullptr);
            if (descriptor != nullptr)
                LocalFree(descriptor);
        }
        if (pipe == INVALID_HANDLE_VALUE)
            break;

        OverlappedEvent connect_ov;
        bool connected = false;
        if (ConnectNamedPipe(pipe, &connect_ov.overlapped) != FALSE) {
            connected = true;
        } else {
            const DWORD status = GetLastError();
            if (status == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (status == ERROR_IO_PENDING) {
                HANDLE waits[] = {connect_ov.event, stop};
                connected = WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0;
            }
        }
        if (!connected) {
            CancelIoEx(pipe, nullptr);
            CloseHandle(pipe);
            break;
        }

        client_connected_.store(true, std::memory_order_release);

        QByteArray accumulator;
        char buffer[8192];
        OverlappedEvent read_ov;
        bool read_pending = false;
        bool close_connection = false;

        while (!close_connection && WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
            if (!read_pending) {
                read_ov.Rearm();
                DWORD read_now = 0;
                if (ReadFile(pipe, buffer, sizeof(buffer), &read_now, &read_ov.overlapped) != FALSE) {
                    SetEvent(read_ov.event);
                } else if (GetLastError() != ERROR_IO_PENDING) {
                    break;
                }
                read_pending = true;
            }

            HANDLE waits[] = {read_ov.event, outbound_ready, stop};
            const DWORD outcome = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
            if (outcome == WAIT_OBJECT_0 + 2)
                break;

            if (outcome == WAIT_OBJECT_0 + 1) {
                for (const QByteArray& line : TakeOutbound()) {
                    if (!write_all(pipe, line)) {
                        close_connection = true;
                        break;
                    }
                }
                continue;
            }

            DWORD transferred = 0;
            if (GetOverlappedResult(pipe, &read_ov.overlapped, &transferred, FALSE) == FALSE || transferred == 0)
                break;
            read_pending = false;
            accumulator.append(buffer, static_cast<int>(transferred));

            // A frame that never terminates must not grow without bound; refuse
            // it once instead of buffering whatever the peer keeps sending.
            if (accumulator.size() > kMaxRequestBytes) {
                (void)write_all(pipe, SerializeLine(MakeErrorResponse(
                                          {kLatestProtocolVersion,
                                           {},
                                           QString::fromLatin1(error_code::kRequestTooLarge),
                                           QStringLiteral("Request exceeds %1 bytes").arg(kMaxRequestBytes)})));
                break;
            }

            int newline = accumulator.indexOf('\n');
            while (newline >= 0) {
                QByteArray line = accumulator.left(newline);
                accumulator.remove(0, newline + 1);
                if (line.endsWith('\r'))
                    line.chop(1);

                if (!line.trimmed().isEmpty()) {
                    ParsedRequest request;
                    ParseFailure failure;
                    QByteArray response;
                    if (ParseRequest(line, &request, &failure)) {
                        response = DispatchOnOwningThread(request, &close_connection);
                    } else {
                        response = SerializeLine(
                            MakeErrorResponse({failure.protocol, failure.id, failure.code, failure.message}));
                        // A protocol-version mismatch is not survivable: the peer
                        // is speaking a language this server does not know.
                        close_connection = failure.code == QString::fromLatin1(error_code::kProtocolVersionMismatch) ||
                                           failure.code == QString::fromLatin1(error_code::kRequestTooLarge);
                    }
                    if (!write_all(pipe, response)) {
                        close_connection = true;
                        break;
                    }
                }
                if (close_connection)
                    break;
                newline = accumulator.indexOf('\n');
            }
        }

        client_connected_.store(false, std::memory_order_release);
        RequestSessionReset();
        // Drop anything queued for the client that just left; the next one gets
        // a clean channel, not this connection's leftovers.
        (void)TakeOutbound();

        CancelIoEx(pipe, nullptr);
        // NEVER on the stop path. FlushFileBuffers on a named-pipe SERVER blocks
        // until the CLIENT has read everything still buffered -- with no timeout
        // and nothing to cancel it. A client that has stopped reading therefore
        // holds this thread, and Stop()'s join() with it, for as long as it likes.
        //
        // That is not hypothetical: a runner waiting for THIS process to exit is
        // by definition not reading, so the last event written before shutdown
        // (the update card moving to "pending" as the app closes for a swap) was
        // enough to keep the application alive indefinitely -- which the updater
        // then reported, correctly for what it could observe, as appWontClose.
        //
        // Letting a departing peer drain its buffer is politeness; it does not
        // outrank the product's ability to exit. On a normal client-initiated
        // disconnect the pipe is already gone and the flush returns at once, so
        // this costs nothing there.
        if (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0)
            FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

} // namespace exosnap::control
