#pragma once

// control/session.h -- the connection state machine and the dispatch skeleton
// every ExoSnap control endpoint shares.
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
//      letting a client retry credentials against a live process.
//   3. system.hello a second time is refused; a handshake is per connection.
//   4. The protocol version of the hello is the connection's version. Every
//      later request must use the same one -- a client that spoke v1 to
//      authenticate and v2 to act would be two clients on one credential.
//
// Preconditions are not decided here. Dispatch() reads the process's command
// table through control::EvaluateIn, and so does the state snapshot's
// availableActions; see command_policy.h for why that is one table and not two.
//
// What a process supplies is the small set of protected virtuals at the bottom:
// its identity, its command table, its events, its state and how to execute a
// command. Everything above them is the same for every endpoint, which is the
// point -- a second hand-written session machine is a second place for a
// half-completed handshake to be accepted.

#include <control/command_policy.h>
#include <control/protocol.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <utility>

namespace exosnap::control {

// What executing a command produced. Kept separate from the wire envelope so
// the execution never has to think about protocol versions or about which
// fields a v1 response is allowed to carry.
struct Outcome {
    bool ok = true;
    QJsonObject result;
    // Meaningful only for mutating commands; read-only ones leave `settled`
    // absent from the response entirely.
    bool settled = true;
    QString code;
    QString message;
    QJsonObject requirements;
    QJsonObject actual;
};

[[nodiscard]] Outcome Succeeded(QJsonObject result, bool settled = true);
[[nodiscard]] Outcome Failed(QString code, QString message, QJsonObject requirements = {}, QJsonObject actual = {});
[[nodiscard]] Outcome Failed(const char* code, QString message, QJsonObject requirements = {}, QJsonObject actual = {});

[[nodiscard]] QJsonArray ToJsonArray(const QStringList& values);

// The non-template face the transport talks to, so the pipe server does not
// have to know which process it is serving.
class ControlDispatcher {
  public:
    virtual ~ControlDispatcher() = default;

    // A parsed, protocol-valid request. Returns the response object to send.
    [[nodiscard]] virtual QJsonObject Dispatch(const ParsedRequest& request) = 0;

    // The version agreed at the handshake, which is the version events are
    // written in. kMinimumProtocolVersion until a hello succeeds.
    [[nodiscard]] virtual int negotiatedProtocol() const noexcept = 0;

    // Set once a handshake failed fatally. The transport must close the
    // connection after writing the response.
    [[nodiscard]] virtual bool connectionPoisoned() const noexcept = 0;

    // Called by the transport when a client disconnects, so the next connection
    // starts from an unauthenticated state instead of inheriting one.
    virtual void ResetSession() = 0;

    // For the revision an unsolicited event carries. Absent when no state
    // source is bound.
    [[nodiscard]] virtual std::optional<std::uint64_t> StateRevision() const = 0;
};

template <class State> class ControlSession : public ControlDispatcher {
  public:
    explicit ControlSession(QString run_id) : run_id_(std::move(run_id)) {
    }

    [[nodiscard]] int negotiatedProtocol() const noexcept override {
        return negotiated_protocol_;
    }
    [[nodiscard]] bool connectionPoisoned() const noexcept override {
        return poisoned_;
    }
    [[nodiscard]] bool handshakeComplete() const noexcept {
        return handshake_complete_;
    }

    void ResetSession() override {
        handshake_complete_ = false;
        poisoned_ = false;
        negotiated_protocol_ = kMinimumProtocolVersion;
    }

    [[nodiscard]] std::optional<std::uint64_t> StateRevision() const override {
        return HasState() ? std::optional<std::uint64_t>(Revision()) : std::nullopt;
    }

    // Named ...For so a process may keep its own static CommandNames()/
    // EventNames() helpers without colliding with these.
    [[nodiscard]] QStringList CommandNamesFor(int protocol) const {
        return CommandNamesForProtocolIn(Commands(), protocol);
    }

    [[nodiscard]] QJsonObject Dispatch(const ParsedRequest& request) override {
        if (request.command == QLatin1String("system.hello"))
            return HandleHello(request);

        if (!handshake_complete_) {
            return MakeErrorResponse({request.protocol, request.id, QString::fromLatin1(error_code::kHandshakeRequired),
                                      QStringLiteral("system.hello must be the first command on a connection")});
        }

        if (request.protocol != negotiated_protocol_) {
            // The handshake fixed the dialect. A client that switches
            // mid-connection is either two clients on one credential or a bug
            // that would otherwise surface as a field quietly missing from half
            // the transcript.
            poisoned_ = true;
            return MakeErrorResponse(
                {negotiated_protocol_, request.id, QString::fromLatin1(error_code::kProtocolVersionMismatch),
                 QStringLiteral("This connection negotiated protocol %1 at its handshake").arg(negotiated_protocol_)});
        }

        const CommandDescriptor<State>* command = FindCommandIn(Commands(), request.command);
        if (command == nullptr || command->minimum_protocol > request.protocol) {
            // Fail closed. No prefix matching, no "did you mean", no reflection
            // -- and a protocol-2 command asked for over protocol 1 is answered
            // exactly as the build that predates it would have answered.
            return MakeErrorResponse({request.protocol, request.id, QString::fromLatin1(error_code::kUnknownCommand),
                                      QStringLiteral("Unknown command: %1").arg(request.command)});
        }

        if (!HasState()) {
            return MakeErrorResponse({request.protocol, request.id, QString::fromLatin1(error_code::kUnavailable),
                                      QStringLiteral("No application state is bound to this server")});
        }

        if (const std::optional<QString> invalid = ValidateParams(command->name, command->parameters, request.params);
            invalid.has_value()) {
            return MakeErrorResponse({request.protocol,
                                      request.id,
                                      QString::fromLatin1(error_code::kInvalidParams),
                                      *invalid,
                                      {},
                                      {},
                                      std::nullopt});
        }

        const State before = StateValue();
        if (const PreconditionVerdict verdict = EvaluateIn(*command, before); !verdict.allowed()) {
            ErrorEnvelope envelope;
            envelope.protocol = request.protocol;
            envelope.id = request.id;
            envelope.code = verdict.code;
            envelope.message = verdict.message;
            envelope.requirements = verdict.requirements;
            envelope.actual = verdict.actual;
            if (request.protocol >= 2)
                envelope.state_revision = Revision();
            return MakeErrorResponse(envelope);
        }

        const Outcome outcome = Execute(*command, request);

        const std::optional<std::uint64_t> revision =
            request.protocol >= 2 ? std::optional<std::uint64_t>(Revision()) : std::nullopt;

        if (!outcome.ok) {
            ErrorEnvelope envelope;
            envelope.protocol = request.protocol;
            envelope.id = request.id;
            envelope.code = outcome.code;
            envelope.message = outcome.message;
            envelope.requirements = outcome.requirements;
            envelope.actual = outcome.actual;
            envelope.state_revision = revision;
            return MakeErrorResponse(envelope);
        }

        SuccessEnvelope envelope;
        envelope.protocol = request.protocol;
        envelope.id = request.id;
        envelope.result = outcome.result;
        envelope.state_revision = revision;
        if (request.protocol >= 2 && command->settle != Settle::NotApplicable)
            envelope.settled = outcome.settled;
        if (request.include_state)
            envelope.state = StateJson(StateValue(), Revision());
        return MakeSuccessResponse(envelope);
    }

  protected:
    // --- What the process supplies -------------------------------------------

    // Enough for a runner to refuse a process it did not mean to talk to.
    [[nodiscard]] virtual QJsonObject Identity() const = 0;
    [[nodiscard]] virtual const CommandTable<State>& Commands() const = 0;
    [[nodiscard]] virtual QStringList EventNamesFor(int protocol) const = 0;
    // False when no state source is bound; every command then answers
    // `unavailable` rather than dereferencing nothing.
    [[nodiscard]] virtual bool HasState() const = 0;
    [[nodiscard]] virtual State StateValue() const = 0;
    [[nodiscard]] virtual std::uint64_t Revision() const = 0;
    [[nodiscard]] virtual QJsonObject StateJson(const State& state, std::uint64_t revision) const = 0;
    [[nodiscard]] virtual Outcome Execute(const CommandDescriptor<State>& command, const ParsedRequest& request) = 0;

    // --- Shared payloads a process's Execute can hand back unchanged ---------

    [[nodiscard]] QJsonObject CapabilitiesPayload(int protocol) const {
        QJsonObject result;
        result.insert(QStringLiteral("protocol"), protocol);
        result.insert(QStringLiteral("commands"), ToJsonArray(CommandNamesFor(protocol)));
        result.insert(QStringLiteral("events"), ToJsonArray(EventNamesFor(protocol)));
        // Additive, and protocol 2 only: a v1 client's capabilities payload is
        // byte-identical to the one it has always received.
        if (protocol >= 2) {
            result.insert(QStringLiteral("errorCodes"), ToJsonArray(AllErrorCodes()));
            QJsonArray supported;
            for (int version = kMinimumProtocolVersion; version <= kLatestProtocolVersion; ++version)
                supported.append(version);
            result.insert(QStringLiteral("supportedProtocols"), supported);
        }
        return result;
    }

    [[nodiscard]] QJsonObject DescribePayload(int protocol) const {
        QJsonObject result = DescribeCommandsIn(Commands(), protocol);
        result.insert(QStringLiteral("events"), ToJsonArray(EventNamesFor(protocol)));
        return result;
    }

  private:
    [[nodiscard]] QJsonObject HandleHello(const ParsedRequest& request) {
        if (handshake_complete_) {
            return MakeErrorResponse({request.protocol, request.id, QString::fromLatin1(error_code::kAlreadyHandshaken),
                                      QStringLiteral("This connection has already completed its handshake")});
        }

        const QJsonValue run_id = request.params.value(QStringLiteral("runId"));
        if (!run_id.isString()) {
            return MakeErrorResponse({request.protocol, request.id, QString::fromLatin1(error_code::kInvalidParams),
                                      QStringLiteral("system.hello requires a string \"runId\" parameter")});
        }
        if (run_id.toString() != run_id_) {
            // Fatal on purpose. The run id is the connection credential; a
            // client that guessed wrong is not given a second guess on a live
            // process.
            poisoned_ = true;
            return MakeErrorResponse({request.protocol, request.id, QString::fromLatin1(error_code::kRunIdMismatch),
                                      QStringLiteral("Run id does not match this process")});
        }

        handshake_complete_ = true;
        negotiated_protocol_ = request.protocol;

        QJsonObject result = HasState() ? Identity() : QJsonObject{};
        result.insert(QStringLiteral("protocol"), request.protocol);
        result.insert(QStringLiteral("runId"), run_id_);
        result.insert(QStringLiteral("commands"), ToJsonArray(CommandNamesFor(request.protocol)));
        result.insert(QStringLiteral("events"), ToJsonArray(EventNamesFor(request.protocol)));
        if (request.protocol >= 2) {
            result.insert(QStringLiteral("errorCodes"), ToJsonArray(AllErrorCodes()));
            QJsonArray supported;
            for (int version = kMinimumProtocolVersion; version <= kLatestProtocolVersion; ++version)
                supported.append(version);
            result.insert(QStringLiteral("supportedProtocols"), supported);
        }

        SuccessEnvelope envelope;
        envelope.protocol = request.protocol;
        envelope.id = request.id;
        envelope.result = result;
        if (request.protocol >= 2 && HasState())
            envelope.state_revision = Revision();
        return MakeSuccessResponse(envelope);
    }

    QString run_id_;
    int negotiated_protocol_ = kMinimumProtocolVersion;
    bool handshake_complete_ = false;
    bool poisoned_ = false;
};

} // namespace exosnap::control
