#pragma once

// UpdaterControlDispatcher.h -- the updater's binding of the shared control
// session to its own command table and its own actions.
//
// The session rules (hello first, run id is the credential, one protocol per
// connection, fail closed on an unknown command) come from libs/control, so this
// endpoint and the application's are provably the same channel. What is here is
// what only this process can answer: its identity, its commands, its state and
// how to execute one of the six product actions.
//
// There is deliberately NO command that arms a handoff. A handoff is a start
// argument by definition; a channel that could set one afterwards would be an
// external caller deciding what an elevated msiexec installs, which is the one
// shape of this feature that is not safe.

#include "UpdaterCommandPolicy.h"
#include "UpdaterControlSource.h"

#include <control/options.h>
#include <control/session.h>

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace exosnap::updater_control {

// The option that arms the endpoint, and the role its pipe name carries. Without
// the option there is no pipe, no thread and no log line -- the same rule the
// application's channel follows, and for the same reason: an endpoint nobody
// asked for is attack surface nobody accounted for.
inline constexpr const char* kControlOption = exosnap::control::option::kUpdaterControl;
inline constexpr const char* kControlRole = exosnap::control::role::kUpdater;

// The one event this endpoint emits. It fires when the revision advances, which
// is what lets a client wait on "something observable changed" rather than on a
// clock -- and, unlike a per-phase event, it cannot be missing for the phase a
// test happens to care about.
inline constexpr const char* kStateChangedEvent = "updater.stateChanged";

class UpdaterControlDispatcher : public exosnap::control::ControlSession<FlowState> {
  public:
    UpdaterControlDispatcher(UpdaterControlSource* source, QString run_id);

    [[nodiscard]] static QStringList CommandNames(int protocol);
    [[nodiscard]] static QStringList EventNames(int protocol);

  protected:
    [[nodiscard]] QJsonObject Identity() const override;
    [[nodiscard]] const exosnap::control::CommandTable<FlowState>& Commands() const override;
    [[nodiscard]] QStringList EventNamesFor(int protocol) const override;
    [[nodiscard]] bool HasState() const override;
    [[nodiscard]] FlowState StateValue() const override;
    [[nodiscard]] std::uint64_t Revision() const override;
    [[nodiscard]] QJsonObject StateJson(const FlowState& state, std::uint64_t revision) const override;
    [[nodiscard]] exosnap::control::Outcome Execute(const CommandDescriptor& command,
                                                    const ParsedRequest& request) override;

  private:
    UpdaterControlSource* source_ = nullptr;
};

} // namespace exosnap::updater_control
