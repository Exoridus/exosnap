#include "UpdaterAutomation.h"

#include <utility>

UpdaterAutomation::UpdaterAutomation(QJsonObject identity, Intents intents)
    : identity_(std::move(identity)), intents_(std::move(intents)) {
}

bool UpdaterAutomation::RevisionBearingEqual(const exosnap::update::UpdateFlowState& a,
                                             const exosnap::update::UpdateFlowState& b) {
    exosnap::update::UpdateFlowState left = a;
    exosnap::update::UpdateFlowState right = b;
    // The one deliberate exclusion. See the header: a 12 Hz progress tick that
    // advanced the revision would make "wait for the revision to advance" mean
    // "wait 80 ms", which is the sleep this whole mechanism exists to remove.
    left.downloaded_bytes = 0;
    left.total_bytes = 0;
    right.downloaded_bytes = 0;
    right.total_bytes = 0;
    return left == right;
}

bool UpdaterAutomation::Publish(const exosnap::update::UpdateFlowState& state) {
    const bool advanced = !RevisionBearingEqual(state_, state);
    state_ = state;
    if (advanced)
        ++revision_;
    return advanced;
}

QJsonObject UpdaterAutomation::Identity() const {
    return identity_;
}

exosnap::update::UpdateFlowState UpdaterAutomation::State() const {
    return state_;
}

std::uint64_t UpdaterAutomation::StateRevision() const {
    return revision_;
}

bool UpdaterAutomation::Invoke(const std::function<bool(QString*)>& intent, const char* name, QString* error) {
    if (!intent) {
        // Not reachable in the shipped wiring, and it fails loudly rather than
        // quietly succeeding: a command that answers ok without doing anything
        // is the false success this whole contract is built to prevent.
        if (error != nullptr)
            *error = QStringLiteral("%1 is not wired in this process").arg(QString::fromLatin1(name));
        return false;
    }
    return intent(error);
}

bool UpdaterAutomation::Check(QString* error) {
    return Invoke(intents_.check, "updater.check", error);
}

bool UpdaterAutomation::Download(QString* error) {
    return Invoke(intents_.download, "updater.download", error);
}

bool UpdaterAutomation::Apply(QString* error) {
    return Invoke(intents_.apply, "updater.apply", error);
}

bool UpdaterAutomation::Retry(QString* error) {
    return Invoke(intents_.retry, "updater.retry", error);
}

bool UpdaterAutomation::Cancel(QString* error) {
    return Invoke(intents_.cancel, "updater.cancel", error);
}

bool UpdaterAutomation::Close(QString* error) {
    return Invoke(intents_.close, "updater.close", error);
}
