#include "RecordingHistoryStore.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include "diagnostics/AppLog.h"
#include "settings/ConfigPaths.h"

namespace exosnap {
namespace {

constexpr int kSchemaVersion = 2;

// ---- Serialization helpers ----

QString CodecToString(recorder_core::Container c) {
    switch (c) {
    case recorder_core::Container::WebM:
        return QStringLiteral("webm");
    case recorder_core::Container::Matroska:
        return QStringLiteral("mkv");
    case recorder_core::Container::Mp4:
        return QStringLiteral("mp4");
    }
    return QString();
}

std::optional<recorder_core::Container> StringToContainer(const QString& s) {
    const QString lower = s.trimmed().toLower();
    if (lower == QStringLiteral("webm"))
        return recorder_core::Container::WebM;
    if (lower == QStringLiteral("mkv") || lower == QStringLiteral("matroska"))
        return recorder_core::Container::Matroska;
    if (lower == QStringLiteral("mp4"))
        return recorder_core::Container::Mp4;
    return std::nullopt;
}

QString VideoCodecToString(recorder_core::VideoCodec c) {
    switch (c) {
    case recorder_core::VideoCodec::Av1Nvenc:
        return QStringLiteral("av1");
    case recorder_core::VideoCodec::H264Nvenc:
        return QStringLiteral("h264");
    case recorder_core::VideoCodec::HevcNvenc:
        return QStringLiteral("hevc");
    }
    return QString();
}

std::optional<recorder_core::VideoCodec> StringToVideoCodec(const QString& s) {
    const QString lower = s.trimmed().toLower();
    if (lower == QStringLiteral("av1") || lower == QStringLiteral("av1_nvenc"))
        return recorder_core::VideoCodec::Av1Nvenc;
    if (lower == QStringLiteral("h264") || lower == QStringLiteral("h264_nvenc"))
        return recorder_core::VideoCodec::H264Nvenc;
    if (lower == QStringLiteral("hevc") || lower == QStringLiteral("hevc_nvenc") || lower == QStringLiteral("h265"))
        return recorder_core::VideoCodec::HevcNvenc;
    return std::nullopt;
}

QString AudioCodecToString(recorder_core::AudioCodec c) {
    switch (c) {
    case recorder_core::AudioCodec::AacMf:
        return QStringLiteral("aac");
    case recorder_core::AudioCodec::Opus:
        return QStringLiteral("opus");
    case recorder_core::AudioCodec::Pcm:
        return QStringLiteral("pcm");
    case recorder_core::AudioCodec::Flac:
        return QStringLiteral("flac");
    }
    return QString();
}

std::optional<recorder_core::AudioCodec> StringToAudioCodec(const QString& s) {
    const QString lower = s.trimmed().toLower();
    if (lower == QStringLiteral("aac") || lower == QStringLiteral("aac_mf"))
        return recorder_core::AudioCodec::AacMf;
    if (lower == QStringLiteral("opus"))
        return recorder_core::AudioCodec::Opus;
    if (lower == QStringLiteral("pcm"))
        return recorder_core::AudioCodec::Pcm;
    if (lower == QStringLiteral("flac"))
        return recorder_core::AudioCodec::Flac;
    return std::nullopt;
}

std::optional<RecordingMarkerType> StringToMarkerType(const QString& s) {
    const QString lower = s.trimmed().toLower();
    if (lower == QStringLiteral("general"))
        return RecordingMarkerType::General;
    if (lower == QStringLiteral("cut"))
        return RecordingMarkerType::Cut;
    if (lower == QStringLiteral("highlight"))
        return RecordingMarkerType::Highlight;
    return std::nullopt;
}

// ---- Serialize ----

QJsonObject SegmentToJson(const CompletedRecordingSegment& seg) {
    QJsonObject obj;
    obj[QStringLiteral("path")] = seg.file_path;
    obj[QStringLiteral("index")] = static_cast<int>(seg.index);
    obj[QStringLiteral("sessionStartMs")] = static_cast<qint64>(seg.session_start_ms);
    obj[QStringLiteral("durationMs")] = static_cast<qint64>(seg.duration_seconds * 1000.0);
    obj[QStringLiteral("fileSizeBytes")] = seg.file_size_bytes;
    obj[QStringLiteral("succeeded")] = seg.succeeded;
    return obj;
}

// Returns false when a segment object is structurally invalid (missing/empty
// path or negative numerics). A false here drops just this segment, never the
// whole recording.
bool ValidateSegment(const QJsonObject& obj, CompletedRecordingSegment& out) {
    if (!obj.contains(QStringLiteral("path")) || !obj[QStringLiteral("path")].isString())
        return false;
    out.file_path = obj[QStringLiteral("path")].toString().trimmed();
    if (out.file_path.isEmpty())
        return false;

    out.index = static_cast<uint32_t>(obj.value(QStringLiteral("index")).toInt(0));
    {
        const double v = obj.value(QStringLiteral("sessionStartMs")).toDouble(0.0);
        out.session_start_ms = v < 0.0 ? 0ull : static_cast<uint64_t>(v);
    }
    {
        const double ms = obj.value(QStringLiteral("durationMs")).toDouble(0.0);
        if (ms < 0.0)
            return false;
        out.duration_seconds = ms / 1000.0;
    }
    {
        const QJsonValue& val = obj.value(QStringLiteral("fileSizeBytes"));
        const qint64 v = static_cast<qint64>(val.toDouble(0.0));
        if (v < 0)
            return false;
        out.file_size_bytes = v;
    }
    out.succeeded = obj.value(QStringLiteral("succeeded")).toBool(true);
    return true;
}

QJsonObject RecordingToJson(const CompletedRecording& rec) {
    QJsonObject obj;
    obj[QStringLiteral("path")] = rec.file_path;
    obj[QStringLiteral("displayName")] = rec.display_name;
    obj[QStringLiteral("fileSizeBytes")] = rec.file_size_bytes;
    obj[QStringLiteral("durationMs")] = static_cast<qint64>(rec.duration_seconds * 1000.0);
    obj[QStringLiteral("sourceWidth")] = static_cast<int>(rec.source_width);
    obj[QStringLiteral("sourceHeight")] = static_cast<int>(rec.source_height);
    obj[QStringLiteral("outputWidth")] = static_cast<int>(rec.output_width);
    obj[QStringLiteral("outputHeight")] = static_cast<int>(rec.output_height);
    obj[QStringLiteral("frameRateNum")] = static_cast<int>(rec.frame_rate_num);
    obj[QStringLiteral("frameRateDen")] = static_cast<int>(rec.frame_rate_den);
    obj[QStringLiteral("cfr")] = rec.cfr;
    obj[QStringLiteral("container")] = CodecToString(rec.container);
    obj[QStringLiteral("videoCodec")] = VideoCodecToString(rec.video_codec);
    obj[QStringLiteral("audioCodec")] = AudioCodecToString(rec.audio_codec);
    obj[QStringLiteral("isDisplayTarget")] = rec.is_display_target;
    obj[QStringLiteral("createdAt")] =
        rec.completed_at.isValid() ? rec.completed_at.toString(Qt::ISODateWithMs) : QString();
    if (!rec.segments.empty()) {
        QJsonArray segs;
        for (const auto& seg : rec.segments)
            segs.append(SegmentToJson(seg));
        obj[QStringLiteral("segments")] = segs;
    }
    // Markers (VR-006): without these the marker summary silently disappears
    // from restored history entries even though the sidecar file still exists.
    if (!rec.markers.empty()) {
        QJsonArray markers;
        for (const auto& m : rec.markers) {
            QJsonObject mo;
            mo[QStringLiteral("timeMs")] = static_cast<qint64>(m.time_ms);
            mo[QStringLiteral("type")] = QLatin1StringView(RecordingMarkerTypeToString(m.type));
            mo[QStringLiteral("label")] = QString::fromStdString(m.label);
            markers.append(mo);
        }
        obj[QStringLiteral("markers")] = markers;
    }
    if (!rec.marker_sidecar_path.isEmpty())
        obj[QStringLiteral("markerSidecarPath")] = rec.marker_sidecar_path;
    return obj;
}

// ---- Deserialize / Validate ----

bool ValidateCompletedRecording(const QJsonObject& obj, CompletedRecording& out) {
    // Path is required
    if (!obj.contains(QStringLiteral("path")) || !obj[QStringLiteral("path")].isString())
        return false;
    out.file_path = obj[QStringLiteral("path")].toString().trimmed();
    if (out.file_path.isEmpty())
        return false;

    // Display name (optional)
    if (obj.contains(QStringLiteral("displayName")))
        out.display_name = obj[QStringLiteral("displayName")].toString();

    // File size (non-negative)
    if (obj.contains(QStringLiteral("fileSizeBytes"))) {
        const QJsonValue& val = obj[QStringLiteral("fileSizeBytes")];
        if (!val.isDouble())
            return false;
        qint64 v = static_cast<qint64>(val.toDouble());
        if (v < 0)
            return false;
        out.file_size_bytes = v;
    }

    // Duration in ms → seconds (non-negative)
    if (obj.contains(QStringLiteral("durationMs"))) {
        const QJsonValue& val = obj[QStringLiteral("durationMs")];
        if (!val.isDouble())
            return false;
        double ms = val.toDouble();
        if (ms < 0.0)
            return false;
        out.duration_seconds = ms / 1000.0;
    }

    // Resolution fields
    auto readUint = [&](const char* key, uint32_t& target) -> bool {
        if (!obj.contains(QLatin1StringView(key)))
            return true;
        const QJsonValue& val = obj[QLatin1StringView(key)];
        if (!val.isDouble())
            return false;
        double v = val.toDouble();
        if (v < 0.0 || v > static_cast<double>(UINT32_MAX))
            return false;
        target = static_cast<uint32_t>(v);
        return true;
    };

    if (!readUint("sourceWidth", out.source_width))
        return false;
    if (!readUint("sourceHeight", out.source_height))
        return false;
    if (!readUint("outputWidth", out.output_width))
        return false;
    if (!readUint("outputHeight", out.output_height))
        return false;
    if (!readUint("frameRateNum", out.frame_rate_num))
        return false;
    if (!readUint("frameRateDen", out.frame_rate_den))
        return false;

    // CFR
    if (obj.contains(QStringLiteral("cfr")))
        out.cfr = obj[QStringLiteral("cfr")].toBool();

    // Container enum
    if (obj.contains(QStringLiteral("container"))) {
        auto c = StringToContainer(obj[QStringLiteral("container")].toString());
        if (!c.has_value())
            return false;
        out.container = *c;
    }

    // Video codec enum
    if (obj.contains(QStringLiteral("videoCodec"))) {
        auto v = StringToVideoCodec(obj[QStringLiteral("videoCodec")].toString());
        if (!v.has_value())
            return false;
        out.video_codec = *v;
    }

    // Audio codec enum
    if (obj.contains(QStringLiteral("audioCodec"))) {
        auto a = StringToAudioCodec(obj[QStringLiteral("audioCodec")].toString());
        if (!a.has_value())
            return false;
        out.audio_codec = *a;
    }

    // Display target
    if (obj.contains(QStringLiteral("isDisplayTarget")))
        out.is_display_target = obj[QStringLiteral("isDisplayTarget")].toBool();

    // Timestamp
    if (obj.contains(QStringLiteral("createdAt"))) {
        QString ts = obj[QStringLiteral("createdAt")].toString().trimmed();
        if (!ts.isEmpty()) {
            QDateTime parsed = QDateTime::fromString(ts, Qt::ISODateWithMs);
            if (!parsed.isValid()) {
                parsed = QDateTime::fromString(ts, Qt::ISODate);
            }
            if (!parsed.isValid())
                return false;
            out.completed_at = parsed;
        }
    }

    // Segments (v2). A single invalid segment is skipped; the rest (and the
    // recording) survive. Missing/empty array => legacy single-file recording.
    if (obj.contains(QStringLiteral("segments")) && obj[QStringLiteral("segments")].isArray()) {
        const QJsonArray segs = obj[QStringLiteral("segments")].toArray();
        int skipped_segments = 0;
        for (const QJsonValue& sv : segs) {
            if (!sv.isObject()) {
                ++skipped_segments;
                continue;
            }
            CompletedRecordingSegment seg;
            if (ValidateSegment(sv.toObject(), seg))
                out.segments.push_back(std::move(seg));
            else
                ++skipped_segments;
        }
        if (skipped_segments > 0) {
            diagnostics::AppLog::warning(
                QStringLiteral("history.store"),
                QStringLiteral("Skipped %1 invalid segment(s) in a recording entry").arg(skipped_segments));
        }
    }

    // Markers (VR-006). An invalid marker is skipped; missing array means a
    // marker-less (or pre-VR-006) entry — never a load failure.
    if (obj.contains(QStringLiteral("markers")) && obj[QStringLiteral("markers")].isArray()) {
        const QJsonArray markers = obj[QStringLiteral("markers")].toArray();
        int skipped_markers = 0;
        for (const QJsonValue& mv : markers) {
            if (!mv.isObject()) {
                ++skipped_markers;
                continue;
            }
            const QJsonObject mo = mv.toObject();
            const double time_ms = mo.value(QStringLiteral("timeMs")).toDouble(-1.0);
            const auto type = StringToMarkerType(mo.value(QStringLiteral("type")).toString());
            if (time_ms < 0.0 || !type.has_value()) {
                ++skipped_markers;
                continue;
            }
            RecordingMarker marker;
            marker.time_ms = static_cast<uint64_t>(time_ms);
            marker.type = *type;
            marker.label = mo.value(QStringLiteral("label")).toString().toStdString();
            out.markers.push_back(std::move(marker));
        }
        if (skipped_markers > 0) {
            diagnostics::AppLog::warning(
                QStringLiteral("history.store"),
                QStringLiteral("Skipped %1 invalid marker(s) in a recording entry").arg(skipped_markers));
        }
    }
    if (obj.contains(QStringLiteral("markerSidecarPath")))
        out.marker_sidecar_path = obj[QStringLiteral("markerSidecarPath")].toString();

    out.succeeded = true;
    return true;
}

} // namespace

// ---- RecordingHistoryStore ----

RecordingHistoryStore::RecordingHistoryStore() {
    const QString config_dir = settings::ResolveAppConfigDir();
    if (!config_dir.isEmpty()) {
        QDir().mkpath(config_dir);
        file_path_ = QDir(config_dir).filePath(QStringLiteral("recording-history.json"));
    } else {
        file_path_ = QStringLiteral("recording-history.json");
    }
}

RecordingHistoryStore::RecordingHistoryStore(QString file_path) : file_path_(std::move(file_path)) {
}

QVector<CompletedRecording> RecordingHistoryStore::Load() const {
    QVector<CompletedRecording> result;

    if (file_path_.isEmpty())
        return result;

    QFileInfo info(file_path_);
    if (!info.exists()) {
        diagnostics::AppLog::info(QStringLiteral("history.store"),
                                  QStringLiteral("No recording history file — starting with empty history"));
        return result;
    }

    QFile file(file_path_);
    if (!file.open(QIODevice::ReadOnly)) {
        diagnostics::AppLog::warning(
            QStringLiteral("history.store"),
            QStringLiteral("Could not open recording history file for reading: %1").arg(file_path_));
        return result;
    }

    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parse_error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        diagnostics::AppLog::warning(
            QStringLiteral("history.store"),
            QStringLiteral("Malformed recording history JSON: %1").arg(parse_error.errorString()));
        return result;
    }

    if (!doc.isObject()) {
        diagnostics::AppLog::warning(QStringLiteral("history.store"),
                                     QStringLiteral("Recording history JSON root is not an object"));
        return result;
    }

    QJsonObject root = doc.object();

    // Check schema version
    int version = root.value(QStringLiteral("version")).toInt(0);
    if (version == 0) {
        diagnostics::AppLog::warning(QStringLiteral("history.store"),
                                     QStringLiteral("Recording history file missing version field"));
        return result;
    }
    if (version > kSchemaVersion) {
        diagnostics::AppLog::warning(QStringLiteral("history.store"),
                                     QStringLiteral("Unsupported recording history schema version %1 (current: %2)")
                                         .arg(version)
                                         .arg(kSchemaVersion));
        return result;
    }

    if (!root.contains(QStringLiteral("recordings")) || !root[QStringLiteral("recordings")].isArray()) {
        diagnostics::AppLog::warning(QStringLiteral("history.store"),
                                     QStringLiteral("Recording history JSON missing recordings array"));
        return result;
    }

    QJsonArray recordings_array = root[QStringLiteral("recordings")].toArray();
    result.reserve(recordings_array.size());

    int skipped = 0;
    for (const QJsonValue& val : recordings_array) {
        if (!val.isObject()) {
            ++skipped;
            continue;
        }

        CompletedRecording rec;
        if (ValidateCompletedRecording(val.toObject(), rec)) {
            result.append(rec);
        } else {
            ++skipped;
        }
    }

    if (skipped > 0) {
        diagnostics::AppLog::warning(QStringLiteral("history.store"),
                                     QStringLiteral("Skipped %1 invalid recording history entries").arg(skipped));
    }

    diagnostics::AppLog::info(QStringLiteral("history.store"),
                              QStringLiteral("Loaded %1 recording history entries%2")
                                  .arg(result.size())
                                  .arg(skipped > 0 ? QStringLiteral(" (%1 skipped)").arg(skipped) : QString()));

    return result;
}

bool RecordingHistoryStore::Save(const QVector<CompletedRecording>& recordings) const {
    if (file_path_.isEmpty())
        return false;

    QDir().mkpath(QFileInfo(file_path_).absolutePath());

    QJsonArray recordings_array;
    for (const auto& rec : recordings) {
        recordings_array.append(RecordingToJson(rec));
    }

    QJsonObject root;
    root[QStringLiteral("version")] = kSchemaVersion;
    root[QStringLiteral("recordings")] = recordings_array;

    QJsonDocument doc(root);

    QSaveFile file(file_path_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        diagnostics::AppLog::warning(
            QStringLiteral("history.store"),
            QStringLiteral("Could not open recording history file for atomic write: %1").arg(file_path_));
        return false;
    }

    file.write(doc.toJson(QJsonDocument::Indented));

    if (!file.commit()) {
        diagnostics::AppLog::warning(
            QStringLiteral("history.store"),
            QStringLiteral("Atomic save commit failed for recording history: %1").arg(file_path_));
        return false;
    }

    diagnostics::AppLog::info(QStringLiteral("history.store"),
                              QStringLiteral("Persisted %1 recording history entries").arg(recordings.size()));
    return true;
}

const QString& RecordingHistoryStore::StorePath() const {
    return file_path_;
}

} // namespace exosnap
