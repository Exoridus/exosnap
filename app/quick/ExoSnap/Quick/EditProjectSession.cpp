#include "EditSessionAdapter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMetaObject>
#include <QPointer>

#include <exosnap/engine/edit_player_engine.h>

#include <Windows.h>

namespace exosnap::quick {
namespace {

constexpr int kRecoveryIntervalMs = 8000;

using PathPin = std::unique_ptr<void, decltype(&CloseHandle)>;

bool PinLocalComponent(const QString& component, bool directory, bool optional, std::vector<PathPin>& pins) {
    const auto native = QDir::toNativeSeparators(component).toStdWString();
    const auto handle = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return optional && GetLastError() == ERROR_FILE_NOT_FOUND;
    PathPin pin(handle, CloseHandle);
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return false;
    pins.push_back(std::move(pin));
    return true;
}

std::vector<PathPin> PinLocalProjectSource(const QString& path) {
    const auto normal = QDir::fromNativeSeparators(path);
    if (normal.size() < 3 || normal.at(1) != QLatin1Char(':') || normal.at(2) != QLatin1Char('/') ||
        normal.mid(2).contains(QLatin1Char(':')))
        return {};
    if (GetDriveTypeW(QDir::toNativeSeparators(normal.first(3)).toStdWString().c_str()) != DRIVE_FIXED)
        return {};
    // Retain each ancestor until the path-based media APIs have opened the local files.
    std::vector<PathPin> pins;
    for (qsizetype end = normal.indexOf(QLatin1Char('/'), 3); end >= 0; end = normal.indexOf(QLatin1Char('/'), end + 1))
        if (!PinLocalComponent(normal.first(end), true, false, pins))
            return {};
    if (!PinLocalComponent(normal, false, false, pins) ||
        !PinLocalComponent(normal + QStringLiteral(".exosnap.json"), false, true, pins))
        return {};
    return pins;
}

edit::ProjectDocument CaptureDocument(const edit::EditSnapshot& snapshot, const std::array<bool, 4>& options) {
    return {snapshot, options[0], options[1], options[2], options[3]};
}

QString ProjectPath(const QString& requested) {
    if (requested.isEmpty())
        return {};
    const QFileInfo file(requested);
    if (file.isSymLink() || file.isJunction())
        return {};
    const auto canonical = file.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    const auto parent = QFileInfo(file.absolutePath()).canonicalFilePath();
    return parent.isEmpty() ? QString() : QDir(parent).filePath(file.fileName());
}

} // namespace

EditSessionAdapter::EditingOptions EditSessionAdapter::editingOptions() const {
    return {snapping, ripple, autoCrossfade, linkedSelection};
}

void EditSessionAdapter::setProjectOptions(const EditingOptions& options) {
    snapping = options[0];
    ripple = options[1];
    autoCrossfade = options[2];
    linkedSelection = options[3];
}

QString EditSessionAdapter::projectName() const {
    return project_path_.isEmpty() ? tr("Untitled project") : QFileInfo(project_path_).completeBaseName();
}

void EditSessionAdapter::enableProjectRecovery(const QString& configuration_directory) {
    project_tracking_ = true;
    const auto directory = QDir(configuration_directory).filePath(QStringLiteral("editor-recovery"));
    if (!QDir().mkpath(directory)) {
        project_message_ = tr("Automatic project recovery is unavailable.");
        emit projectChanged();
        return;
    }
    const auto path = QDir(directory).filePath(QStringLiteral("draft.exosnap"));
    auto lock = std::make_unique<QLockFile>(QDir(directory).filePath(QStringLiteral("owner.lock")));
    lock->setStaleLockTime(0);
    if (!lock->tryLock()) {
        project_message_ = tr("Automatic project recovery is in use by another ExoSnap instance.");
        emit projectChanged();
        return;
    }
    project_recovery_lock_ = std::move(lock);
    project_recovery_path_ = path;
    project_recovery_available_ = QFile::exists(path);
    connect(&project_recovery_timer_, &QTimer::timeout, this, &EditSessionAdapter::scheduleProjectRecovery);
    project_recovery_timer_.start(kRecoveryIntervalMs);
    emit projectChanged();
}

void EditSessionAdapter::projectError(edit::ProjectFileError error) {
    switch (error) {
    case edit::ProjectFileError::None:
        project_message_.clear();
        break;
    case edit::ProjectFileError::Io:
        project_message_ = tr("The project file could not be read or written.");
        break;
    case edit::ProjectFileError::Invalid:
        project_message_ = tr("The project is damaged or contains unsupported edit data.");
        break;
    case edit::ProjectFileError::UnsupportedVersion:
        project_message_ = tr("This project was saved in an unsupported format version.");
        break;
    case edit::ProjectFileError::Changed:
        project_message_ = tr("The project changed on disk. Save As to keep your edits in a separate file.");
        break;
    }
}

void EditSessionAdapter::newProject(qulonglong consent_revision) {
    if (project_busy_ || consent_revision != closeGuardRevision())
        return;
    project_tracking_ = true;
    resetWorkspace();
    recovered_project_ = false;
    project_path_.clear();
    project_digest_.clear();
    project_message_.clear();
    saved_options_ = {true, false, true, true};
    setProjectOptions(saved_options_);
    clearOwnedRecovery();
    emit interactionSettingsChanged();
    emit projectChanged();
}

void EditSessionAdapter::openProject(const QUrl& url, qulonglong consent_revision) {
    readProject(url.isLocalFile() ? url.toLocalFile() : QString(), consent_revision, false);
}

void EditSessionAdapter::recoverProject(qulonglong consent_revision) {
    if (project_recovery_available_)
        readProject(project_recovery_path_, consent_revision, true);
}

void EditSessionAdapter::readProject(const QString& requested, quint64 consent_revision, bool recovery) {
    if (project_busy_ || consent_revision != closeGuardRevision())
        return;
    project_busy_ = true;
    project_message_.clear();
    emit projectChanged();
    QPointer<EditSessionAdapter> self(this);
    project_pool_.start([self, requested, consent_revision, recovery, recovery_path = project_recovery_path_] {
        const auto path = ProjectPath(requested);
        const bool allowed =
            !path.isEmpty() && (recovery || path.compare(ProjectPath(recovery_path), Qt::CaseInsensitive) != 0);
        auto result =
            allowed ? edit::ReadProjectFile(path) : edit::ProjectReadResult{.error = edit::ProjectFileError::Io};
        edit::Workspace restored;
        if (result.error == edit::ProjectFileError::None && !restored.restoreProject(result.document.timeline))
            result.error = edit::ProjectFileError::Invalid;
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, path, consent_revision, recovery, error = result.error, digest = std::move(result.digest),
             options = EditingOptions{result.document.snapping, result.document.ripple, result.document.auto_crossfade,
                                      result.document.linked_selection},
             restored = std::move(restored)]() mutable {
                self->project_busy_ = false;
                self->projectError(error);
                if (error == edit::ProjectFileError::None && consent_revision != self->closeGuardRevision()) {
                    self->project_message_ =
                        tr("The timeline changed while opening the project. Open it again to replace it.");
                } else if (error == edit::ProjectFileError::None) {
                    self->project_tracking_ = true;
                    self->resetWorkspace();
                    if (!recovery)
                        self->clearOwnedRecovery();
                    self->workspace_ = std::move(restored);
                    self->recovered_project_ = recovery;
                    self->project_path_ = recovery ? QString() : path;
                    self->project_digest_ = recovery ? QByteArray() : std::move(digest);
                    self->initial_workspace_ = recovery ? edit::Workspace{}.snapshot() : self->workspace_.snapshot();
                    self->saved_options_ = recovery ? EditingOptions{true, false, true, true} : options;
                    self->setProjectOptions(options);
                    self->project_recovery_available_ = !recovery && self->project_recovery_available_;
                    self->publishWorkspace(true);
                    emit self->interactionSettingsChanged();
                    self->restoreProjectSources();
                }
                emit self->projectChanged();
            },
            Qt::QueuedConnection);
    });
}

void EditSessionAdapter::saveProject(const QUrl& destination) {
    if (project_busy_ || !preflightPendingDraft())
        return;
    const auto requested = destination.isEmpty()       ? project_path_
                           : destination.isLocalFile() ? destination.toLocalFile()
                                                       : QString();
    project_tracking_ = true;
    const auto snapshot = workspace_.snapshot();
    const auto options = editingOptions();
    project_busy_ = true;
    project_message_.clear();
    emit projectChanged();
    QPointer<EditSessionAdapter> self(this);
    project_pool_.start([self, requested, snapshot, options, project_path = project_path_,
                         project_digest = project_digest_, recovery_path = project_recovery_path_] {
        const auto path = ProjectPath(requested);
        const auto expected = path.compare(project_path, Qt::CaseInsensitive) == 0
                                  ? std::optional<QByteArray>(project_digest)
                                  : std::nullopt;
        QByteArray digest;
        const bool allowed = !path.isEmpty() && path.compare(ProjectPath(recovery_path), Qt::CaseInsensitive) != 0;
        const auto error = allowed ? edit::WriteProjectFile(path, CaptureDocument(*snapshot, options), expected, digest)
                                   : edit::ProjectFileError::Io;
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, path, snapshot, options, error, digest = std::move(digest)] {
                self->project_busy_ = false;
                self->projectError(error);
                if (error == edit::ProjectFileError::None) {
                    self->recovered_project_ = false;
                    self->project_path_ = path;
                    self->project_digest_ = digest;
                    self->initial_workspace_ = snapshot;
                    self->saved_options_ = options;
                    if (!self->hasUnsavedEdits())
                        self->clearOwnedRecovery();
                    emit self->unsavedEditsChanged();
                }
                emit self->projectChanged();
            },
            Qt::QueuedConnection);
    });
}

void EditSessionAdapter::clearOwnedRecovery(bool discard_pending) {
    if (!project_recovery_lock_ || (project_recovery_available_ && !discard_pending))
        return;
    project_recovery_snapshot_.reset();
    if (discard_pending) {
        project_busy_ = true;
        emit projectChanged();
    }
    const auto path = project_recovery_path_;
    QPointer<EditSessionAdapter> self(this);
    project_pool_.start([self, path, discard_pending] {
        const bool removed = !QFile::exists(path) || QFile::remove(path);
        if (!self || (removed && !discard_pending))
            return;
        QMetaObject::invokeMethod(
            self,
            [self, removed, discard_pending] {
                if (discard_pending)
                    self->project_busy_ = false;
                self->project_recovery_available_ = !removed;
                self->project_message_ =
                    removed ? QString() : tr("The project recovery snapshot could not be removed.");
                emit self->projectChanged();
            },
            Qt::QueuedConnection);
    });
}

void EditSessionAdapter::discardProjectRecovery() {
    if (project_busy_ || !project_recovery_available_)
        return;
    clearOwnedRecovery(true);
}

void EditSessionAdapter::scheduleProjectRecovery() {
    if (project_recovery_available_ || project_recovery_write_pending_ || project_busy_)
        return;
    const auto revision = closeGuardRevision();
    const auto snapshot = workspace_.snapshot();
    if (snapshot == project_recovery_snapshot_ && revision == project_recovery_revision_)
        return;
    if (!hasUnsavedEdits()) {
        if (project_recovery_snapshot_)
            clearOwnedRecovery();
        return;
    }
    project_recovery_write_pending_ = true;
    const auto path = project_recovery_path_;
    const auto options = editingOptions();
    QPointer<EditSessionAdapter> self(this);
    project_pool_.start([self, path, snapshot, options, revision] {
        QByteArray digest;
        const auto error = edit::WriteProjectFile(path, CaptureDocument(*snapshot, options), std::nullopt, digest);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, snapshot, revision, error] {
                self->project_recovery_write_pending_ = false;
                if (error == edit::ProjectFileError::None) {
                    self->project_recovery_snapshot_ = snapshot;
                    self->project_recovery_revision_ = revision;
                } else {
                    self->project_message_ = tr("The automatic project recovery snapshot could not be written.");
                    emit self->projectChanged();
                }
            },
            Qt::QueuedConnection);
    });
}

void EditSessionAdapter::finishProjectRecovery(bool discard) {
    project_recovery_timer_.stop();
    project_pool_.waitForDone();
    if (!project_recovery_lock_ || project_recovery_available_)
        return;
    if (!discard && hasUnsavedEdits()) {
        QByteArray digest;
        const auto result = edit::WriteProjectFile(
            project_recovery_path_, CaptureDocument(*workspace_.snapshot(), editingOptions()), std::nullopt, digest);
        if (result != edit::ProjectFileError::None)
            return;
    } else {
        QFile::remove(project_recovery_path_);
    }
    project_recovery_lock_.reset();
}

void EditSessionAdapter::restoreProjectSources() {
    const auto snapshot = workspace_.snapshot();
    const auto cancelled = source_cancelled_;
    const auto admission = optionalIoAdmission();
    QPointer<EditSessionAdapter> self(this);
    scheduleSourceVerification([self, snapshot, cancelled, admission] {
        for (const auto& source : snapshot->assets) {
            if (cancelled->load(std::memory_order_relaxed))
                return;
            if (!source->source_descriptor)
                continue;
            const auto& expected = source->source_descriptor->asset_identity_fingerprint;
            if (std::none_of(expected.begin(), expected.end(), [](auto byte) { return byte != 0; }))
                continue;
            const auto pins = PinLocalProjectSource(QString::fromStdWString(source->path));
            if (pins.empty())
                continue;
            const auto acquired = engine::AcquireMediaReadCapability(std::filesystem::path(source->path));
            if (!acquired.succeeded())
                continue;
            const auto access = *acquired.value();
            const auto proof = engine::VerifyMediaIdentity(access, expected, {}, admission, cancelled.get());
            if (!proof.succeeded())
                continue;
            auto metadata = engine::ProbeEditMedia(access, {}, admission, cancelled.get(), true);
            if (!metadata.error.empty())
                continue;
            auto imported = edit::DescribeSourceImport(metadata, source->duration, access);
            if (!imported.descriptor || !self)
                continue;
            QMetaObject::invokeMethod(
                self,
                [self, source, cancelled, access, proof = *proof.value(), metadata = std::move(metadata),
                 verified = std::make_shared<edit::ValidatedSourceDescriptor>(*imported.descriptor)]() mutable {
                    if (cancelled != self->source_cancelled_ ||
                        !self->workspace_.bindProjectAsset(source->id, source->source_version, std::move(verified)))
                        return;
                    self->source_publications_[source->id] = {
                        access,
                        proof,
                        std::make_shared<engine::EditMediaMetadata>(std::move(metadata)),
                        {},
                        false,
                        false,
                        source->source_version};
                    self->syncMedia(true);
                    emit self->sourceVerificationChanged(source->id);
                    emit self->workspaceChanged();
                },
                Qt::QueuedConnection);
        }
    });
}

} // namespace exosnap::quick
