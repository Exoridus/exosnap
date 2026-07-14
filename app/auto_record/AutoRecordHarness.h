#pragma once

#include <QString>
#include <QStringList>

namespace exosnap::auto_record {

enum class TargetKind { Monitor, Window, Region };
enum class HdrMode { Off, Tonemap, Native };

struct AutoRecordOptions {
    bool enable_preview = false;
    TargetKind target = TargetKind::Monitor;
    QString target_window_title;                  // required when target == Window
    QStringList audio_rows;                       // subset of {"app","sys","mic"}, order = row order
    QString merge_above;                          // row name that merges into the row above, or empty
    QString container = QStringLiteral("mkv");    // "mkv" | "mp4" | "webm"
    QString video_codec = QStringLiteral("av1");  // "h264" | "hevc" | "av1"
    QString audio_codec = QStringLiteral("opus"); // "opus" | "aac" | "pcm"
    int chroma = 420;                             // 420 | 444
    int bit_depth = 8;                            // 8 | 10
    HdrMode hdr_mode = HdrMode::Off;
    int duration_seconds = 10;
    int capture_frame_at_seconds = -1; // -1 = disabled
    QString screenshot_path;           // preview mode only
};

bool HasAutoRecordRequest(const QStringList& args);
bool ParseAutoRecordOptions(const QStringList& args, AutoRecordOptions* out, QString* error);

} // namespace exosnap::auto_record
