#pragma once

// LiveVerifyControlServer.h -- the application's Live Verify endpoint.
//
// The transport itself (named pipe, DACL on the creating user,
// PIPE_REJECT_REMOTE_CLIENTS, FILE_FLAG_FIRST_PIPE_INSTANCE, run id in the name,
// dispatch with a timeout instead of a hang) lives in libs/control
// (control/control_server.h) because the updater's automation endpoint needs
// exactly the same properties, and a second implementation of them is a second
// place for the DACL to be forgotten.
//
// This class is the application's binding of it: its dispatcher, its role in the
// endpoint name ("LiveVerify", so the name is unchanged), and its log tag.
//
// A normal launch still has NO endpoint at all -- no pipe, no thread, no log
// line. That is asserted by live_verify.live_verify_server_tests, not by
// inspection; keep it that way.

#include "LiveVerifyDispatcher.h"

#include <control/control_server.h>

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace exosnap::live_verify {

class LiveVerifySource;

class LiveVerifyControlServer : public QObject {
    Q_OBJECT

  public:
    // How long the pipe thread waits for the GUI thread to answer one request.
    static constexpr int kDispatchTimeoutMs = exosnap::control::ControlServer::kDispatchTimeoutMs;

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
        return server_.pipeName();
    }
    [[nodiscard]] bool running() const noexcept {
        return server_.running();
    }

    // GUI thread. Queues one event line. Dropped when no client is connected:
    // events are a synchronisation aid, and a buffered backlog replayed to a
    // client that connects later would describe a past the client never saw.
    void EmitEvent(const QString& name, QJsonObject data);

  private:
    LiveVerifyDispatcher dispatcher_;
    exosnap::control::ControlServer server_;
};

} // namespace exosnap::live_verify
