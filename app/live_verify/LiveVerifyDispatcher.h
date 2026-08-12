#pragma once

// LiveVerifyDispatcher.h -- the command allowlist and the session state machine.
//
// One request in, one response out, with no I/O of its own. That is what makes
// the whole rejection surface -- unknown command, missing handshake, wrong run
// id, bad parameters, a refusing intent -- testable without a pipe.
//
// Session rules, in order:
//   1. The first accepted command MUST be system.hello.
//   2. system.hello must carry the exact run id this process was launched with.
//      Wrong id is fatal for the connection: the server closes it rather than
//      letting a client retry credentials against a live application.
//   3. system.hello a second time is refused; a handshake is per connection.

#include "LiveVerifyProtocol.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace exosnap::live_verify {

class LiveVerifySource;

class LiveVerifyDispatcher {
  public:
    LiveVerifyDispatcher(LiveVerifySource* source, QString run_id);

    // Every command this build answers, sorted. Also the payload of
    // system.capabilities, so a client can never be told about a command the
    // dispatcher would then reject.
    [[nodiscard]] static QStringList CommandNames();
    // Every event this build can emit. Same contract in the other direction: a
    // runner that waits for an event not in this list would wait forever.
    [[nodiscard]] static QStringList EventNames();

    // A parsed, protocol-valid request. Returns the response object to send.
    [[nodiscard]] QJsonObject Dispatch(const ParsedRequest& request);

    // Set once a handshake failed fatally. The transport must close the
    // connection after writing the response.
    [[nodiscard]] bool connectionPoisoned() const noexcept {
        return poisoned_;
    }
    [[nodiscard]] bool handshakeComplete() const noexcept {
        return handshake_complete_;
    }

    // Called by the transport when a client disconnects, so the next connection
    // starts from an unauthenticated state instead of inheriting one.
    void ResetSession();

  private:
    [[nodiscard]] QJsonObject HandleHello(const ParsedRequest& request);

    LiveVerifySource* source_ = nullptr;
    QString run_id_;
    bool handshake_complete_ = false;
    bool poisoned_ = false;
};

} // namespace exosnap::live_verify
