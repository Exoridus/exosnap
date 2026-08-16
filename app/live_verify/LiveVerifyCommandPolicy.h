#pragma once

// LiveVerifyCommandPolicy.h -- one table that says what every command is, and
// one predicate per command that says whether it may run right now.
//
// The reason this is a single table and not two is the whole point of it.
// `Dispatch()` needs to know whether a command may run; `ui.getState` needs to
// publish which commands may run. Written twice, those two answers drift, and
// they drift silently -- the runner is told an action is available, sends it and
// is refused, and the only symptom is a check that fails for a reason its own
// transcript contradicts. Here `Evaluate()` and `AvailableActions()` read the
// same predicates, so divergence is not a bug that can be introduced.
//
// Everything here is pure. Given an AutomationState it decides, with no Qt
// objects, no window and no application -- which is what lets the whole
// precondition surface be tested exhaustively instead of by driving the app into
// each state.

#include "LiveVerifyAutomationState.h"
#include "LiveVerifyProtocol.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace exosnap::live_verify {

// Why a command was refused, in the shape the response carries. An empty `code`
// means it may run.
struct PreconditionVerdict {
    QString code;
    QString message;
    // The precondition, and what was actually observed, as parallel objects.
    // Same keys on both sides: a runner diffs them instead of reading prose.
    QJsonObject requirements;
    QJsonObject actual;

    [[nodiscard]] bool allowed() const noexcept {
        return code.isEmpty();
    }
};

struct CommandParameter {
    QString name;
    // "string" | "int" | "bool" | "enum"
    QString type;
    bool required = false;
    // Populated for "enum" only, and it is the exact accepted set -- the
    // validator and this description read the same list.
    QStringList values;
};

// Whether the command's observable postcondition holds by the time the response
// is written.
enum class Settle {
    // Read-only. There is no postcondition, so `settled` is absent rather than
    // trivially true -- a client must not be able to read "settled" off a query
    // and conclude an action completed.
    NotApplicable,
    // The effect is observable in ui.getState before the response leaves. These
    // are the commands that let a runner drop its sleep entirely.
    Synchronous,
    // Accepted, effect still outstanding. The client waits for the named event
    // or for stateRevision to advance -- with ITS own timeout, not a fixed wait.
    Asynchronous,
};

struct CommandDescriptor {
    QString name;
    // Lowest envelope version that answers this command. A protocol-1 client
    // asking for a version-2 command gets `unknown_command`, which is exactly
    // what it would get from the build that predates the command.
    int minimum_protocol = kMinimumProtocolVersion;
    // Read-only queries are false. Only mutating commands appear in
    // availableActions: a list that included every snapshot would be a list of
    // things that are always true.
    bool mutating = false;
    bool idempotent = true;
    Settle settle = Settle::NotApplicable;
    QVector<CommandParameter> parameters;
    PreconditionVerdict (*precondition)(const AutomationState&) = nullptr;
};

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
