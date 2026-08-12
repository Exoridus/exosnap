#include "AutoRecordHarness.h"

// Pure CLI-argument parsing only (HasAutoRecordRequest / ParseAutoRecordOptions).
// Kept in its own translation unit, separate from AutoRecordHarness.cpp's recording
// drive loop, so the parser's own gtest target (auto_record_harness_tests) needs
// only Qt6::Core — not Qt6::Widgets, RecordingCoordinator, or any capability/
// recorder_core dependency the recording logic pulls in.

namespace exosnap::auto_record {
namespace {

bool ParseTargetKind(const QString& text, TargetKind* out) {
    if (text == QStringLiteral("monitor")) {
        *out = TargetKind::Monitor;
        return true;
    }
    if (text == QStringLiteral("window")) {
        *out = TargetKind::Window;
        return true;
    }
    if (text == QStringLiteral("region")) {
        *out = TargetKind::Region;
        return true;
    }
    return false;
}

bool ParseHdrMode(const QString& text, HdrMode* out) {
    if (text == QStringLiteral("off")) {
        *out = HdrMode::Off;
        return true;
    }
    if (text == QStringLiteral("tonemap")) {
        *out = HdrMode::Tonemap;
        return true;
    }
    if (text == QStringLiteral("native")) {
        *out = HdrMode::Native;
        return true;
    }
    return false;
}

} // namespace

bool HasAutoRecordRequest(const QStringList& args) {
    return args.contains(QStringLiteral("--auto-record"));
}

bool ParseAutoRecordOptions(const QStringList& args, AutoRecordOptions* out, QString* error) {
    if (out == nullptr)
        return false;

    AutoRecordOptions parsed;
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        const auto require_value = [&](QString* target) -> bool {
            if (i + 1 >= args.size()) {
                if (error)
                    *error = QStringLiteral("Missing value for %1").arg(arg);
                return false;
            }
            *target = args.at(++i);
            return true;
        };

        if (arg == QStringLiteral("--auto-record")) {
            continue;
        } else if (arg == QStringLiteral("--enable-preview")) {
            parsed.enable_preview = true;
        } else if (arg == QStringLiteral("--target")) {
            QString value;
            if (!require_value(&value) || !ParseTargetKind(value, &parsed.target)) {
                if (error)
                    *error = QStringLiteral("--target requires monitor|window|region");
                return false;
            }
        } else if (arg == QStringLiteral("--target-window-title")) {
            if (!require_value(&parsed.target_window_title))
                return false;
        } else if (arg == QStringLiteral("--audio-rows")) {
            QString value;
            if (!require_value(&value))
                return false;
            parsed.audio_rows = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
        } else if (arg == QStringLiteral("--merge-above")) {
            if (!require_value(&parsed.merge_above))
                return false;
        } else if (arg == QStringLiteral("--container")) {
            QString value;
            if (!require_value(&value))
                return false;
            if (value != QStringLiteral("mkv") && value != QStringLiteral("mp4") && value != QStringLiteral("webm")) {
                if (error)
                    *error = QStringLiteral("--container requires mkv|mp4|webm");
                return false;
            }
            parsed.container = value;
        } else if (arg == QStringLiteral("--video-codec")) {
            if (!require_value(&parsed.video_codec))
                return false;
        } else if (arg == QStringLiteral("--audio-codec")) {
            if (!require_value(&parsed.audio_codec))
                return false;
        } else if (arg == QStringLiteral("--chroma")) {
            QString value;
            if (!require_value(&value))
                return false;
            bool ok = false;
            parsed.chroma = value.toInt(&ok);
            if (!ok || (parsed.chroma != 420 && parsed.chroma != 444)) {
                if (error)
                    *error = QStringLiteral("--chroma requires 420|444");
                return false;
            }
        } else if (arg == QStringLiteral("--bit-depth")) {
            QString value;
            if (!require_value(&value))
                return false;
            bool ok = false;
            parsed.bit_depth = value.toInt(&ok);
            if (!ok || (parsed.bit_depth != 8 && parsed.bit_depth != 10)) {
                if (error)
                    *error = QStringLiteral("--bit-depth requires 8|10");
                return false;
            }
        } else if (arg == QStringLiteral("--hdr")) {
            QString value;
            if (!require_value(&value) || !ParseHdrMode(value, &parsed.hdr_mode)) {
                if (error)
                    *error = QStringLiteral("--hdr requires off|tonemap|native");
                return false;
            }
        } else if (arg == QStringLiteral("--duration")) {
            QString value;
            if (!require_value(&value))
                return false;
            bool ok = false;
            parsed.duration_seconds = value.toInt(&ok);
            if (!ok || parsed.duration_seconds <= 0) {
                if (error)
                    *error = QStringLiteral("--duration requires a positive integer");
                return false;
            }
        } else if (arg == QStringLiteral("--frame-rate")) {
            QString value;
            if (!require_value(&value))
                return false;
            bool ok = false;
            parsed.frame_rate = value.toInt(&ok);
            // Bounded by what the product itself offers (the Expert frame-rate
            // field accepts 1-240). Falling back to 60 on a typo would let a
            // verification run claim a rate it never exercised.
            if (!ok || parsed.frame_rate < 1 || parsed.frame_rate > 240) {
                if (error)
                    *error = QStringLiteral("--frame-rate requires an integer between 1 and 240");
                return false;
            }
        } else if (arg == QStringLiteral("--repeat-cycles")) {
            QString value;
            if (!require_value(&value))
                return false;
            bool ok = false;
            parsed.repeat_cycles = value.toInt(&ok);
            if (!ok || parsed.repeat_cycles <= 0) {
                if (error)
                    *error = QStringLiteral("--repeat-cycles requires a positive integer");
                return false;
            }
        } else if (arg == QStringLiteral("--pause-at")) {
            QString value;
            if (!require_value(&value))
                return false;
            bool ok = false;
            parsed.pause_at_seconds = value.toInt(&ok);
            if (!ok || parsed.pause_at_seconds < 0) {
                if (error)
                    *error = QStringLiteral("--pause-at requires a non-negative integer");
                return false;
            }
        } else if (arg == QStringLiteral("--pause-for")) {
            QString value;
            if (!require_value(&value))
                return false;
            bool ok = false;
            parsed.pause_for_seconds = value.toInt(&ok);
            if (!ok || parsed.pause_for_seconds <= 0) {
                if (error)
                    *error = QStringLiteral("--pause-for requires a positive integer");
                return false;
            }
        } else if (arg == QStringLiteral("--capture-frame-at")) {
            QString value;
            if (!require_value(&value))
                return false;
            bool ok = false;
            parsed.capture_frame_at_seconds = value.toInt(&ok);
            if (!ok) {
                if (error)
                    *error = QStringLiteral("--capture-frame-at requires an integer");
                return false;
            }
        } else if (arg == QStringLiteral("--screenshot-path")) {
            if (!require_value(&parsed.screenshot_path))
                return false;
        } else if (arg == QStringLiteral("--capture-frame-in-ready")) {
            parsed.capture_frame_in_ready = true;
        } else if (arg == QStringLiteral("--benchmark-scenario")) {
            if (!require_value(&parsed.benchmark_scenario))
                return false;
        } else if (arg == QStringLiteral("--benchmark-output")) {
            if (!require_value(&parsed.benchmark_output_dir))
                return false;
        } else if (arg == QStringLiteral("--benchmark-notes")) {
            if (!require_value(&parsed.benchmark_source_notes))
                return false;
        } else if (arg == QStringLiteral("--benchmark-warmup")) {
            QString value;
            if (!require_value(&value))
                return false;
            bool ok = false;
            parsed.benchmark_warmup_seconds = value.toInt(&ok);
            if (!ok || parsed.benchmark_warmup_seconds < 0) {
                if (error)
                    *error = QStringLiteral("--benchmark-warmup requires a non-negative integer");
                return false;
            }
        }
    }

    if (parsed.target == TargetKind::Window && parsed.target_window_title.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("--target=window requires --target-window-title");
        return false;
    }

    // A benchmark run whose reports have nowhere to go is a run whose numbers are
    // lost the moment the process exits — refuse it rather than record for a minute
    // and then discard the measurement.
    if (!parsed.benchmark_scenario.trimmed().isEmpty() && parsed.benchmark_output_dir.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("--benchmark-scenario requires --benchmark-output");
        return false;
    }
    if (parsed.benchmark_scenario.trimmed().isEmpty() && parsed.benchmark_warmup_seconds > 0) {
        if (error)
            *error = QStringLiteral("--benchmark-warmup requires --benchmark-scenario");
        return false;
    }

    *out = parsed;
    return true;
}

} // namespace exosnap::auto_record
