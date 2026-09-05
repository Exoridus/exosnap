#include "diagnostics/SessionReport.h"

#include "observability/ObservabilityJson.h"
#include "ui/CodecLabels.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace exosnap::diagnostics {
namespace {

using exosnap::engine::MetricAvailability;

// A metric value that is either a real number or the availability word — never a
// fabricated zero (honest-diagnostics rule). The word rather than a bare
// "unavailable" for every non-measurement: a release gate reading these counters
// has to tell a measurement that never arrived from one that arrived wrong, and
// it shares the vocabulary with every other observability surface so the two
// cannot drift apart.
QJsonValue MetricOrUnavailable(double value, MetricAvailability availability) {
    if (availability != MetricAvailability::Available) {
        return exosnap::observability::AvailabilityKey(availability);
    }
    return value;
}

QString RateControlLabel(exosnap::engine::RateControlMode mode) {
    switch (mode) {
    case exosnap::engine::RateControlMode::ConstantQuality:
        return QStringLiteral("CQ");
    case exosnap::engine::RateControlMode::VariableBitrate:
        return QStringLiteral("VBR");
    case exosnap::engine::RateControlMode::ConstantBitrate:
        return QStringLiteral("CBR");
    case exosnap::engine::RateControlMode::Lossless:
        return QStringLiteral("Lossless");
    }
    return QStringLiteral("CQ");
}

QString PresetLabel(exosnap::engine::NvencPreset preset) {
    return QStringLiteral("P%1").arg(static_cast<int>(preset) + 1);
}

QJsonObject BuildEncoderInit(const exosnap::engine::EncoderInitInfo& init) {
    QJsonObject o;
    o[QStringLiteral("codec")] = ui::videoCodecLabel(init.codec);
    o[QStringLiteral("preset")] = PresetLabel(init.preset);
    o[QStringLiteral("rate_control")] = RateControlLabel(init.rc_mode);
    o[QStringLiteral("target_bitrate_kbps")] = static_cast<double>(init.target_bitrate_kbps);
    o[QStringLiteral("max_bitrate_kbps")] = static_cast<double>(init.max_bitrate_kbps);
    o[QStringLiteral("cq")] = static_cast<double>(init.cq);
    o[QStringLiteral("gop_length")] = static_cast<double>(init.gop_length);
    o[QStringLiteral("bframes")] = static_cast<double>(init.bframes);
    o[QStringLiteral("lookahead_frames")] = static_cast<double>(init.lookahead_frames);
    o[QStringLiteral("temporal_aq")] = init.temporal_aq;
    o[QStringLiteral("spatial_aq")] = init.spatial_aq;
    o[QStringLiteral("bit_depth")] = (init.bit_depth == exosnap::engine::BitDepth::Bit10) ? 10 : 8;
    o[QStringLiteral("color_full_range")] = init.color_full_range;
    return o;
}

} // namespace

QByteArray BuildSessionReportJson(const SessionReportInputs& inputs) {
    QJsonObject root;
    root[QStringLiteral("schema_version")] = inputs.schema_version;
    root[QStringLiteral("recording_session_id")] = inputs.recording_session_id;
    root[QStringLiteral("launch_session_id")] = inputs.launch_session_id;
    if (!inputs.app_version.isEmpty()) {
        root[QStringLiteral("app_version")] = inputs.app_version;
    }
    if (!inputs.started_at.isEmpty()) {
        root[QStringLiteral("started_at")] = inputs.started_at;
    }
    if (!inputs.ended_at.isEmpty()) {
        root[QStringLiteral("ended_at")] = inputs.ended_at;
    }
    root[QStringLiteral("succeeded")] = inputs.result.succeeded;
    if (!inputs.output_filename.isEmpty()) {
        root[QStringLiteral("output_filename")] = inputs.output_filename;
    }

    // ---- Resolved output format (from the recording result) ----
    {
        QJsonObject fmt;
        fmt[QStringLiteral("container")] = ui::containerLabel(inputs.result.container);
        fmt[QStringLiteral("video_codec")] = ui::videoCodecLabel(inputs.result.video_codec);
        fmt[QStringLiteral("audio_codec")] = ui::audioCodecLabel(inputs.result.audio_codec);
        fmt[QStringLiteral("source_width")] = static_cast<double>(inputs.result.source_width);
        fmt[QStringLiteral("source_height")] = static_cast<double>(inputs.result.source_height);
        fmt[QStringLiteral("output_width")] = static_cast<double>(inputs.result.output_width);
        fmt[QStringLiteral("output_height")] = static_cast<double>(inputs.result.output_height);
        fmt[QStringLiteral("frame_rate_num")] = static_cast<double>(inputs.result.frame_rate_num);
        fmt[QStringLiteral("frame_rate_den")] = static_cast<double>(inputs.result.frame_rate_den);
        fmt[QStringLiteral("cfr")] = inputs.result.cfr;
        fmt[QStringLiteral("output_file_bytes")] = static_cast<double>(inputs.result.output_file_bytes);
        fmt[QStringLiteral("elapsed_seconds")] = inputs.result.elapsed_seconds;
        root[QStringLiteral("output_format")] = fmt;
    }

    // ---- Capture stall (QCR-804, independent of the snapshot) ----
    // Always emitted, including the zero: "this session had no capture stall" is
    // itself the answer a reader of a frozen-looking recording is after, and an
    // absent key would read as "the check does not exist in this build".
    {
        QJsonObject stall;
        stall[QStringLiteral("episodes")] = static_cast<double>(inputs.window_capture_stall_episodes);
        stall[QStringLiteral("detected")] = inputs.window_capture_stall_episodes > 0;
        root[QStringLiteral("window_capture_stall")] = stall;
    }

    // ---- Session ledger (independent of the snapshot) ----
    if (!inputs.ledger.empty()) {
        QJsonArray entries;
        for (const LedgerEntry& entry : inputs.ledger) {
            QJsonObject e;
            e[QStringLiteral("id")] = QString::fromStdString(entry.id);
            e[QStringLiteral("title")] = QString::fromStdString(entry.title);
            e[QStringLiteral("count")] = static_cast<double>(entry.count);
            e[QStringLiteral("first_seen_s")] = entry.first_seen_s;
            e[QStringLiteral("last_seen_s")] = entry.last_seen_s;
            e[QStringLiteral("total_active_s")] = entry.total_active_s;
            // A check that measures no number reports the word, not a zero.
            e[QStringLiteral("worst")] =
                entry.worst.has_value()
                    ? QJsonValue(*entry.worst)
                    : QJsonValue(exosnap::observability::AvailabilityKey(MetricAvailability::Unavailable));
            e[QStringLiteral("budget")] =
                entry.budget.has_value()
                    ? QJsonValue(*entry.budget)
                    : QJsonValue(exosnap::observability::AvailabilityKey(MetricAvailability::Unavailable));
            e[QStringLiteral("unit")] = QString::fromStdString(entry.unit);
            QJsonArray occurrences;
            for (const LedgerOccurrence& occurrence : entry.occurrences) {
                QJsonObject o;
                o[QStringLiteral("start_s")] = occurrence.start_s;
                o[QStringLiteral("end_s")] = occurrence.end_s;
                o[QStringLiteral("worst")] = occurrence.worst;
                occurrences.append(o);
            }
            e[QStringLiteral("occurrences")] = occurrences;
            entries.append(e);
        }
        root[QStringLiteral("ledger")] = entries;
    }

    // ---- Requested config (independent of the snapshot) ----
    {
        QJsonObject cfg;
        cfg[QStringLiteral("capture_backend")] = inputs.capture_backend;
        cfg[QStringLiteral("bit_depth")] = inputs.bit_depth;
        cfg[QStringLiteral("chroma")] = inputs.chroma;
        cfg[QStringLiteral("color_range")] = inputs.color_range;
        cfg[QStringLiteral("hdr_mode")] = inputs.hdr_mode;
        root[QStringLiteral("config")] = cfg;
    }

    // ---- Encoder init parameters (from the snapshot when available) ----
    if (inputs.has_snapshot && inputs.snapshot.encoder_init.valid) {
        root[QStringLiteral("encoder_init")] = BuildEncoderInit(inputs.snapshot.encoder_init);
    } else {
        root[QStringLiteral("encoder_init")] = QStringLiteral("unavailable");
    }

    // ---- Counters from the final snapshot ----
    if (inputs.has_snapshot) {
        const auto& s = inputs.snapshot;
        QJsonObject counters;

        QJsonObject drops;
        drops[QStringLiteral("coalesced")] = static_cast<double>(s.capture.frames_dropped_coalesced);
        drops[QStringLiteral("cfr")] = static_cast<double>(s.capture.frames_dropped_cfr);
        drops[QStringLiteral("backpressure")] = static_cast<double>(s.capture.frames_dropped_backpressure);
        drops[QStringLiteral("processing_failure")] = static_cast<double>(s.capture.frames_dropped_processing_failure);
        counters[QStringLiteral("frames_dropped")] = drops;
        counters[QStringLiteral("frames_duplicated")] = static_cast<double>(s.capture.frames_duplicated);
        counters[QStringLiteral("frames_captured")] = static_cast<double>(s.capture.frames_captured);
        counters[QStringLiteral("frames_emitted")] = static_cast<double>(s.capture.frames_emitted);

        counters[QStringLiteral("audio_discontinuities")] =
            MetricOrUnavailable(static_cast<double>(s.audio.discontinuities), s.audio.discontinuity_availability);
        // Milliseconds, not frames: the acceptance criterion is how much audio a
        // listener lost, which a frame count cannot state without the sample rate.
        const double disc_rate = s.audio.sample_rate > 0 ? static_cast<double>(s.audio.sample_rate) : 0.0;
        counters[QStringLiteral("audio_discontinuity_ms_total")] = MetricOrUnavailable(
            disc_rate > 0.0 ? static_cast<double>(s.audio.discontinuity_frames_total) * 1000.0 / disc_rate : 0.0,
            disc_rate > 0.0 ? s.audio.discontinuity_availability : MetricAvailability::Unavailable);
        counters[QStringLiteral("audio_discontinuity_ms_longest")] = MetricOrUnavailable(
            disc_rate > 0.0 ? static_cast<double>(s.audio.discontinuity_frames_longest) * 1000.0 / disc_rate : 0.0,
            disc_rate > 0.0 ? s.audio.discontinuity_availability : MetricAvailability::Unavailable);

        counters[QStringLiteral("encoder_submitted")] = static_cast<double>(s.video_encoder.frames_submitted);
        counters[QStringLiteral("encoder_encoded")] = static_cast<double>(s.video_encoder.frames_encoded);
        counters[QStringLiteral("encoder_backlog")] = static_cast<double>(s.video_encoder.backlog);
        counters[QStringLiteral("encoder_forced_keyframes")] = static_cast<double>(s.video_encoder.forced_keyframes);
        // How often the driver's actual pictureType disagreed with the
        // submission-side GOP-phase prediction. Warn-only during the recording,
        // so a soak run needs the end-of-session total to see it happened at
        // all: non-zero means the enforced FORCEIDR cadence and the picture
        // types NVENC actually produced diverged, which is what to look at
        // first when keyframe spacing (and therefore seek granularity or a
        // split's cut point) looks wrong. The sibling outputTimeStamp check is
        // deliberately not reported here — it aborts the encode instead of
        // producing a packet, so its counter can never be anything but 0 and
        // printing it would suggest a check that had a chance to fire.
        counters[QStringLiteral("encoder_keyframe_prediction_mismatches")] =
            static_cast<double>(s.video_encoder.keyframe_prediction_mismatches);
        counters[QStringLiteral("mux_failures")] = static_cast<double>(s.mux.failures);

        counters[QStringLiteral("duration_skew_ms")] =
            MetricOrUnavailable(s.duration_skew_ms, s.duration_skew_availability);
        counters[QStringLiteral("av_drift_ms")] = MetricOrUnavailable(s.av_drift_ms, s.av_drift_availability);
        counters[QStringLiteral("peak_av_drift_ms")] =
            MetricOrUnavailable(s.peak_av_drift_ms, s.peak_av_drift_availability);

        // Clock slaving, from the same track av_drift_ms is taken from and therefore
        // gated on the same availability: the raw device-vs-QPC drift before
        // compensation, the compensation rate the controller ended on, and how far it
        // had shifted the timeline at the end (raw - residual). A soak run reads these
        // together with peak_av_drift_ms: a large correction with a small residual is
        // slaving working, not a defect.
        counters[QStringLiteral("av_drift_raw_ms")] = MetricOrUnavailable(s.av_drift_raw_ms, s.av_drift_availability);
        counters[QStringLiteral("clock_slaving_ppm")] =
            MetricOrUnavailable(s.clock_slaving_ppm, s.av_drift_availability);
        counters[QStringLiteral("clock_slaving_compensation_ms")] =
            MetricOrUnavailable(s.av_drift_raw_ms - s.av_drift_ms, s.av_drift_availability);
        counters[QStringLiteral("clock_slaving_active")] = s.clock_slaving_active;

        root[QStringLiteral("counters")] = counters;
        root[QStringLiteral("pipeline_health")] = QString::fromUtf8(exosnap::engine::ToString(s.health));
        root[QStringLiteral("bottleneck")] = QString::fromUtf8(exosnap::engine::ToString(s.bottleneck));

        // ---- Audio end-of-session facts ----
        // degraded_sources is the count still lost when the recording ended;
        // degraded_occurred is the latched "it happened at least once" bit. The
        // engine keeps no timestamped device-event history, so neither the moment
        // of a loss nor the number of loss episodes can be reported here.
        {
            QJsonObject audio;
            audio[QStringLiteral("track_count")] = static_cast<double>(s.audio.track_count);
            audio[QStringLiteral("degraded_sources_at_end")] = static_cast<double>(s.audio.degraded_sources);
            audio[QStringLiteral("degraded_occurred")] = s.audio.source_degraded_occurred;

            // Resampler tail flushed at stop, per track. undrained > 0 means captured
            // audio was dropped instead of encoded. A track that never reached its
            // drain (failed session, join timeout, no resample context) reports
            // "unavailable" — its counters sit at their initial 0, and printing that
            // would claim a clean drain that never happened.
            QJsonArray drain;
            const int tracks = std::min<int>(static_cast<int>(s.audio.resampler_drained_frames.size()),
                                             std::max<int>(0, static_cast<int>(s.audio.track_count)));
            for (int i = 0; i < tracks; ++i) {
                const auto idx = static_cast<std::size_t>(i);
                const MetricAvailability drained = s.audio.resampler_drain_recorded[idx]
                                                       ? MetricAvailability::Available
                                                       : MetricAvailability::Unavailable;
                QJsonObject t;
                t[QStringLiteral("track")] = i;
                t[QStringLiteral("drained_frames")] =
                    MetricOrUnavailable(static_cast<double>(s.audio.resampler_drained_frames[idx]), drained);
                t[QStringLiteral("undrained_frames")] =
                    MetricOrUnavailable(static_cast<double>(s.audio.resampler_undrained_frames[idx]), drained);
                drain.append(t);
            }
            audio[QStringLiteral("resampler_drain")] = drain;
            root[QStringLiteral("audio")] = audio;
        }

        // ---- Video pacing ----
        // The CFR target the pipeline paced to, and whether it held it over the whole
        // session. Deliberately NOT capture.actual_fps: that is an instantaneous rate
        // over the last publish window, and on the terminal snapshot (built after the
        // workers joined) it measures the finalize gap, not the recording. The session
        // average is the honest whole-run answer. Caveat: the denominator is the session's
        // elapsed time, which includes paused and finalize time — a session paused for half
        // its length averages ~30 where it paced 60. Renormalizing onto recording-active
        // time needs a paused-time accumulator the engine does not keep; for an unpaused
        // soak run the average is exact. The pacing *outcome* stays where it already lives:
        // counters.frames_duplicated (CFR holds) and counters.frames_dropped.cfr (ticks
        // that produced no frame).
        {
            QJsonObject pacing;
            pacing[QStringLiteral("cfr")] = inputs.result.cfr;
            pacing[QStringLiteral("target_fps")] = s.capture.target_fps;
            pacing[QStringLiteral("average_emitted_fps")] =
                (s.elapsed_seconds > 0.0)
                    ? QJsonValue(static_cast<double>(s.capture.frames_emitted) / s.elapsed_seconds)
                    : QJsonValue(QStringLiteral("unavailable"));
            // Only observed on VFR capture. On CFR the snapshot field holds the nominal
            // interval derived from the target rate, which is not a measurement — so the
            // report publishes "unavailable" there rather than echoing 1000/target_fps.
            pacing[QStringLiteral("frame_interval_ms")] =
                MetricOrUnavailable(s.capture.frame_interval_ms, s.capture.interval_observed);
            root[QStringLiteral("video_pacing")] = pacing;
        }
    } else {
        root[QStringLiteral("counters")] = QStringLiteral("unavailable");
        root[QStringLiteral("audio")] = QStringLiteral("unavailable");
        root[QStringLiteral("video_pacing")] = QStringLiteral("unavailable");
    }

    // ---- Segment list (from the recording result) ----
    {
        QJsonArray segments;
        for (const auto& seg : inputs.result.segments) {
            QJsonObject o;
            o[QStringLiteral("index")] = static_cast<double>(seg.index);
            o[QStringLiteral("duration_seconds")] = seg.duration_seconds;
            o[QStringLiteral("bytes")] = static_cast<double>(seg.file_size_bytes);
            o[QStringLiteral("finalized")] = seg.succeeded;
            segments.append(o);
        }
        root[QStringLiteral("segments")] = segments;
    }

    // ---- Error phase (only on failure) ----
    if (!inputs.result.succeeded) {
        QJsonObject err;
        err[QStringLiteral("phase")] = QString::fromStdWString(inputs.result.error_phase);
        err[QStringLiteral("hresult")] = QString::fromStdWString(inputs.result.hresult_text);
        err[QStringLiteral("detail")] = QString::fromStdWString(inputs.result.error_detail);
        root[QStringLiteral("error")] = err;
    }

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool WriteSessionReport(const QString& reports_dir, const SessionReportInputs& inputs, int keep_n, QString* out_path,
                        QString* out_error) {
    if (inputs.recording_session_id.trimmed().isEmpty()) {
        if (out_error)
            *out_error = QStringLiteral("missing recording session id");
        return false;
    }

    QDir dir(reports_dir);
    if (!dir.mkpath(QStringLiteral("."))) {
        if (out_error)
            *out_error = QStringLiteral("could not create reports directory");
        return false;
    }

    const QString file_path = dir.filePath(QStringLiteral("session-%1.json").arg(inputs.recording_session_id));
    QSaveFile file(file_path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (out_error)
            *out_error = file.errorString();
        return false;
    }
    file.write(BuildSessionReportJson(inputs));
    if (!file.commit()) {
        if (out_error)
            *out_error = file.errorString();
        return false;
    }

    // Prune: keep the newest keep_n reports (by modification time).
    if (keep_n > 0) {
        QFileInfoList reports =
            dir.entryInfoList({QStringLiteral("session-*.json")}, QDir::Files, QDir::Time); // newest first
        for (int i = keep_n; i < reports.size(); ++i) {
            QFile::remove(reports[i].absoluteFilePath());
        }
    }

    if (out_path)
        *out_path = file_path;
    return true;
}

} // namespace exosnap::diagnostics
