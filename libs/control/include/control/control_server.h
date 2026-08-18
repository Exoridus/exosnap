#pragma once

// control/control_server.h -- the local-only transport for an ExoSnap control
// channel.
//
// Transport choice: a native Windows named pipe, not QLocalServer and not a
// socket. Three reasons, in order of weight:
//
//   1. No TCP, ever. A named pipe has no port and no listening socket, so
//      "is the control channel reachable from the network" is answered by the
//      API rather than by a bind address that could be mistyped. `netstat`
//      showing nothing is then a real proof, not a coincidence.
//   2. QLocalServer would pull Qt6::Network into the SHIPPING executables for
//      one release-verification seam -- a new DLL in every package, for a
//      feature that is dormant in every user launch. (Qt's Windows QLocalServer
//      is a named pipe underneath anyway.)
//   3. A pipe can be created with an explicit DACL. This one grants the creating
//      user alone, and sets PIPE_REJECT_REMOTE_CLIENTS.
//
// The endpoint name embeds a role and the run id (control::PipeName), so two
// verification runs cannot collide, a normal second process -- which creates no
// pipe at all -- cannot be mistaken for one, and a runner can hold the
// application's endpoint and the updater's endpoint of the SAME run at once.
//
// Threading: one owned worker thread does all pipe I/O. Requests are handed to
// the owning (GUI) thread with a queued call plus a future, never a blocking
// queued connection: the destructor runs ON that thread, and a blocking
// connection would deadlock against its own join. A dispatch that does not come
// back within kDispatchTimeoutMs is answered with `dispatch_timeout` rather than
// hanging the connection, because a wedged main thread is itself a finding.

#include <control/session.h>

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <mutex>
#include <thread>

namespace exosnap::control {

class ControlServer : public QObject {
    Q_OBJECT

  public:
    // How long the pipe thread waits for the owning thread to answer one request.
    static constexpr int kDispatchTimeoutMs = 15000;

    // `role` becomes part of the endpoint name ("LiveVerify", "Updater");
    // `log_tag` is the bracketed prefix of the two lines this class logs.
    ControlServer(ControlDispatcher* dispatcher, QString role, QString run_id, QString log_tag,
                  QObject* parent = nullptr);
    ~ControlServer() override;

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Creates the endpoint and starts the worker. False with `error` filled when
    // the pipe could not be created -- the caller must treat that as fatal for a
    // verification launch.
    [[nodiscard]] bool Start(QString* error);
    void Stop();

    [[nodiscard]] const QString& pipeName() const noexcept {
        return pipe_name_;
    }
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // Owning thread. Queues one event line. Dropped when no client is connected:
    // events are a synchronisation aid, and a buffered backlog replayed to a
    // client that connects later would describe a past the client never saw.
    void EmitEvent(const QString& name, QJsonObject data);

  private:
    void ServeLoop();
    // Runs on the pipe thread. Returns the response bytes to write, and sets
    // `close_connection` when the handshake failed fatally.
    [[nodiscard]] QByteArray DispatchOnOwningThread(const ParsedRequest& request, bool* close_connection);
    void RequestSessionReset();
    [[nodiscard]] QList<QByteArray> TakeOutbound();

    ControlDispatcher* dispatcher_ = nullptr;
    QString run_id_;
    QString pipe_name_;
    QString log_tag_;

    // Win32 HANDLEs kept as void* so windows.h stays out of this header.
    void* stop_event_ = nullptr;
    void* outbound_event_ = nullptr;
    // The FILE_FLAG_FIRST_PIPE_INSTANCE handle, created in Start() and consumed
    // by the worker's first iteration. Created up front so a launch that cannot
    // own the endpoint fails at startup rather than at first connect, and held
    // rather than reopened so nothing can claim the name in between.
    void* first_pipe_ = nullptr;

    std::mutex outbound_mutex_;
    QList<QByteArray> outbound_;
    std::atomic<bool> client_connected_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace exosnap::control
