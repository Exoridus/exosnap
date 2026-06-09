#pragma once

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSize>
#include <QString>
#include <QVector>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "RecordingMarker.h"
#include <recorder_core/recorder_session.h>

namespace exosnap {

// Overall outcome of a (possibly multi-segment) recording. A recording that
// produced some valid segments but failed to finalize one is reported as
// CompletedWithPartialFailure — never silently downgraded to a plain success.
enum class CompletedRecordingStatus {
    Completed,
    CompletedWithPartialFailure,
    Failed,
};

// One finalized media segment of a logical recording (SPLIT-RECORDING-R1).
// Single-file recordings carry exactly one segment (index 0).
struct CompletedRecordingSegment {
    QString file_path;
    uint32_t index = 0;            // 0-based segment index
    uint64_t session_start_ms = 0; // start on the continuous session timeline
    double duration_seconds = 0.0; // segment-local media duration
    qint64 file_size_bytes = 0;
    bool succeeded = false; // false => finalize failed / file quarantined

    [[nodiscard]] QString fileName() const {
        return file_path.isEmpty() ? QString() : QFileInfo(file_path).fileName();
    }

    [[nodiscard]] QString parentFolder() const {
        return file_path.isEmpty() ? QString() : QFileInfo(file_path).absolutePath();
    }

    // Honest existence check: a segment reported succeeded but missing on disk
    // (user deleted/moved it) is not "present".
    [[nodiscard]] bool fileExists() const {
        return succeeded && !file_path.isEmpty() && QFileInfo::exists(file_path);
    }

    bool operator==(const CompletedRecordingSegment& o) const noexcept {
        return file_path == o.file_path && index == o.index && session_start_ms == o.session_start_ms &&
               duration_seconds == o.duration_seconds && file_size_bytes == o.file_size_bytes &&
               succeeded == o.succeeded;
    }
    bool operator!=(const CompletedRecordingSegment& o) const noexcept {
        return !(*this == o);
    }
};

struct CompletedRecording {
    QString file_path;
    QString display_name;
    qint64 file_size_bytes = 0;
    double duration_seconds = 0.0;

    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;
    bool cfr = true;

    recorder_core::Container container = recorder_core::Container::WebM;
    recorder_core::VideoCodec video_codec = recorder_core::VideoCodec::Av1Nvenc;
    recorder_core::AudioCodec audio_codec = recorder_core::AudioCodec::Opus;

    bool is_display_target = false;
    QDateTime completed_at;

    bool succeeded = false;

    // Recording markers (finalized list after recording completes)
    std::vector<RecordingMarker> markers;
    QString marker_sidecar_path;

    // Multi-segment split results (SPLIT-RECORDING-R1). A single-file recording
    // carries exactly one segment whose file_path equals file_path above; a split
    // recording carries one entry per finalized segment. Empty is treated as a
    // legacy single-file recording described by the scalar fields above.
    std::vector<CompletedRecordingSegment> segments;

    [[nodiscard]] bool isMultiSegment() const noexcept {
        return segments.size() > 1;
    }

    [[nodiscard]] int segmentCount() const noexcept {
        return segments.empty() ? (hasFile() ? 1 : 0) : static_cast<int>(segments.size());
    }

    // Overall status derived from the per-segment success flags. A recording with
    // no segments falls back to the scalar succeeded flag.
    [[nodiscard]] CompletedRecordingStatus status() const noexcept {
        if (segments.empty())
            return succeeded ? CompletedRecordingStatus::Completed : CompletedRecordingStatus::Failed;
        bool any_ok = false;
        bool any_fail = false;
        for (const auto& s : segments) {
            any_ok |= s.succeeded;
            any_fail |= !s.succeeded;
        }
        if (any_ok && any_fail)
            return CompletedRecordingStatus::CompletedWithPartialFailure;
        return any_ok ? CompletedRecordingStatus::Completed : CompletedRecordingStatus::Failed;
    }

    // Sum of segment-local durations (or the scalar duration for single-file).
    [[nodiscard]] double totalDurationSeconds() const noexcept {
        if (segments.empty())
            return duration_seconds;
        double total = 0.0;
        for (const auto& s : segments)
            total += s.duration_seconds;
        return total;
    }

    // Sum of segment file sizes (or the scalar size for single-file).
    [[nodiscard]] qint64 totalSizeBytes() const noexcept {
        if (segments.empty())
            return file_size_bytes;
        qint64 total = 0;
        for (const auto& s : segments)
            total += s.file_size_bytes;
        return total;
    }

    [[nodiscard]] bool hasFile() const noexcept {
        return !file_path.isEmpty() && succeeded;
    }

    [[nodiscard]] bool fileExists() const noexcept {
        if (!hasFile())
            return false;
        return QFileInfo::exists(file_path);
    }

    [[nodiscard]] QString fileName() const {
        if (!display_name.isEmpty())
            return display_name;
        if (file_path.isEmpty())
            return QString();
        return QFileInfo(file_path).fileName();
    }

    [[nodiscard]] QString parentFolder() const {
        if (file_path.isEmpty())
            return QString();
        return QFileInfo(file_path).absolutePath();
    }

    [[nodiscard]] qint64 fileSizeFromDisk() const {
        if (!fileExists())
            return 0;
        QFileInfo info(file_path);
        return info.size();
    }

    [[nodiscard]] QSize sourceSize() const noexcept {
        return QSize(static_cast<int>(source_width), static_cast<int>(source_height));
    }

    [[nodiscard]] QSize outputSize() const noexcept {
        return QSize(static_cast<int>(output_width), static_cast<int>(output_height));
    }
};

inline bool operator==(const CompletedRecording& a, const CompletedRecording& b) noexcept {
    return a.file_path == b.file_path && a.file_size_bytes == b.file_size_bytes &&
           a.duration_seconds == b.duration_seconds && a.source_width == b.source_width &&
           a.source_height == b.source_height && a.output_width == b.output_width &&
           a.output_height == b.output_height && a.frame_rate_num == b.frame_rate_num &&
           a.frame_rate_den == b.frame_rate_den && a.cfr == b.cfr && a.container == b.container &&
           a.video_codec == b.video_codec && a.audio_codec == b.audio_codec && a.succeeded == b.succeeded;
}

inline bool operator!=(const CompletedRecording& a, const CompletedRecording& b) noexcept {
    return !(a == b);
}

// File operation helpers (used by RecordPage and tests)
inline bool IsValidWindowsFilename(const QString& name) {
    if (name.trimmed().isEmpty())
        return false;

    static const QStringList kReservedNames = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),  QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9"),
    };

    static const QVector<QChar> kInvalidChars = {
        QChar('<'), QChar('>'), QChar(':'), QChar('"'), QChar('/'), QChar('\\'), QChar('|'), QChar('?'), QChar('*'),
    };

    const QString trimmed = name.trimmed();

    for (const QChar c : trimmed) {
        if (c.unicode() < 32)
            return false;
        if (kInvalidChars.contains(c))
            return false;
    }

    const int dot_pos = trimmed.indexOf(QLatin1Char('.'));
    const QString stem = (dot_pos >= 0) ? trimmed.left(dot_pos).toUpper() : trimmed.toUpper();

    for (const QString& reserved : kReservedNames) {
        if (stem == reserved)
            return false;
    }

    bool has_non_whitespace = false;
    for (const QChar c : trimmed) {
        if (!c.isSpace() && c != QChar('.')) {
            has_non_whitespace = true;
            break;
        }
    }
    return has_non_whitespace;
}

inline std::optional<QString> ValidateRenameForFile(const QString& new_name, const QString& current_extension,
                                                    const QString& parent_folder) {
    if (new_name.trimmed().isEmpty())
        return QStringLiteral("Name cannot be empty.");

    if (!IsValidWindowsFilename(new_name))
        return QStringLiteral("Name contains invalid characters or is a reserved Windows device name.");

    if (new_name.contains(QLatin1Char('/')) || new_name.contains(QLatin1Char('\\')))
        return QStringLiteral("Name must not contain path separators.");

    QString target_name = new_name.trimmed();
    bool has_explicit_extension = false;
    for (const QChar c : target_name) {
        if (c == QChar('.')) {
            has_explicit_extension = true;
            break;
        }
    }

    if (!has_explicit_extension && !current_extension.isEmpty()) {
        target_name += current_extension;
    }

    const QString target_path = QDir(parent_folder).absoluteFilePath(target_name);

    if (QFileInfo::exists(target_path))
        return QStringLiteral("A file with that name already exists.");

    if (target_path.length() >= 260)
        return QStringLiteral("The resulting path is too long.");

    return std::nullopt;
}

} // namespace exosnap
