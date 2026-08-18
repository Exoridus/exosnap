#include "UpdaterControlDispatcher.h"

#include <QJsonValue>

#include <utility>

namespace exosnap::updater_control {
namespace {

using exosnap::control::Failed;
using exosnap::control::Outcome;
using exosnap::control::Succeeded;

namespace ec = exosnap::control::error_code;

// An intent that passed its precondition and still came back false. Rather than
// invent a second explanation, the policy is asked again against the state as it
// is NOW: if the product refused because something changed between the check and
// the call, the client gets that reason in the same structured form it would
// have got a moment earlier. Only when the state still permits the command is
// this an operational failure.
Outcome IntentRefused(const CommandDescriptor& command, const UpdaterControlSource& source, const QString& error) {
    const PreconditionVerdict verdict = Evaluate(command, source.State());
    if (!verdict.allowed())
        return Failed(verdict.code, verdict.message, verdict.requirements, verdict.actual);
    return Failed(ec::kOperationFailed, error);
}

} // namespace

UpdaterControlDispatcher::UpdaterControlDispatcher(UpdaterControlSource* source, QString run_id)
    : exosnap::control::ControlSession<FlowState>(std::move(run_id)), source_(source) {
}

QStringList UpdaterControlDispatcher::CommandNames(int protocol) {
    return CommandNamesForProtocol(protocol);
}

QStringList UpdaterControlDispatcher::EventNames(int protocol) {
    QStringList names;
    // Protocol 2 only, because the payload it carries -- the revision -- is a
    // protocol-2 field. A v1 client would receive an event it cannot correlate.
    if (protocol >= 2)
        names.append(QString::fromLatin1(kStateChangedEvent));
    names.sort();
    return names;
}

QJsonObject UpdaterControlDispatcher::Identity() const {
    return source_ != nullptr ? source_->Identity() : QJsonObject{};
}

const exosnap::control::CommandTable<FlowState>& UpdaterControlDispatcher::Commands() const {
    return AllCommands();
}

QStringList UpdaterControlDispatcher::EventNamesFor(int protocol) const {
    return EventNames(protocol);
}

bool UpdaterControlDispatcher::HasState() const {
    return source_ != nullptr;
}

FlowState UpdaterControlDispatcher::StateValue() const {
    return source_ != nullptr ? source_->State() : FlowState{};
}

std::uint64_t UpdaterControlDispatcher::Revision() const {
    return source_ != nullptr ? source_->StateRevision() : 0;
}

QJsonObject UpdaterControlDispatcher::StateJson(const FlowState& state, std::uint64_t revision) const {
    return StateToJson(state, revision);
}

exosnap::control::Outcome UpdaterControlDispatcher::Execute(const CommandDescriptor& command,
                                                            const ParsedRequest& request) {
    if (command.name == QLatin1String("system.capabilities"))
        return Succeeded(CapabilitiesPayload(request.protocol));
    if (command.name == QLatin1String("ipc.describe"))
        return Succeeded(DescribePayload(request.protocol));
    if (command.name == QLatin1String("updater.getState"))
        return Succeeded(StateToJson(source_->State(), source_->StateRevision()));

    QString error;
    const auto intent = [&](bool (UpdaterControlSource::*action)(QString*)) {
        if (!(source_->*action)(&error))
            return IntentRefused(command, *source_, error);
        // Accepted, NOT completed. The snapshot describes the state at the
        // moment the intent was taken; the authoritative confirmation is the
        // next stateRevision, and settled:false says so on the wire.
        return Succeeded(StateToJson(source_->State(), source_->StateRevision()), /*settled=*/false);
    };

    if (command.name == QLatin1String("updater.check"))
        return intent(&UpdaterControlSource::Check);
    if (command.name == QLatin1String("updater.download"))
        return intent(&UpdaterControlSource::Download);
    if (command.name == QLatin1String("updater.apply"))
        return intent(&UpdaterControlSource::Apply);
    if (command.name == QLatin1String("updater.retry"))
        return intent(&UpdaterControlSource::Retry);
    if (command.name == QLatin1String("updater.cancel"))
        return intent(&UpdaterControlSource::Cancel);
    if (command.name == QLatin1String("updater.close"))
        return intent(&UpdaterControlSource::Close);

    // Reachable only if a command is added to the policy table without a branch
    // here; loud rather than silently answering nothing.
    return Failed(ec::kUnknownCommand, QStringLiteral("Command %1 is listed but not implemented").arg(command.name));
}

} // namespace exosnap::updater_control
