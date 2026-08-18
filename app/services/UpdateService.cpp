// UpdateService.cpp -- Qt bridge implementation for the update engine.

#include "UpdateService.h"

#include <update/http_download.h>
#include <update/install_mode_detector.h>
#include <update/manifest_io.h>
#include <update/package_verifier.h>
#include <update/update_checker.h>
#include <update/update_types.h>
#include <update_handoff/handoff.h>

#include "ExoSnapBuildInfo.h" // exosnap::build::kVersion (generated from PROJECT_VERSION)
#include "RecordingCoordinator.h"
#include "UpdateCheckGate.h"
#include "WhatsNewPayload.h"

#include "../diagnostics/AppLog.h"
#include "../diagnostics/StructuredLog.h"

#include "../viewmodels/RecordViewModel.h" // for UiRecordingState

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QWinEventNotifier>
#include <atomic>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace exosnap {
namespace {

// Where prepared update transactions live. Deliberately NOT under the updater
// staging directory: LaunchUpdater wipes that tree on every launch, and the
// document plus the manifest bytes it references have to survive exactly that
// moment.
[[nodiscard]] QString TransactionsRoot() {
    const QString local_data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (local_data.isEmpty())
        return {};
    return QDir(local_data).filePath(QStringLiteral("update-transactions"));
}

// Bounded lifetime, without a heuristic: preparing a new transaction removes
// every older one EXCEPT the directory the last launched updater was handed --
// that child may still be reading it. Nothing else is kept, so an interrupted
// or failed operation leaves at most one stale directory behind.
void PruneTransactions(const QString& keep_new, const QString& keep_in_flight) {
    const QString root = TransactionsRoot();
    if (root.isEmpty())
        return;
    const QDir dir(root);
    if (!dir.exists())
        return;
    for (const QString& name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString path = dir.filePath(name);
        if (path == keep_new || path == keep_in_flight)
            continue;
        QDir(path).removeRecursively();
    }
}

// Fetch the release trust anchor for an offered version: the exact manifest
// bytes and their detached signature, into a fresh transaction directory.
//
// Nothing here verifies anything. The application is not the trust boundary --
// it downloads the bytes because it, not the updater, owns release resolution
// under the handoff contract, and it hands the PATHS over. The updater re-reads
// those bytes and verifies the signature itself before parsing a single field.
[[nodiscard]] UpdateService::PreparedUpdate PrepareUpdateTransaction(const exosnap::update::UpdateCheckResult& result) {
    UpdateService::PreparedUpdate prepared;
    prepared.target_version = QString::fromStdString(result.available_version_raw);

    const QString root = TransactionsRoot();
    if (root.isEmpty()) {
        prepared.error = QStringLiteral("Can't locate a directory to prepare the update in.");
        return prepared;
    }
    if (result.manifest_url.empty() || result.manifest_signature_url.empty()) {
        prepared.error = QStringLiteral("The offered release carries no signed update manifest.");
        return prepared;
    }

    prepared.update_transaction_id = exosnap::update_handoff::MakeUpdateTransactionId();
    prepared.directory = QDir(root).filePath(prepared.update_transaction_id);
    if (!QDir().mkpath(prepared.directory)) {
        prepared.error = QStringLiteral("Can't create the update transaction directory.");
        return prepared;
    }
    prepared.manifest_path =
        QDir(prepared.directory).filePath(QString::fromLatin1(exosnap::update_handoff::kManifestFileName));
    prepared.manifest_signature_path =
        QDir(prepared.directory).filePath(QString::fromLatin1(exosnap::update_handoff::kManifestSignatureFileName));

    // Never cancelled: these are two sub-kilobyte GETs on a worker that already
    // ran a feed request. There is no user-facing operation to cancel here.
    const std::atomic<bool> no_cancel{false};
    const std::pair<std::string, QString> assets[] = {
        {result.manifest_url, prepared.manifest_path},
        {result.manifest_signature_url, prepared.manifest_signature_path},
    };
    for (const auto& [url, destination] : assets) {
        if (const auto error = exosnap::update::DownloadToFile(
                url, QDir::toNativeSeparators(destination).toStdWString(), {}, no_cancel)) {
            prepared.error =
                QStringLiteral("Can't fetch the signed update manifest: %1").arg(QString::fromStdString(*error));
            return prepared;
        }
    }
    return prepared;
}

} // namespace

// ---------------------------------------------------------------------------
// Internal implementation
// ---------------------------------------------------------------------------
class UpdateService::Impl {
  public:
    RecordingCoordinator* coordinator = nullptr;
    exosnap::update::UpdateChannel channel = exosnap::update::UpdateChannel::Stable;
    exosnap::update::InstallMode install_mode{};
    exosnap::update::UpdateState state{};
    // QCR-202. Single-flight admission and completion attribution are the same
    // fact, so they are the same field under the same mutex: a check is in
    // flight exactly while `active_operation` is non-zero, and only the
    // operation that still matches it may publish. `checking` used to be an
    // atomic the worker cleared *before* it wrote its results, which let a
    // second request start while the first was still publishing — two workers
    // writing `state` at once, and a UI that said "not checking" the whole time.
    std::uint64_t next_operation_id = 1;
    std::uint64_t active_operation = 0;
    // ADR 0055. Atomic because SetVerifyReinstallMode/IsVerifyReinstallMode run
    // outside the mutex; its value is snapshotted into the operation context at
    // check start so a toggle cannot change what a running check means.
    std::atomic<bool> verify_reinstall{false};
    // Both mutex-guarded: they are read by the check worker on a pool thread and
    // written from the GUI thread at startup.
    QString dev_feed_override;
    QString updater_automation_run_id;
    UpdateService::UpdaterLaunchInfo last_updater_launch;
    // What the last adopted check prepared for a handoff. Replaced wholesale by
    // each adopted check, so it can never describe an offer the card has moved
    // past.
    UpdateService::PreparedUpdate prepared_update;
    mutable QMutex mutex;

    // WHATS-NEW: gap notes from the most recent completed check (mutex-guarded).
    std::vector<exosnap::update::ReleaseNote> gap_notes;

    // WHATS-NEW: the full channel-history notes from the most recent completed check
    // (mutex-guarded, mirrors gap_notes).
    std::vector<exosnap::update::ReleaseNote> all_channel_notes;

#if defined(_WIN32)
    HANDLE updater_process = nullptr;
    QWinEventNotifier* updater_exit_notifier = nullptr;
    qint64 updater_pid = 0;
#endif

    // Build the recording guard from the RecordingCoordinator's public API.
    // Reads `coordinator` under `mutex` since SetRecordingCoordinator() (called from
    // MainWindow on the UI thread once RecordPage finishes its deferred init) can
    // race with RequestUpdateCheck()'s background worker thread capturing the guard.
    exosnap::update::RecordingGuardFn MakeGuard() const {
        RecordingCoordinator* coord;
        {
            QMutexLocker lk(&mutex);
            coord = coordinator;
        }
        return [coord]() -> exosnap::update::UpdateBlockReason {
            if (!coord)
                return exosnap::update::UpdateBlockReason::NotBlocked;
            auto s = coord->State();
            if (s == UiRecordingState::Saving || s == UiRecordingState::Stopping)
                return exosnap::update::UpdateBlockReason::Finalizing;
            if (s == UiRecordingState::Recording || s == UiRecordingState::Paused ||
                s == UiRecordingState::ArmedFromRecovery || s == UiRecordingState::Preparing ||
                s == UiRecordingState::Countdown)
                return exosnap::update::UpdateBlockReason::ActiveRecording;
            return exosnap::update::UpdateBlockReason::NotBlocked;
        };
    }

    // Declared as the LAST data member so it is destroyed FIRST: its destructor
    // waits for the check worker, which writes `state`/`gap_notes` above under
    // `mutex` once CheckForUpdate returns. The previous worker was a detached
    // QThread nobody joined, so ~UpdateService's `delete impl_` could free that
    // mutex and that state while the worker was still about to lock and write
    // them. Never add a data member below this one.
    //
    // The wait has no deadline, because the engine offers none: CheckForUpdate
    // does a WinHTTP GET that installs no WinHttpSetTimeouts call and exposes
    // no cancel handle, so it ends only on WinHTTP's own connect/send/receive
    // defaults. Giving it a deadline is an update-engine policy decision and is
    // deliberately not invented here.
    QThreadPool check_pool;
};

// ---------------------------------------------------------------------------
// UpdateService
// ---------------------------------------------------------------------------
UpdateService::UpdateService(RecordingCoordinator* coordinator, QObject* parent) : QObject(parent), impl_(new Impl) {
    impl_->coordinator = coordinator;
    impl_->install_mode = exosnap::update::DetectInstallMode();
    impl_->state.channel = impl_->channel;
    impl_->state.install_mode = impl_->install_mode;
}

UpdateService::~UpdateService() {
#if defined(_WIN32)
    if (impl_->updater_exit_notifier != nullptr)
        impl_->updater_exit_notifier->setEnabled(false);
    if (impl_->updater_process != nullptr)
        ::CloseHandle(impl_->updater_process);
#endif
    // This is also the join: Impl's last member is a QThreadPool whose
    // destructor waits for the check worker, and it runs before the `mutex` and
    // `state` that worker writes are destroyed. Nothing the worker can touch
    // may be torn down above this line.
    delete impl_;
}

void UpdateService::SetRecordingCoordinator(RecordingCoordinator* coordinator) {
    QMutexLocker lk(&impl_->mutex);
    impl_->coordinator = coordinator;
}

exosnap::update::UpdateChannel UpdateService::Channel() const {
    // Under the mutex like every other read of it: SetChannel can run on the GUI
    // thread while a check worker is reading the same field.
    QMutexLocker lk(&impl_->mutex);
    return impl_->channel;
}

void UpdateService::SetChannel(exosnap::update::UpdateChannel ch) {
    QMutexLocker lk(&impl_->mutex);
    impl_->channel = ch;
    // "Update available — 1.2.0" was an answer about the previous feed; keeping
    // it under the new channel's label is the same misattribution the
    // completion gate prevents, only from the other direction.
    impl_->state = ApplyUpdateChannelSelection(impl_->state, ch);
}

void UpdateService::SetVerifyReinstallMode(bool on) {
    impl_->verify_reinstall.store(on);
    if (on)
        diagnostics::AppLog::info(
            QStringLiteral("update"),
            QStringLiteral("Verification reinstall mode active — the running version %1 may be reinstalled "
                           "through the normal update path (this run only; nothing was persisted)")
                .arg(QString::fromLatin1(exosnap::build::kVersion)));
}

bool UpdateService::IsVerifyReinstallMode() const {
    return impl_->verify_reinstall.load();
}

void UpdateService::SetDevFeedOverride(const QString& base_url) {
    {
        QMutexLocker lk(&impl_->mutex);
        impl_->dev_feed_override = base_url;
    }
    if (!base_url.isEmpty()) {
        // Loud on purpose: every check and every updater launch in this run
        // talks to a feed that is not the product's. A support bundle from such
        // a run must say so.
        diagnostics::AppLog::warning(
            QStringLiteral("update"),
            QStringLiteral("Update feed overridden for this run — checks and the updater both use %1").arg(base_url));
    }
}

QString UpdateService::DevFeedOverride() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->dev_feed_override;
}

void UpdateService::SetUpdaterAutomationRunId(const QString& run_id) {
    QMutexLocker lk(&impl_->mutex);
    impl_->updater_automation_run_id = run_id;
}

QString UpdateService::UpdaterAutomationRunId() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->updater_automation_run_id;
}

UpdateService::UpdaterLaunchInfo UpdateService::LastUpdaterLaunch() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->last_updater_launch;
}

UpdateService::PreparedUpdate UpdateService::LastPreparedUpdate() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->prepared_update;
}

exosnap::update::UpdateBlockReason UpdateService::CurrentBlockReason() const {
    auto guard = impl_->MakeGuard();
    return guard ? guard() : exosnap::update::UpdateBlockReason::NotBlocked;
}

exosnap::update::UpdateState UpdateService::CurrentState() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->state;
}

std::vector<exosnap::update::ReleaseNote> UpdateService::LastGapNotes() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->gap_notes;
}

std::vector<exosnap::update::ReleaseNote> UpdateService::LastAllChannelNotes() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->all_channel_notes;
}

void UpdateService::RequestUpdateCheck() {
    auto* impl = impl_;
    auto* self = this;

    // QCR-202. Admission and the operation context are one critical section: the
    // channel and the verification mode are read here, on the requesting thread,
    // and travel with the operation. Reading them inside the worker meant the
    // check could query one feed and be interpreted as another.
    UpdateCheckOperation operation;
    QString feed_override;
    {
        QMutexLocker lk(&impl_->mutex);
        if (impl_->active_operation != 0)
            return; // already in progress
        operation.id = impl_->next_operation_id++;
        operation.channel = impl_->channel;
        operation.verify_reinstall = impl_->verify_reinstall.load();
        // Snapshotted with the rest of the operation context, for the same
        // reason: which feed a check ran against must not be able to change
        // while it runs.
        feed_override = impl_->dev_feed_override;
        impl_->active_operation = operation.id;
        impl_->state.checking = true;
        impl_->state.channel = operation.channel;
    }

    impl_->check_pool.start([impl, self, operation, feed_override]() {
        namespace upd = exosnap::update;

        upd::CheckParams params;
        params.current_version = upd::ParseSemVer(exosnap::build::kVersion).value_or(upd::SemVer{0, 0, 0});
        // kVersion is treated as an opaque full version string ("0.9.0-rc4",
        // "0.9.0-dev"): the verification gate compares it byte-for-byte, so a
        // build whose identity does not match any release tag simply never
        // qualifies — which is the safe direction.
        params.current_version_raw = exosnap::build::kVersion;
        params.allow_same_version_reinstall = operation.verify_reinstall;
        params.channel = operation.channel;
        params.recording_guard = impl->MakeGuard();

        // With a dev feed override the recording guard still applies -- it is a
        // product rule about what the machine is doing, not about which feed is
        // being read -- but the official-build gate does not: that gate is a
        // policy about the PRODUCTION feed, and this is by construction not it.
        // Without this branch a dev build could never exercise its own check,
        // and therefore never the app-to-updater handoff either.
        upd::UpdateCheckResult result;
        if (feed_override.isEmpty()) {
            result = upd::CheckForUpdate(params);
        } else {
            params.api_base_url = feed_override.toStdString();
            const upd::UpdateBlockReason blocked =
                params.recording_guard ? params.recording_guard() : upd::UpdateBlockReason::NotBlocked;
            if (blocked != upd::UpdateBlockReason::NotBlocked) {
                result.check_failed = true;
                result.error_message = blocked == upd::UpdateBlockReason::ActiveRecording
                                           ? "Update check blocked: recording in progress"
                                           : "Update check blocked: recording finalizing";
            } else {
                result = upd::CheckAgainstFeed(params);
            }
        }

        // Prepare the handoff for whatever this check found, BEFORE anything is
        // published: resolving a release and fetching the bytes that prove it
        // are one act, and the application owns both under the handoff contract.
        // A failure here does not hide the update -- a release that exists and
        // is newer must never be reported as "up to date" -- it is recorded and
        // refuses the apply with a truthful reason instead.
        std::optional<UpdateService::PreparedUpdate> prepared;
        if (result.update_available)
            prepared = PrepareUpdateTransaction(result);

        UpdateCheckCompletion completion;
        upd::UpdateChannel channel_now = operation.channel;
        QString prune_keep_new;
        QString prune_keep_in_flight;
        bool prune = false;
        {
            QMutexLocker lk(&impl->mutex);
            completion =
                ResolveUpdateCheckCompletion(impl->state, operation, impl->active_operation, impl->channel, result);
            if (operation.id == impl->active_operation)
                impl->active_operation = 0;
            if (completion.publish)
                impl->state = completion.state;
            if (completion.verdict == UpdateCompletionVerdict::Adopt) {
                // Wholesale replacement, including with an empty one: a
                // preparation that belonged to a previous offer must not survive
                // into a check that offers something else (or nothing).
                impl->prepared_update = prepared.value_or(UpdateService::PreparedUpdate{});
                prune_keep_new = impl->prepared_update.directory;
                prune_keep_in_flight = impl->last_updater_launch.handoff_path.isEmpty()
                                           ? QString()
                                           : QFileInfo(impl->last_updater_launch.handoff_path).absolutePath();
                prune = true;
            }
            if (completion.adopt_notes) {
                impl->gap_notes = result.gap_notes;
                impl->all_channel_notes = result.all_channel_notes;
            } else if (completion.verdict == UpdateCompletionVerdict::ChannelChanged) {
                // Notes for a feed the user has left must not survive into the
                // pending "What's new" payload LaunchUpdater writes.
                impl->gap_notes.clear();
                impl->all_channel_notes.clear();
            }
            channel_now = impl->channel;
        }

        // Outside the mutex: filesystem work, and nothing above depends on it.
        if (prune)
            PruneTransactions(prune_keep_new, prune_keep_in_flight);
        // A prepared transaction this check does NOT own (its result was
        // discarded) is removed here rather than left to accumulate.
        if (!prune && prepared.has_value() && !prepared->directory.isEmpty())
            QDir(prepared->directory).removeRecursively();
        if (prune && prepared.has_value() && !prepared->error.isEmpty()) {
            // Loud, because the card still offers the update and the apply will
            // refuse: a support bundle has to show why.
            diagnostics::AppLog::warning(
                QStringLiteral("update"),
                QStringLiteral("Version %1 is on offer, but its update handoff could not be prepared: %2")
                    .arg(QString::fromStdString(result.available_version_raw), prepared->error));
        }

        switch (completion.verdict) {
        case UpdateCompletionVerdict::SupersededByNewerCheck:
            // The newer operation owns the state and will publish its own; this
            // one says nothing at all.
            diagnostics::AppLog::info(
                QStringLiteral("update"),
                QStringLiteral("Discarded the result of update check %1: a newer check has since started")
                    .arg(operation.id));
            return;
        case UpdateCompletionVerdict::ChannelChanged:
            diagnostics::AppLog::info(
                QStringLiteral("update"),
                QStringLiteral("Discarded the result of update check %1: it ran on the %2 channel and the "
                               "selection has since moved to %3")
                    .arg(operation.id)
                    .arg(UpdateChannelToString(operation.channel), UpdateChannelToString(channel_now)));
            break;
        case UpdateCompletionVerdict::Adopt:
            break;
        }

        const bool deliver_result = completion.verdict == UpdateCompletionVerdict::Adopt;
        const auto operation_channel = operation.channel;
        QMetaObject::invokeMethod(
            self,
            [self, result, deliver_result, operation_channel]() {
                // Checked again here, not only in the worker: this call was
                // queued, so the channel can have changed between the worker
                // publishing and the GUI thread delivering. Without this the
                // card could still be filled in from the other feed's answer.
                if (deliver_result && self->Channel() == operation_channel)
                    emit self->updateCheckComplete(result);
                emit self->updateStateChanged(self->CurrentState());
            },
            Qt::QueuedConnection);
    });
}

void UpdateService::LaunchUpdater() {
    namespace upd = exosnap::update;

    // Recording guard: never start a swap while a capture or MP4 remux is in
    // flight (same policy as the check path). The UI keeps the button disabled,
    // but guard defensively here too.
    if (CurrentBlockReason() != upd::UpdateBlockReason::NotBlocked) {
        emit updateError(upd::VerifyResult::PackageNotFound,
                         QStringLiteral("Can't update while recording or finalizing."));
        return;
    }

    const QString app_dir = QCoreApplication::applicationDirPath();

    // The handoff precondition, checked before a single file is staged: this
    // process only ever hands over the EXACT version the card is offering,
    // prepared without error. Refusing here is what stops an updater from being
    // started for a transaction that describes a different release.
    const upd::UpdateState state_now = CurrentState();
    const PreparedUpdate prepared = LastPreparedUpdate();
    if (const QString refusal = HandoffRefusalReason(state_now, prepared); !refusal.isEmpty()) {
        emit updateError(upd::VerifyResult::PackageNotFound, refusal);
        return;
    }

    // Stage into %LOCALAPPDATA%\<org>\ExoSnap\updater\ (AppLocalDataLocation +
    // "/updater"). Running the updater from a separate directory lets it replace
    // the app's own files while the app is still shutting down.
    const QString local_data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (local_data.isEmpty()) {
        emit updateError(upd::VerifyResult::PackageNotFound,
                         QStringLiteral("Can't locate a staging directory for the updater."));
        return;
    }
    const QString staging_dir = QDir(local_data).filePath(QStringLiteral("updater"));

    // Wipe any prior staging and recreate a clean tree.
    QDir staging(staging_dir);
    if (staging.exists())
        staging.removeRecursively();
    if (!QDir().mkpath(staging_dir)) {
        emit updateError(upd::VerifyResult::PackageNotFound,
                         QStringLiteral("Can't create the updater staging directory."));
        return;
    }

    // QCR-203. A staging step that fails must not leave a half-built updater
    // behind in %LOCALAPPDATA% for the user to find and double-click. Every
    // required-step failure exits through here.
    const auto fail_staging = [this, staging_dir](const QString& detail) {
        QDir(staging_dir).removeRecursively();
        emit updateError(exosnap::update::VerifyResult::PackageNotFound, detail);
    };

    // Copy the mandatory runtime subset; a missing entry is a hard failure.
    for (const QString& rel : UpdaterStagingFileList()) {
        const QString src = QDir(app_dir).filePath(rel);
        const QString dst = QDir(staging_dir).filePath(rel);
        if (!QFileInfo::exists(src)) {
            fail_staging(QStringLiteral("Updater runtime file missing: %1").arg(rel));
            return;
        }
        QDir().mkpath(QFileInfo(dst).absolutePath());
        if (!QFile::copy(src, dst)) {
            fail_staging(QStringLiteral("Failed to stage updater file: %1").arg(rel));
            return;
        }
    }

    // Best-effort, and genuinely optional: without the styles plugin the updater
    // falls back to Qt's built-in style. It looks different; it still runs and
    // still swaps the installation correctly.
    const QDir styles_src(QDir(app_dir).filePath(QStringLiteral("plugins/styles")));
    if (styles_src.exists()) {
        const QString styles_dst_dir = QDir(staging_dir).filePath(QStringLiteral("plugins/styles"));
        QDir().mkpath(styles_dst_dir);
        for (const QFileInfo& fi : styles_src.entryInfoList(QStringList{QStringLiteral("*.dll")}, QDir::Files)) {
            if (!QFile::copy(fi.absoluteFilePath(), QDir(styles_dst_dir).filePath(fi.fileName()))) {
                diagnostics::AppLog::warning(
                    QStringLiteral("update"),
                    QStringLiteral("Optional updater style plugin %1 was not staged; the updater will use Qt's "
                                   "built-in style.")
                        .arg(fi.fileName()));
            }
        }
    }

    // QCR-203, required: the platform plugin is staged to plugins/platforms/,
    // and Qt's default library paths cover the application directory and the
    // *build-time* Qt plugins path — neither of which exists on a user machine.
    // Without this file the staged updater cannot load qwindows.dll and dies at
    // startup with "could not load the Qt platform plugin". ADR 0037 §E treats
    // it as part of the staging contract for exactly that reason: the packaging
    // gate's updater smoke reproduces this write verbatim. So its failure is a
    // staging failure, not something to shrug at — the previous code checked
    // only whether the file opened and ignored the write and the close.
    {
        const QByteArray qt_conf_bytes("[Paths]\nPlugins = plugins\n");
        QFile qt_conf(QDir(staging_dir).filePath(QStringLiteral("qt.conf")));
        if (!qt_conf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fail_staging(QStringLiteral("Failed to write the updater's qt.conf: %1").arg(qt_conf.errorString()));
            return;
        }
        const qint64 written = qt_conf.write(qt_conf_bytes);
        // flush() before close(): QFile::close() reports nothing, and a buffered
        // write that fails on flush would otherwise pass unnoticed — the same
        // failure mode QCR-108 found in the output-folder probe.
        const bool flushed = qt_conf.flush();
        qt_conf.close();
        if (written != qt_conf_bytes.size() || !flushed || qt_conf.error() != QFileDevice::NoError) {
            fail_staging(QStringLiteral("Failed to write the updater's qt.conf: %1").arg(qt_conf.errorString()));
            return;
        }
    }

    const QString staged_exe = QDir(staging_dir).filePath(QStringLiteral("exosnap-updater.exe"));

    // WHATS-NEW: persist the gap notes as a pending payload BEFORE launching, so
    // the one-time "What's new" overlay can show on the first launch of the new
    // build. Only meaningful when the last check produced notes for the target
    // version. If there is nothing to show, clear any stale payload instead.
    {
        std::vector<upd::ReleaseNote> notes;
        // The SAME snapshot the handoff document is built from, so the payload
        // can only ever describe the version the updater is allowed to install.
        // Re-reading the live state here would open a window in which a check
        // completing mid-launch made the notes describe one version and the
        // handoff pin another; reading available_version->ToString() instead
        // would additionally re-spell a foreign prerelease label.
        const std::string target_raw = state_now.available_version_raw;
        {
            QMutexLocker lk(&impl_->mutex);
            notes = impl_->gap_notes;
        }
        const QString payload_path = WhatsNewPayloadPath();
        if (!target_raw.empty() && !notes.empty()) {
            WhatsNewPendingPayload payload;
            payload.target_version = QString::fromStdString(target_raw);
            for (const auto& n : notes) {
                WhatsNewNote note;
                note.version = QString::fromStdString(n.version.ToString());
                note.body = QString::fromStdString(n.body_markdown);
                note.html_url = QString::fromStdString(n.html_url);
                payload.notes.push_back(note);
            }
            // QCR-203, optional: this payload is presentation only. Losing it
            // costs the one-time "What's new" overlay on the next launch and
            // nothing else — the update itself, its verification and the swap
            // are entirely unaffected — so a failure is reported, not fatal.
            // Aborting the update over release notes would be the worse bug.
            if (!WriteWhatsNewPayload(payload_path, payload)) {
                diagnostics::AppLog::warning(
                    QStringLiteral("update"),
                    QStringLiteral("Could not write the pending \"What's new\" payload to %1; the update "
                                   "continues, but the post-update notes overlay will not appear.")
                        .arg(payload_path));
                // A stale payload from an earlier update would now claim to
                // describe this one, so it goes rather than being left behind.
                DeleteWhatsNewPayload(payload_path);
            }
        } else {
            DeleteWhatsNewPayload(payload_path);
        }
    }

#if defined(_WIN32)
    const quint32 pid = static_cast<quint32>(::GetCurrentProcessId());
    const bool verify_reinstall = impl_->verify_reinstall.load();
    if (verify_reinstall)
        diagnostics::AppLog::info(QStringLiteral("update"),
                                  QStringLiteral("Launching the updater in verification reinstall mode — it will "
                                                 "refuse any version but %1")
                                      .arg(QString::fromLatin1(exosnap::build::kVersion)));
    // The document, then the command line that points at it. Written atomically
    // so the child can never observe a partial one: it is created as a temporary
    // sibling and renamed over the final name.
    const exosnap::update_handoff::UpdateHandoff handoff = BuildUpdateHandoff(
        state_now, prepared, app_dir, pid, QString::fromLatin1(exosnap::build::kVersion), verify_reinstall);
    const QString handoff_path =
        QDir(prepared.directory).filePath(QString::fromLatin1(exosnap::update_handoff::kHandoffFileName));
    QString handoff_error;
    if (!exosnap::update_handoff::WriteUpdateHandoffAtomically(handoff_path, handoff, &handoff_error)) {
        emit updateError(upd::VerifyResult::PackageNotFound,
                         QStringLiteral("Can't write the update handoff: %1").arg(handoff_error));
        return;
    }
    diagnostics::AppLog::info(
        QStringLiteral("update"),
        QStringLiteral("Update transaction %1 hands version %2 to the updater (handoff schema %3)")
            .arg(handoff.update_transaction_id, handoff.target_version)
            .arg(exosnap::update_handoff::kHandoffVersion));

    const QStringList flags = BuildUpdaterArgs(QDir::toNativeSeparators(handoff_path), UpdaterAutomationRunId());

    // Launch detached: the app does not wait — the updater sends WM_CLOSE when it is
    // ready to swap. QProcess applies the correct Windows argument-quoting rules so
    // paths/args with spaces or quotes are passed through safely.
    qint64 updater_pid = 0;
    const bool ok = QProcess::startDetached(QDir::toNativeSeparators(staged_exe), flags,
                                            QDir::toNativeSeparators(staging_dir), &updater_pid);
    if (!ok) {
        emit updateError(upd::VerifyResult::PackageNotFound, QStringLiteral("Failed to launch the updater."));
        return;
    }

    // Keep an event-driven watch on the detached process while the old app is
    // alive. If the user closes a failed/cancelled updater before the marked
    // close handoff, MainWindow can immediately re-arm the card. This is process
    // lifecycle ownership, not UI polling.
    impl_->updater_pid = updater_pid;
    {
        // Recorded BEFORE the exit watcher is wired, so a child that dies
        // immediately still leaves behind what it was.
        QMutexLocker lk(&impl_->mutex);
        impl_->last_updater_launch.pid = updater_pid;
        impl_->last_updater_launch.staged_exe = QDir::toNativeSeparators(staged_exe);
        // Read directly, not through CurrentState(): that helper takes the same
        // non-recursive mutex this block already holds.
        impl_->last_updater_launch.target_version = handoff.target_version;
        impl_->last_updater_launch.automation_run_id = impl_->updater_automation_run_id;
        // The operation and the document it travelled in. Reported rather than
        // derivable: a check reads the transaction id to correlate the two
        // processes and the path to see exactly what was handed over.
        impl_->last_updater_launch.update_transaction_id = handoff.update_transaction_id;
        impl_->last_updater_launch.handoff_path = QDir::toNativeSeparators(handoff_path);
    }
    // The same fact as the AppLog line above, but structured and keyed by the
    // transaction id, so `events.recent?updateTransactionId=...` returns this
    // process's half of the update without anyone parsing prose. No path is
    // carried: the handoff document's location is an implementation detail of
    // this launch, and the launch snapshot already reports it to the one client
    // that has a reason to look at it.
    diagnostics::logEvent(diagnostics::LogSeverity::Info, "update", "update.updaterLaunched",
                          {{"updateTransactionId", handoff.update_transaction_id.toStdString()},
                           {"targetVersion", handoff.target_version.toStdString()},
                           {"updaterPid", std::to_string(updater_pid)}});
    impl_->updater_process =
        ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(updater_pid));
    if (impl_->updater_process != nullptr) {
        impl_->updater_exit_notifier = new QWinEventNotifier(impl_->updater_process, this);
        connect(impl_->updater_exit_notifier, &QWinEventNotifier::activated, this, [this](HANDLE) {
            DWORD exit_code = 0;
            if (!::GetExitCodeProcess(impl_->updater_process, &exit_code))
                exit_code = static_cast<DWORD>(-1);

            impl_->updater_exit_notifier->setEnabled(false);
            impl_->updater_exit_notifier->deleteLater();
            impl_->updater_exit_notifier = nullptr;
            ::CloseHandle(impl_->updater_process);
            impl_->updater_process = nullptr;

            const qint64 process_id = impl_->updater_pid;
            impl_->updater_pid = 0;
            emit updaterExited(process_id, static_cast<quint32>(exit_code));
        });
    } else {
        diagnostics::AppLog::warning(
            QStringLiteral("update"),
            QStringLiteral("Updater launched, but its process lifetime could not be observed (pid %1).")
                .arg(updater_pid));
    }
    emit updaterLaunched();
#else
    emit updateError(upd::VerifyResult::PackageNotFound, QStringLiteral("The updater is only available on Windows."));
#endif
}

void UpdateService::HandoffToInstaller(const QString& installer_path) {
    namespace upd = exosnap::update;
    bool ok = upd::HandoffToInstaller(installer_path.toStdString());
    if (!ok) {
        emit updateError(upd::VerifyResult::PackageNotFound, "Failed to launch installer");
    }
}

} // namespace exosnap
