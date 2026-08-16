#pragma once

// LiveVerifyDispatcher.h -- the command allowlist and the session state machine.
//
// One request in, one response out, with no I/O of its own. That is what makes
// the whole rejection surface -- unknown command, missing handshake, wrong run
// id, bad parameters, an unmet precondition, a refusing intent -- testable
// without a pipe.
//
// Session rules, in order:
//   1. The first accepted command MUST be system.hello.
//   2. system.hello must carry the exact run id this process was launched with.
//      Wrong id is fatal for the connection: the server closes it rather than
//      letting a client retry credentials against a live application.
//   3. system.hello a second time is refused; a handshake is per connection.
//   4. The protocol version of the hello is the connection's version. Every
//      later request must use the same one -- a client that spoke v1 to
//      authenticate and v2 to act would be two clients on one credential.
//
// Preconditions are not decided here either. Dispatch() reads
// LiveVerifyCommandPolicy, and so does ui.getState's availableActions; see
// LiveVerifyCommandPolicy.h for why that is one table and not two.

#include "LiveVerifyProtocol.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace exosnap::live_verify {

class LiveVerifySource;

class LiveVerifyDispatcher {
  public:
    LiveVerifyDispatcher(LiveVerifySource* source, QString run_id);

    // Every command a client of `protocol` may send, sorted. Also the payload of
    // system.capabilities, so a client can never be told about a command the
    // dispatcher would then reject.
    [[nodiscard]] static QStringList CommandNames(int protocol);
    // Every event this build can emit at that version. Same contract in the
    // other direction: a runner that waits for an event not in this list would
    // wait forever.
    [[nodiscard]] static QStringList EventNames(int protocol);

    // A parsed, protocol-valid request. Returns the response object to send.
    [[nodiscard]] QJsonObject Dispatch(const ParsedRequest& request);

    // The version agreed at the handshake, which is the version events are
    // written in. kMinimumProtocolVersion until a hello succeeds.
    [[nodiscard]] int negotiatedProtocol() const noexcept {
        return negotiated_protocol_;
    }

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
    int negotiated_protocol_ = kMinimumProtocolVersion;
    bool handshake_complete_ = false;
    bool poisoned_ = false;
};

} // namespace exosnap::live_verify
