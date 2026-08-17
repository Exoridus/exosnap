#pragma once

// UpdaterCommandPolicy.h -- one table that says what every updater command is,
// and one predicate per command that says whether it may run right now.
//
// Same construction as the application's channel, and for the same reason:
// `Dispatch()` needs to know whether a command may run, and `updater.getState`
// needs to publish which commands may run. Written twice, those two answers
// drift silently -- the runner is told an action is available, sends it and is
// refused, and the only symptom is a check that fails for a reason its own
// transcript contradicts. Here Evaluate() and AvailableActions() read the same
// predicates, so divergence is not a bug that can be introduced.
//
// The refusal taxonomy is used precisely, because a runner branches on it:
//   * invalid_state -- the updater is simply in the wrong phase for this
//     command (apply while idle, download with nothing on offer).
//   * blocked       -- the phase would be right and a product rule refuses
//     anyway: a check in a build that may not contact the feed, a cancel during
//     the staged rename. THIS is the one that must never be faked. The engine
//     observes cancellation during a download and during the bounded msiexec
//     wait, and nowhere else; a `cancel` accepted during the rename would be a
//     lie the client cannot detect.
//
// Everything here is pure. Given an UpdateFlowState it decides, with no Qt
// objects, no window and no worker -- which is what lets the whole precondition
// surface be tested exhaustively instead of by driving a real installation into
// each state.

#include <control/command_policy.h>

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstdint>

#include <update/update_flow_state.h>

namespace exosnap::updater_control {

using exosnap::control::CommandParameter;
using exosnap::control::ParsedRequest;
using exosnap::control::PreconditionVerdict;
using exosnap::control::Settle;

using FlowState = exosnap::update::UpdateFlowState;
using CommandDescriptor = exosnap::control::CommandDescriptor<FlowState>;

// Every command this build answers, in declaration order. The single authority:
// the dispatcher's routing, system.capabilities and ipc.describe all read it.
[[nodiscard]] const exosnap::control::CommandTable<FlowState>& AllCommands();

[[nodiscard]] const CommandDescriptor* FindCommand(const QString& name);
[[nodiscard]] QStringList CommandNamesForProtocol(int protocol);
[[nodiscard]] PreconditionVerdict Evaluate(const CommandDescriptor& command, const FlowState& state);
[[nodiscard]] QStringList AvailableActions(const FlowState& state);
[[nodiscard]] QJsonObject DescribeCommands(int protocol);

// The observable state as the protocol publishes it, including availableActions.
// Names come from the shared vocabulary tables (update_flow_state.h), never from
// a second list written here.
[[nodiscard]] QJsonObject StateToJson(const FlowState& state, std::uint64_t state_revision);

} // namespace exosnap::updater_control
