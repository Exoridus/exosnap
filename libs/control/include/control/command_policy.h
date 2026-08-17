#pragma once

// control/command_policy.h -- one table per process that says what every
// command is, and one predicate per command that says whether it may run now.
//
// The reason this is a single table and not two is the whole point of it.
// Dispatch needs to know whether a command may run; the state snapshot needs to
// publish which commands may run. Written twice, those two answers drift, and
// they drift silently -- the runner is told an action is available, sends it and
// is refused, and the only symptom is a check that fails for a reason its own
// transcript contradicts. Here Evaluate() and AvailableActions() read the same
// predicates, so divergence is not a bug that can be introduced.
//
// The MECHANICS are shared; the TABLE is not. Each process templates these on
// its own state type and writes its own commands, because "may record.start run"
// and "may updater.apply run" are product questions about different products.
//
// Everything here is pure. Given a state it decides, with no Qt objects, no
// window and no application -- which is what lets a whole precondition surface
// be tested exhaustively instead of by driving a process into each state.

#include <control/protocol.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <optional>

namespace exosnap::control {

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
    // The effect is observable in the state snapshot before the response leaves.
    // These are the commands that let a runner drop its sleep entirely.
    Synchronous,
    // Accepted, effect still outstanding. The client waits for the named event
    // or for stateRevision to advance -- with ITS own timeout, not a fixed wait.
    Asynchronous,
};

template <class State> struct CommandDescriptor {
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
    PreconditionVerdict (*precondition)(const State&) = nullptr;
};

template <class State> using CommandTable = QVector<CommandDescriptor<State>>;

// --- Shared, non-template pieces --------------------------------------------

[[nodiscard]] PreconditionVerdict Allowed();

// A refusal whose cause is stated as a key that exists on both sides. The two
// objects always carry the SAME key: `requires` is what the command needs,
// `actual` is what was observed, and a client compares them without reading a
// word of English.
[[nodiscard]] PreconditionVerdict Refuse(const char* code, QString message, const QString& key, QJsonValue required,
                                         QJsonValue observed);

// One validator for every command, driven by the same parameter descriptions
// the describe payload publishes. A hand-written check per command is how a
// documented parameter set and an enforced one come apart.
[[nodiscard]] std::optional<QString>
ValidateParams(const QString& command_name, const QVector<CommandParameter>& parameters, const QJsonObject& params);

[[nodiscard]] QString SettleName(Settle settle);

[[nodiscard]] QJsonArray DescribeParameters(const QVector<CommandParameter>& parameters);

// --- Template mechanics ------------------------------------------------------

// Null when the name is not in the table. Fail-closed by construction: there is
// no prefix matching, no "did you mean" and no reflection anywhere near it.
template <class State>
[[nodiscard]] const CommandDescriptor<State>* FindCommandIn(const CommandTable<State>& commands, const QString& name) {
    const auto match = std::find_if(commands.begin(), commands.end(),
                                    [&name](const CommandDescriptor<State>& c) { return c.name == name; });
    return match == commands.end() ? nullptr : &*match;
}

// The precondition verdict for a command in a state. Commands with no
// precondition answer `allowed`.
template <class State>
[[nodiscard]] PreconditionVerdict EvaluateIn(const CommandDescriptor<State>& command, const State& state) {
    if (command.precondition == nullptr)
        return Allowed();
    return command.precondition(state);
}

// Every mutating command whose precondition currently holds, sorted. Reads the
// same predicates EvaluateIn() does; see the file comment for why that matters.
template <class State>
[[nodiscard]] QStringList AvailableActionsIn(const CommandTable<State>& commands, const State& state) {
    QStringList actions;
    for (const CommandDescriptor<State>& command : commands) {
        if (!command.mutating)
            continue;
        if (EvaluateIn(command, state).allowed())
            actions.append(command.name);
    }
    actions.sort();
    return actions;
}

// Sorted names a client of `protocol` may send.
template <class State>
[[nodiscard]] QStringList CommandNamesForProtocolIn(const CommandTable<State>& commands, int protocol) {
    QStringList names;
    for (const CommandDescriptor<State>& command : commands) {
        if (command.minimum_protocol <= protocol)
            names.append(command.name);
    }
    names.sort();
    return names;
}

// The static half of capability discovery: names, parameters, idempotency,
// settle behaviour and the protocol each command needs. Not a JSON Schema --
// the parameter surfaces are zero to three flat fields, and a schema generator
// would be more code than the validation it describes.
template <class State> [[nodiscard]] QJsonObject DescribeCommandsIn(const CommandTable<State>& commands, int protocol) {
    QJsonArray described;
    for (const CommandDescriptor<State>& command : commands) {
        if (command.minimum_protocol > protocol)
            continue;
        QJsonObject json;
        json.insert(QStringLiteral("name"), command.name);
        json.insert(QStringLiteral("minimumProtocol"), command.minimum_protocol);
        json.insert(QStringLiteral("mutating"), command.mutating);
        json.insert(QStringLiteral("idempotent"), command.idempotent);
        json.insert(QStringLiteral("settle"), SettleName(command.settle));
        json.insert(QStringLiteral("parameters"), DescribeParameters(command.parameters));
        described.append(json);
    }

    QJsonArray error_codes;
    for (const QString& code : AllErrorCodes())
        error_codes.append(code);

    QJsonArray supported;
    for (int version = kMinimumProtocolVersion; version <= kLatestProtocolVersion; ++version)
        supported.append(version);

    QJsonObject json;
    json.insert(QStringLiteral("protocol"), protocol);
    json.insert(QStringLiteral("supportedProtocols"), supported);
    json.insert(QStringLiteral("commands"), described);
    json.insert(QStringLiteral("errorCodes"), error_codes);
    return json;
}

} // namespace exosnap::control
