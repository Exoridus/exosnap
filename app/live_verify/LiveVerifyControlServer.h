#pragma once

// LiveVerifyControlServer.h -- the local-only transport for the Live Verify
// control channel.
//
// Transport choice: a native Windows named pipe, not QLocalServer and not a
// socket. Three reasons, in order of weight:
//
//   1. No TCP, ever. A named pipe has no port and no listening socket, so
//      "is the control channel reachable from the network" is answered by the
//      API rather than by a bind address that could be mistyped. `netstat`
//      showing nothing is then a real proof, not a coincidence.
//   2. QLocalServer would pull Qt6::Network into the SHIPPING executable for one
//      release-verification seam -- a new DLL in every package, for a feature
//      that is dormant in every user launch. (Qt's Windows QLocalServer is a
//      named pipe underneath anyway.)
//   3. A pipe can be created with an explicit DACL. This one grants the creating
//      user alone, and sets PIPE_REJECT_REMOTE_CLIENTS.
//
// The endpoint name embeds the run id (LiveVerifyOptions::PipeNameForRunId), so
// two verification runs cannot collide and a normal second ExoSnap instance --
// which creates no pipe at all -- cannot be mistaken for one.
//
// Threading: one owned worker thread does all pipe I/O. Requests are handed to
// the Qt GUI thread with a queued call plus a future, never a blocking queued
// connection: the destructor runs ON the GUI thread, and a blocking connection
// would deadlock against its own join. A dispatch that does not come back within
// kDispatchTimeoutMs is answered with `dispatch_timeout` rather than hanging the
// connection, because a wedged GUI thread is itself an acceptance finding.

#include "LiveVerifyDispatcher.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace exosnap::live_verify {

class LiveVerifySource;

class LiveVerifyControlServer : public QObject {
    Q_OBJECT

  public:
    // How long the pipe thread waits for the GUI thread to answer one request.
    static constexpr int kDispatchTimeoutMs = 15000;

    LiveVerifyControlServer(LiveVerifySource* source, QString run_id, QObject* parent = nullptr);
    ~LiveVerifyControlServer() override;

    LiveVerifyControlServer(const LiveVerifyControlServer&) = delete;
    LiveVerifyControlServer& operator=(const LiveVerifyControlServer&) = delete;

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

    // GUI thread. Queues one event line. Dropped when no client is connected:
    // events are a synchronisation aid, and a buffered backlog replayed to a
    // client that connects later would describe a past the client never saw.
    void EmitEvent(const QString& name, QJsonObject data);

  private:
    void ServeLoop();
    // Runs on the pipe thread. Returns the response bytes to write, and sets
    // `close_connection` when the handshake failed fatally.
    [[nodiscard]] QByteArray DispatchOnGuiThread(const ParsedRequest& request, bool* close_connection);
    void RequestSessionReset();
    [[nodiscard]] QList<QByteArray> TakeOutbound();

    LiveVerifySource* source_ = nullptr;
    QString run_id_;
    QString pipe_name_;
    LiveVerifyDispatcher dispatcher_;

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

} // namespace exosnap::live_verify
