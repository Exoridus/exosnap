#pragma once

// Canonical marker-sidecar (de)serialization — the single source of truth for
// the "<media>.markers.json" companion file.
//
// Both producers and the consumer use these functions so there is exactly ONE
// format and ONE path convention:
//   - RecordingCoordinator writes on AddMarker, on stop (single-file), and per
//     segment (split recordings).
//   - EditExportPage loads the sidecar when the edit surface opens and writes it
//     back when the user adds/edits markers.
//
// Format (version 1):
//   {
//     "version": 1,
//     "media": "<media filename>",   // omitted when empty
//     "timebase": "milliseconds",
//     "segmentIndex": <int>,         // present only for per-segment sidecars
//     "markers": [
//       { "timeMs": <int>, "type": "general|cut|highlight", "label": "" }
//     ]
//   }
//
// The sidecar path is always the media file path with its extension replaced by
// ".markers.json".

#include <filesystem>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include "RecordingMarker.h"

namespace exosnap {

// Build the canonical JSON document for a marker set.
inline QJsonDocument SerializeMarkerSidecar(const std::vector<RecordingMarker>& markers, const QString& media = {},
                                            std::optional<int> segment_index = std::nullopt) {
    QJsonArray markers_array;
    for (const auto& m : markers) {
        QJsonObject obj;
        obj[QStringLiteral("timeMs")] = static_cast<qint64>(m.time_ms);
        obj[QStringLiteral("type")] = QString::fromLatin1(RecordingMarkerTypeToString(m.type));
        obj[QStringLiteral("label")] = QString::fromStdString(m.label);
        markers_array.append(obj);
    }
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    if (!media.isEmpty())
        root[QStringLiteral("media")] = media;
    root[QStringLiteral("timebase")] = QStringLiteral("milliseconds");
    if (segment_index.has_value())
        root[QStringLiteral("segmentIndex")] = *segment_index;
    root[QStringLiteral("markers")] = markers_array;
    return QJsonDocument(root);
}

// Parse a marker set from canonical JSON bytes (lenient on missing keys).
inline std::vector<RecordingMarker> ParseMarkerSidecar(const QByteArray& json) {
    std::vector<RecordingMarker> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonArray arr = doc.object().value(QStringLiteral("markers")).toArray();
    for (const auto& v : arr) {
        const QJsonObject obj = v.toObject();
        RecordingMarker m;
        m.time_ms = static_cast<uint64_t>(obj.value(QStringLiteral("timeMs")).toDouble());
        const QString t = obj.value(QStringLiteral("type")).toString();
        if (t == QStringLiteral("cut"))
            m.type = RecordingMarkerType::Cut;
        else if (t == QStringLiteral("highlight"))
            m.type = RecordingMarkerType::Highlight;
        else
            m.type = RecordingMarkerType::General;
        m.label = obj.value(QStringLiteral("label")).toString().toStdString();
        out.push_back(m);
    }
    return out;
}

// Write `markers` to `sidecar_path` atomically (QSaveFile). Returns true on
// success. An empty path is a no-op that returns false.
inline bool WriteMarkerSidecar(const std::filesystem::path& sidecar_path, const std::vector<RecordingMarker>& markers,
                               const QString& media = {}, std::optional<int> segment_index = std::nullopt) {
    if (sidecar_path.empty())
        return false;
    const QJsonDocument doc = SerializeMarkerSidecar(markers, media, segment_index);
    QSaveFile file(QString::fromStdWString(sidecar_path.wstring()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(doc.toJson(QJsonDocument::Indented));
    return file.commit();
}

// Read markers from `sidecar_path`. Returns an empty vector when the file is
// missing or unparseable.
inline std::vector<RecordingMarker> ReadMarkerSidecar(const std::filesystem::path& sidecar_path) {
    if (sidecar_path.empty())
        return {};
    QFile f(QString::fromStdWString(sidecar_path.wstring()));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return ParseMarkerSidecar(f.readAll());
}

// The ONE path convention: "<media>.markers.json" = the media path with its
// extension replaced.
[[nodiscard]] inline std::filesystem::path DeriveMarkerSidecarPath(const std::filesystem::path& media_path) {
    if (media_path.empty())
        return {};
    std::filesystem::path sidecar = media_path;
    sidecar.replace_extension(L".markers.json");
    return sidecar;
}

// Plan the marker sidecar that rides along an exported (possibly trimmed)
// file. Markers are rebased to the trimmed clip's start; markers cut away by
// the trim are dropped. A sidecar is written ONLY when markers survive —
// otherwise any stale sidecar already at the destination (e.g. from an
// overwrite-original export or a previous export run) must be removed so it
// can never describe timestamps the new file does not have.
struct MarkerExportPlan {
    std::filesystem::path sidecar_path;   // "<export>.markers.json"
    QString media;                        // exported media filename (sidecar "media" field)
    std::vector<RecordingMarker> markers; // retimed survivors (empty => remove)
    [[nodiscard]] bool shouldWrite() const noexcept {
        return !sidecar_path.empty() && !markers.empty();
    }
};

[[nodiscard]] inline MarkerExportPlan PlanMarkerSidecarForExport(const std::filesystem::path& export_media_path,
                                                                 std::vector<RecordingMarker> retimed_markers) {
    MarkerExportPlan plan;
    plan.sidecar_path = DeriveMarkerSidecarPath(export_media_path);
    if (!export_media_path.empty())
        plan.media = QString::fromStdWString(export_media_path.filename().wstring());
    plan.markers = std::move(retimed_markers);
    return plan;
}

// Apply a MarkerExportPlan on disk: write the sidecar when markers survived,
// otherwise delete any stale sidecar at the destination (a leftover from an
// overwritten original or a previous export run). Returns false only when a
// required write fails (a missing file to remove is not a failure).
inline bool ApplyMarkerExportPlan(const MarkerExportPlan& plan) {
    if (plan.sidecar_path.empty())
        return true;
    if (plan.shouldWrite())
        return WriteMarkerSidecar(plan.sidecar_path, plan.markers, plan.media);
    std::error_code ec;
    std::filesystem::remove(plan.sidecar_path, ec);
    return true;
}

} // namespace exosnap
