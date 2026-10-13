#pragma once

#include "models/MarkerSidecar.h"
#include <QDebug>
#include <exosnap/engine/audio_media_manifest.h>

namespace exosnap {

class MetadataCompanionPublicationGuard {
  public:
    explicit MetadataCompanionPublicationGuard(std::vector<std::filesystem::path> created)
        : created_(std::move(created)) {
    }
    MetadataCompanionPublicationGuard(const MetadataCompanionPublicationGuard&) = delete;
    MetadataCompanionPublicationGuard& operator=(const MetadataCompanionPublicationGuard&) = delete;
    ~MetadataCompanionPublicationGuard() {
        if (published_)
            return;
        for (const auto& path : created_) {
            std::error_code error;
            std::filesystem::remove(path, error);
            if (error)
                qWarning() << "Cannot remove unpublished metadata companion:" << QString::fromStdWString(path.wstring())
                           << QString::fromStdString(error.message());
        }
    }
    void Commit() {
        published_ = true;
    }

  private:
    std::vector<std::filesystem::path> created_;
    bool published_ = false;
};

inline bool StageMarkerSidecarForPublication(const std::filesystem::path& staging,
                                             const std::filesystem::path& destination,
                                             const std::vector<RecordingMarker>& markers, std::string& error,
                                             std::optional<int> segment = std::nullopt,
                                             const std::function<bool()>& continue_io = {}) {
    if (continue_io && !continue_io()) {
        error = "Metadata publication cancelled";
        return false;
    }
    const auto owned_reference =
        QString::fromStdWString(DeriveMarkerSidecarPath(destination).filename().wstring()).toStdString();
    std::optional<std::string> reference;
    if (!markers.empty()) {
        if (!WriteMarkerSidecar(DeriveMarkerSidecarPath(staging), markers,
                                QString::fromStdWString(destination.filename().wstring()), segment)) {
            error = "MetadataIncomplete: cannot stage marker companion";
            return false;
        }
        reference = QString::fromStdWString(DeriveMarkerSidecarPath(destination).filename().wstring()).toStdString();
    }
    const auto updated = engine::UpdateMediaIdentityMarkerReference(staging, reference, continue_io, owned_reference);
    if (!updated.succeeded()) {
        error = updated.error()->context;
        return false;
    }
    return true;
}

} // namespace exosnap
