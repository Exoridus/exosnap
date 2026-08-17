#pragma once

// LiveVerifyCommandPolicy.h -- one table that says what every application
// command is, and one predicate per command that says whether it may run now.
//
// The reason this is a single table and not two is the whole point of it.
// `Dispatch()` needs to know whether a command may run; `ui.getState` needs to
// publish which commands may run. Written twice, those two answers drift, and
// they drift silently -- the runner is told an action is available, sends it and
// is refused, and the only symptom is a check that fails for a reason its own
// transcript contradicts. Here `Evaluate()` and `AvailableActions()` read the
// same predicates, so divergence is not a bug that can be introduced.
//
// The MECHANICS of that arrangement are shared with the updater's endpoint
// (control/command_policy.h); the TABLE below is not, because "may record.start
// run" is a question about this product and nothing else.
//
// Everything here is pure. Given an AutomationState it decides, with no Qt
// objects, no window and no application -- which is what lets the whole
// precondition surface be tested exhaustively instead of by driving the app into
// each state.

#include "LiveVerifyAutomationState.h"
#include "LiveVerifyProtocol.h"

#include <control/command_policy.h>

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace exosnap::live_verify {

using exosnap::control::CommandParameter;
using exosnap::control::PreconditionVerdict;
using exosnap::control::Settle;

using CommandDescriptor = exosnap::control::CommandDescriptor<AutomationState>;

// Whether a page has scrollable content the reveal/scroll commands can address.
// Record is deliberately absent: its composition is a fixed preview surface plus
// a transport dock, and it has never scrolled.
[[nodiscard]] bool IsScrollableSurface(const QString& page);

// Every command this build answers, in declaration order. The single authority:
// the dispatcher's routing, system.capabilities, system.hello and ipc.describe
// all read it.
[[nodiscard]] const QVector<CommandDescriptor>& AllCommands();

// Null when the name is not in the table. Fail-closed by construction: there is
// no prefix matching, no "did you mean" and no reflection anywhere near it.
[[nodiscard]] const CommandDescriptor* FindCommand(const QString& name);

// Sorted names a client of `protocol` may send.
[[nodiscard]] QStringList CommandNamesForProtocol(int protocol);

// The precondition verdict for a command in a state. Commands with no
// precondition answer `allowed`.
[[nodiscard]] PreconditionVerdict Evaluate(const CommandDescriptor& command, const AutomationState& state);

// Every mutating command whose precondition currently holds, sorted. Reads the
// same predicates Evaluate() does; see the file comment for why that matters.
[[nodiscard]] QStringList AvailableActions(const AutomationState& state);

// The static half of capability discovery: names, parameters, idempotency,
// settle behaviour and the protocol each command needs. Not a JSON Schema --
// the parameter surfaces are zero to three flat fields, and a schema generator
// would be more code than the validation it describes.
[[nodiscard]] QJsonObject DescribeCommands(int protocol);

// The observable state as the protocol publishes it, including availableActions.
[[nodiscard]] QJsonObject StateToJson(const AutomationState& state, std::uint64_t state_revision);

} // namespace exosnap::live_verify
