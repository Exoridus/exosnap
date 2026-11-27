#include "EditSessionAdapter.h"
#include "models/EditProjectMedia.h"
#include "services/EditProjectStore.h"
#include "settings/ConfigPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <exosnap/engine/edit_player_engine.h>

namespace exosnap::quick {
namespace {
std::array<uint8_t, 32> ExpectedIdentity(const edit::Asset& asset) {
    if (asset.source_descriptor)
        return asset.source_descriptor->asset_identity_fingerprint;
    return asset.legacy_content_sha256.value_or(std::array<uint8_t, 32>{});
}

bool KnownIdentity(const std::array<uint8_t, 32>& identity) {
    return std::any_of(identity.begin(), identity.end(), [](uint8_t byte) { return byte != 0; });
}

template <class Publications>
void BindProjectIdentities(edit::EditSnapshot& captured, const Publications& sources, bool verify_pending,
                           const engine::BackgroundIoAdmission& admission,
                           const std::shared_ptr<std::atomic<bool>>& generation) {
    for (auto& source : captured.assets) {
        const auto publication = sources.find(source->id);
        if ((KnownIdentity(ExpectedIdentity(*source)) && !source->mix_decision_required) ||
            publication == sources.end() || !publication->second.access ||
            publication->second.version != source->source_version)
            continue;
        auto proof = publication->second.proof;
        if (!proof && verify_pending) {
            const auto verified = engine::VerifyMediaIdentity(
                publication->second.access,
                KnownIdentity(ExpectedIdentity(*source)) ? std::optional{ExpectedIdentity(*source)} : std::nullopt, {},
                admission, generation.get());
            if (verified.succeeded())
                proof = *verified.value();
        }
        if (!proof ||
            (KnownIdentity(ExpectedIdentity(*source)) && ExpectedIdentity(*source) != proof->Identity().sha256))
            continue;
        auto asset = std::make_shared<edit::Asset>(*source);
        if (asset->source_descriptor) {
            auto descriptor = std::make_shared<edit::ValidatedSourceDescriptor>(*asset->source_descriptor);
            descriptor->asset_identity_fingerprint = proof->Identity().sha256;
            asset->source_descriptor = std::move(descriptor);
            if (asset->input_mix_selection)
                asset->input_mix_selection->asset_identity_fingerprint = proof->Identity().sha256;
        } else {
            asset->legacy_content_sha256 = proof->Identity().sha256;
        }
        if (asset->mix_decision_required) {
            auto metadata = publication->second.metadata;
            if (verify_pending &&
                (!metadata || metadata->audio_metadata.status == engine::AudioMetadataStatus::Pending))
                metadata = std::make_shared<engine::EditMediaMetadata>(
                    engine::ProbeEditMedia(publication->second.access, {}, admission, generation.get(), true));
            if (metadata && metadata->audio_metadata.status == engine::AudioMetadataStatus::Verified &&
                metadata->audio_metadata.manifest && !metadata->audio_metadata.monitoring_confirmation_required)
                edit::FinalizeProjectRecordedDefault(
                    *asset, captured.clips,
                    edit::DescribeSourceImport(*metadata, asset->duration, publication->second.access));
        }
        source = std::move(asset);
    }
}

} // namespace

void EditSessionAdapter::initializeProjects() {
    if (recovery_lock_)
        return;
    initializeProjectRecovery(settings::ResolveAppDataDir());
}

void EditSessionAdapter::initializeProjectRecovery(const QString& location) {
    projects_active_ = true;
    QDir dir(location);
    if (!dir.mkpath(QStringLiteral("editor-recovery")) || !dir.cd(QStringLiteral("editor-recovery"))) {
        emit projectError(tr("The editor recovery folder is unavailable."));
        return;
    }
    const auto stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm-ss"));
    recovery_path_ = dir.filePath(stamp + QLatin1Char(' ') + QUuid::createUuid().toString(QUuid::WithoutBraces) +
                                  QStringLiteral(".exorecovery"));
    auto lock = std::make_unique<QLockFile>(recovery_path_ + QStringLiteral(".lock"));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        emit projectError(tr("The editor recovery folder is unavailable."));
        return;
    }
    recovery_lock_ = std::move(lock);
    project_pool_.setMaxThreadCount(1);
    project_timer_.setSingleShot(true);
    project_timer_.setInterval(2000);
    connect(&project_timer_, &QTimer::timeout, this, &EditSessionAdapter::checkpointProject);
    const auto changed = [this] {
        if (matchesProjectState(observed_project_state_))
            return;
        (void)closeGuardRevision();
        emit projectChanged();
        emit unsavedEditsChanged();
        if (!project_timer_.isActive())
            project_timer_.start();
    };
    connect(this, &EditSessionAdapter::markersChanged, this, changed);
    connect(this, &EditSessionAdapter::interactionSettingsChanged, this, changed);
    connect(this, &EditSessionAdapter::sourceVerificationChanged, this, [this](qulonglong id) {
        if (sourceProof(id) && projectDirty()) {
            recovery_snapshot_.reset();
            project_timer_.start();
        }
    });
    refreshRecoveries();
    if (projectDirty())
        project_timer_.start();
}

void EditSessionAdapter::refreshRecoveries() {
    recoverable_projects_.clear();
    const auto dir = QFileInfo(recovery_path_).absoluteDir();
    for (const auto& file : dir.entryInfoList({QStringLiteral("*.exorecovery")}, QDir::Files, QDir::Time)) {
        const QString path = file.absoluteFilePath();
        if (path == recovery_path_)
            continue;
        QLockFile lock(path + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        if (lock.tryLock(0))
            recoverable_projects_.append(path);
    }
    emit projectChanged();
}

void EditSessionAdapter::retireCheckpoint() {
    project_timer_.stop();
    recovery_snapshot_.reset();
    recovery_state_ = {};
    ++recovery_generation_;
    recovery_busy_ = false;
    const auto path = recovery_path_;
    if (recovery_lock_)
        project_pool_.start([path] { QFile::remove(path); });
}

void EditSessionAdapter::preserveProjectRecovery() {
    if (!recovery_lock_ || discard_checkpoint_on_quit_ || !projectDirty())
        return;
    preserve_checkpoint_ = true;
    ++recovery_generation_;
    recovery_busy_ = false;
    recovery_snapshot_.reset();
    checkpointProject();
}

void EditSessionAdapter::discardProjectRecoveryOnQuit() {
    // A save failure already queued on the UI thread can still request preservation.
    discard_checkpoint_on_quit_ = true;
    retireCheckpoint();
}

void EditSessionAdapter::checkpointProject() {
    if (!recovery_lock_ || discard_checkpoint_on_quit_ || recovery_busy_)
        return;
    if (!projectDirty()) {
        if (recovery_snapshot_)
            retireCheckpoint();
        return;
    }
    const auto snapshot = workspace_.snapshot();
    if (snapshot == recovery_snapshot_ && matchesProjectState(recovery_state_))
        return;
    recovery_busy_ = true;
    const auto path = recovery_path_;
    project_pool_.start([this, path, snapshot, state = projectSessionState(), generation = recovery_generation_,
                         sources = source_publications_] {
        QByteArray digest;
        auto captured = *snapshot;
        BindProjectIdentities(captured, sources, false, {}, {});
        const auto error = edit::EditProjectStore::Write(path, {}, std::move(captured), digest, true, state);
        QMetaObject::invokeMethod(this, [this, snapshot, state, generation, error] {
            if (generation != recovery_generation_)
                return;
            recovery_busy_ = false;
            if (error.isEmpty()) {
                recovery_snapshot_ = snapshot;
                recovery_state_ = state;
            } else
                emit projectError(error);
            if (snapshot != workspace_.snapshot() || !matchesProjectState(state))
                project_timer_.start();
        });
    });
}

bool EditSessionAdapter::allowProjectReplacement(qulonglong discard_revision, bool discard) {
    if (project_busy_)
        return false;
    if (!discard)
        return !projectDirty();
    if (discard_revision == closeGuardRevision())
        return true;
    emit projectError(tr("The project changed before discard was confirmed. Review it and try again."));
    return false;
}

void EditSessionAdapter::newProject(qulonglong discard_revision, bool discard) {
    if (!allowProjectReplacement(discard_revision, discard))
        return;
    resetWorkspace(true);
    resetProject();
}

void EditSessionAdapter::resetProject() {
    undo_selection_.reset();
    pending_track_delete_ = 0;
    pending_track_delete_revision_ = 0;
    loop_snap_bounds_.reset();
    clip_clipboard_.reset();
    emit clipboardChanged();
    retireCheckpoint();
    project_path_.clear();
    project_digest_.clear();
    recovered_project_ = false;
    recovered_from_.clear();
    recovered_digest_.clear();
    project_baseline_ = workspace_.snapshot();
    autoCrossfade = true;
    project_state_ = projectSessionState();
    preserve_checkpoint_ = false;
    emit interactionSettingsChanged();
    emit projectChanged();
    emit unsavedEditsChanged();
}

void EditSessionAdapter::saveProject(const QUrl& url) {
    if (project_busy_ || !url.isLocalFile() || !preflightPendingDraft())
        return;
    const auto path = QFileInfo(url.toLocalFile()).absoluteFilePath();
    if (!path.endsWith(QStringLiteral(".exoproj"), Qt::CaseInsensitive)) {
        emit projectError(tr("Choose a filename ending in .exoproj."));
        return;
    }
    const auto expected = path == project_path_ ? project_digest_ : QByteArray{};
    const auto snapshot = workspace_.snapshot();
    const auto recovered_path = recovered_from_;
    const auto recovered_digest = recovered_digest_;
    const auto generation = source_cancelled_;
    preserve_checkpoint_ = true;
    project_busy_ = true;
    emit projectChanged();
    const auto sources = source_publications_;
    const auto admission = background_io_;
    project_pool_.start([this, path, expected, snapshot, generation, sources, admission, recovered_path,
                         recovered_digest, state = projectSessionState()] {
        auto captured = *snapshot;
        BindProjectIdentities(captured, sources, true, admission, generation);
        QByteArray digest;
        const auto error = edit::EditProjectStore::Write(path, expected, std::move(captured), digest, false, state);
        const bool recovery_retired =
            error.isEmpty() &&
            (recovered_path.isEmpty() || edit::EditProjectStore::RemoveCheckpoint(recovered_path, recovered_digest));
        QMetaObject::invokeMethod(this, [this, path, snapshot, generation, error, digest, state, recovery_retired] {
            project_busy_ = false;
            if (!error.isEmpty()) {
                preserve_checkpoint_ = true;
                emit projectError(error);
            } else if (generation == source_cancelled_) {
                preserve_checkpoint_ = false;
                project_path_ = path;
                project_digest_ = digest;
                project_baseline_ = snapshot;
                project_state_ = state;
                recovered_project_ = false;
                if (!recovered_from_.isEmpty()) {
                    if (recovery_retired) {
                        recovered_from_.clear();
                        recovered_digest_.clear();
                    } else {
                        emit projectError(tr("The project was saved, but its recovery snapshot could not be removed. "
                                             "Save again to retry."));
                    }
                    refreshRecoveries();
                }
                if (!projectDirty())
                    retireCheckpoint();
                emit projectSaved();
            }
            emit projectChanged();
            emit unsavedEditsChanged();
        });
    });
}

void EditSessionAdapter::openProject(const QUrl& url, qulonglong discard_revision, bool discard) {
    if (url.isLocalFile())
        loadProject(QFileInfo(url.toLocalFile()).absoluteFilePath(), discard_revision, discard, false);
}

void EditSessionAdapter::recoverProject(const QString& path, qulonglong discard_revision, bool discard) {
    if (recoverable_projects_.contains(path))
        loadProject(path, discard_revision, discard, true);
}

void EditSessionAdapter::loadProject(const QString& path, qulonglong discard_revision, bool discard, bool recovery) {
    if (!allowProjectReplacement(discard_revision, discard))
        return;
    const auto before = closeGuardRevision();
    const auto session = source_cancelled_;
    const auto admission = background_io_;
    project_busy_ = true;
    emit projectChanged();
    project_pool_.start([this, path, before, session, admission, recovery] {
        QByteArray digest;
        edit::EditSnapshot snapshot;
        edit::ProjectSessionState state;
        QString error = edit::EditProjectStore::Read(path, snapshot, state, digest);
        std::map<edit::Id, SourcePublication> publications;
        if (error.isEmpty())
            for (auto& source : snapshot.assets) {
                auto asset = std::make_shared<edit::Asset>(*source);
                source = asset;
                asset->state = edit::AssetState::Missing;
                const auto expected = ExpectedIdentity(*asset);
                if (session->load() || !KnownIdentity(expected))
                    continue;
                const auto sourcePath = QString::fromStdWString(asset->path);
                if (sourcePath.startsWith(QStringLiteral("//")) || !QFileInfo(sourcePath).isFile())
                    continue;
                const auto acquired = engine::AcquireMediaReadCapability(asset->path);
                if (!acquired.succeeded())
                    continue;
                const auto access = *acquired.value();
                const auto verified = engine::VerifyMediaIdentity(access, expected, {}, admission, session.get());
                if (!verified.succeeded())
                    continue;
                auto metadata = engine::ProbeEditMedia(access, {}, admission, session.get(), true);
                if (!metadata.error.empty())
                    continue;
                auto imported = edit::DescribeSourceImport(metadata, asset->duration, access);
                if (asset->source_descriptor) {
                    if (!imported.descriptor ||
                        !edit::BindProjectStreamLayout(*asset->source_descriptor, *imported.descriptor, metadata))
                        continue;
                    auto descriptor = std::make_shared<edit::ValidatedSourceDescriptor>(*asset->source_descriptor);
                    descriptor->file_identity = imported.descriptor->file_identity;
                    descriptor->asset_identity_fingerprint = expected;
                    asset->source_descriptor = std::move(descriptor);
                    if (metadata.audio_metadata.status == engine::AudioMetadataStatus::Verified &&
                        metadata.audio_metadata.manifest && !metadata.audio_metadata.monitoring_confirmation_required)
                        edit::FinalizeProjectRecordedDefault(*asset, snapshot.clips, imported);
                }
                asset->state = edit::AssetState::Available;
                publications.emplace(
                    asset->id, SourcePublication{access,
                                                 *verified.value(),
                                                 std::make_shared<const engine::EditMediaMetadata>(std::move(metadata)),
                                                 {},
                                                 false,
                                                 false,
                                                 asset->source_version});
            }
        auto candidate = std::make_shared<edit::Workspace>();
        if (error.isEmpty() && !candidate->restoreProject(snapshot))
            error = tr("The project contains invalid editor data.");
        QMetaObject::invokeMethod(this, [this, before, session, path, digest, recovery, error, candidate,
                                         state = std::move(state), publications = std::move(publications)]() mutable {
            project_busy_ = false;
            if (!error.isEmpty())
                emit projectError(error);
            else if (before != closeGuardRevision() || session != source_cancelled_)
                emit projectError(tr("The workspace changed while opening the project. Open it again to continue."));
            else {
                newProject(before, true);
                workspace_ = std::move(*candidate);
                source_publications_ = std::move(publications);
                initial_workspace_ = workspace_.snapshot();
                project_baseline_ = initial_workspace_;
                project_path_ = recovery ? QString{} : path;
                project_digest_ = recovery ? QByteArray{} : digest;
                recovered_project_ = recovery;
                recovered_from_ = recovery ? path : QString{};
                recovered_digest_ = recovery ? digest : QByteArray{};
                overview_revision_ = (std::numeric_limits<edit::Revision>::max)();
                project_state_ = state;
                autoCrossfade = state.auto_crossfade;
                markers_ = std::move(state.markers);
                emit interactionSettingsChanged();
                emit markersChanged();
                publishWorkspace(true);
            }
            emit projectChanged();
        });
    });
}

} // namespace exosnap::quick
