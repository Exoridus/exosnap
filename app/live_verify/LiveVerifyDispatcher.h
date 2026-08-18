#pragma once

// LiveVerifyDispatcher.h -- the application's binding of the shared control
// session to its own command table and its own intents.
//
// The session rules (hello first, run id is the credential, one protocol per
// connection, fail closed on an unknown command) live in libs/control
// (control/session.h) because the updater's endpoint obeys the same four and a
// second hand-written state machine is a second place for a half-completed
// handshake to be accepted. What is here is what only this process can answer:
// its identity, its command table, its events, its state, and how to execute a
// command against the application.
//
// Preconditions are not decided here either. Dispatch() reads
// LiveVerifyCommandPolicy, and so does ui.getState's availableActions; see
// LiveVerifyCommandPolicy.h for why that is one table and not two.

#include "LiveVerifyAutomationState.h"
#include "LiveVerifyCommandPolicy.h"
#include "LiveVerifyProtocol.h"

#include <control/session.h>

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace exosnap::live_verify {

class LiveVerifySource;

class LiveVerifyDispatcher : public exosnap::control::ControlSession<AutomationState> {
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

  protected:
    [[nodiscard]] QJsonObject Identity() const override;
    [[nodiscard]] const exosnap::control::CommandTable<AutomationState>& Commands() const override;
    [[nodiscard]] QStringList EventNamesFor(int protocol) const override;
    [[nodiscard]] bool HasState() const override;
    [[nodiscard]] AutomationState StateValue() const override;
    [[nodiscard]] std::uint64_t Revision() const override;
    [[nodiscard]] QJsonObject StateJson(const AutomationState& state, std::uint64_t revision) const override;
    [[nodiscard]] exosnap::control::Outcome Execute(const CommandDescriptor& command,
                                                    const ParsedRequest& request) override;

  private:
    LiveVerifySource* source_ = nullptr;
};

} // namespace exosnap::live_verify
