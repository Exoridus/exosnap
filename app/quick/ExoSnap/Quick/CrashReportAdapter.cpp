#include "CrashReportAdapter.h"

#include <QVariantMap>

namespace exosnap::quick {
namespace {

QVariantMap Row(const QString& label, const QString& value) {
    QVariantMap row;
    row.insert(QStringLiteral("label"), label);
    row.insert(QStringLiteral("value"), value);
    return row;
}

} // namespace

CrashReportAdapter::CrashReportAdapter(QObject* parent) : QObject(parent) {
}

bool CrashReportAdapter::active() const noexcept {
    return active_;
}

bool CrashReportAdapter::recordingWasActive() const noexcept {
    return context_.recording_was_active;
}

QString CrashReportAdapter::availabilityText() const {
    // Both variants end on the same promise, because it is the one thing the
    // user needs to read before deciding anything.
    return context_.dump_available
               ? QStringLiteral("A local crash dump is available and can help determine the cause. Nothing is sent "
                                "unless you choose to.")
               : QStringLiteral("Only limited session context is available. Nothing is sent unless you choose to.");
}

const QVariantList& CrashReportAdapter::summaryRows() const noexcept {
    return summary_rows_;
}

QStringList CrashReportAdapter::includedItems() const {
    // The native dump is a separate binary channel, so paths and usernames are
    // deliberately not promised absent here — see channelNote().
    return {
        QStringLiteral("Native crash dump, when available"),
        QStringLiteral("Crash and stack information"),
        QStringLiteral("Encoder, container, video and audio codec context"),
    };
}

QStringList CrashReportAdapter::excludedItems() const {
    return {
        QStringLiteral("Recordings or recording content"),
        QStringLiteral("Output files"),
        QStringLiteral("Settings or presets"),
        QStringLiteral("Application logs"),
    };
}

QString CrashReportAdapter::channelNote() const {
    return QStringLiteral("Sent to Sentry's EU region. The native dump is separate from the privacy-scrubbed "
                          "structured event and can include loaded-module paths, including the ExoSnap install "
                          "path. Your IP address is used in transit; Sentry is configured not to store it.");
}

bool CrashReportAdapter::rememberChoice() const noexcept {
    return remember_choice_;
}

bool CrashReportAdapter::crashFolderAvailable() const noexcept {
    return crash_folder_available_;
}

void CrashReportAdapter::setRememberChoice(bool remember) {
    if (remember_choice_ == remember)
        return;
    remember_choice_ = remember;
    emit rememberChoiceChanged();
}

void CrashReportAdapter::present(const CrashReportContext& context, bool crash_folder_available) {
    context_ = context;
    crash_folder_available_ = crash_folder_available;
    // Every presentation starts from the privacy-by-default draft, so a prior
    // session's tick can never carry into a fresh decision.
    remember_choice_ = false;
    active_ = true;
    rebuildSummaryRows();
    emit rememberChoiceChanged();
    emit changed();
}

void CrashReportAdapter::rebuildSummaryRows() {
    summary_rows_.clear();
    summary_rows_.append(Row(QStringLiteral("SESSION"), QStringLiteral("Did not shut down normally")));
    summary_rows_.append(Row(QStringLiteral("CRASH DUMP"),
                             context_.dump_available ? QStringLiteral("Available") : QStringLiteral("Unavailable")));
    // The client never symbolicates: the dump holds the rest and Sentry resolves
    // stacks against the PDBs. Saying so is honest; showing an empty "CAUSE"
    // row would read as a value nobody bothered to fill in.
    summary_rows_.append(Row(QStringLiteral("CAUSE"), QStringLiteral("Not available locally")));
    if (!context_.version.trimmed().isEmpty())
        summary_rows_.append(Row(QStringLiteral("VERSION"), context_.version));
    if (!context_.encoder.trimmed().isEmpty())
        summary_rows_.append(Row(QStringLiteral("ENCODER"), context_.encoder));
}

void CrashReportAdapter::commit(CrashReportAction action) {
    if (!active_)
        return;
    const CrashReportDecision decision = ResolveCrashReportDecision(action, remember_choice_);
    active_ = false;
    emit changed();
    emit decisionMade(decision, action == CrashReportAction::SendReport);
}

void CrashReportAdapter::sendReport() {
    commit(CrashReportAction::SendReport);
}

void CrashReportAdapter::dontSend() {
    commit(CrashReportAction::DontSend);
}

void CrashReportAdapter::openCrashFolder() {
    if (!active_ || !crash_folder_available_)
        return;
    // Deliberately does NOT close the surface: looking at the folder is how a
    // user decides whether to send, so the decision has to still be there when
    // they come back.
    emit openCrashFolderRequested();
}

void CrashReportAdapter::dismiss() {
    // Dismiss commits nothing — not the remember tick, not a consent change.
    // ResolveCrashReportDecision(Dismiss, …) returns an empty decision for
    // exactly this reason, so the next launch asks again.
    commit(CrashReportAction::Dismiss);
}

} // namespace exosnap::quick
