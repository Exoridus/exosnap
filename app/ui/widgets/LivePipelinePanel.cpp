#include "LivePipelinePanel.h"

#include "../theme/ExoSnapMetrics.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace exosnap::ui::widgets {

using M = ui::theme::ExoSnapMetrics;
using namespace recorder_core;

namespace {

constexpr const char* kDash = "—"; // em dash

QString fmt1(double v) {
    return QString::number(v, 'f', 1);
}

QString fmtFps(double fps) {
    if (fps <= 0.0)
        return QString::fromUtf8(kDash);
    return fmt1(fps);
}

QString fmtMs(double ms) {
    return fmt1(ms) + QStringLiteral(" ms");
}

QString fmtMiB(double mib_s) {
    return fmt1(mib_s) + QStringLiteral(" MiB/s");
}

QString fmtBytesIec(uint64_t bytes) {
    constexpr double kib = 1024.0;
    const double b = static_cast<double>(bytes);
    if (b < kib)
        return QString::number(bytes) + QStringLiteral(" B");
    if (b < kib * kib)
        return fmt1(b / kib) + QStringLiteral(" KiB");
    if (b < kib * kib * kib)
        return fmt1(b / (kib * kib)) + QStringLiteral(" MiB");
    return fmt1(b / (kib * kib * kib)) + QStringLiteral(" GiB");
}

QString fmtElapsed(double seconds) {
    if (seconds < 0.0)
        seconds = 0.0;
    const int total = static_cast<int>(seconds);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString sourceTypeText(CaptureSourceType t) {
    switch (t) {
    case CaptureSourceType::Display:
        return QStringLiteral("Display");
    case CaptureSourceType::Window:
        return QStringLiteral("Window");
    case CaptureSourceType::Region:
        return QStringLiteral("Region");
    case CaptureSourceType::Unknown:
        break;
    }
    return QStringLiteral("Unknown");
}

QString presentModeText(PresentMode m) {
    switch (m) {
    case PresentMode::Composed:
        return QStringLiteral("Composed");
    case PresentMode::IndependentFlip:
        return QStringLiteral("Independent flip");
    case PresentMode::ExclusiveFullscreen:
        return QStringLiteral("Exclusive fullscreen");
    case PresentMode::Unknown:
        break;
    }
    return QStringLiteral("Unknown");
}

QString videoCodecText(VideoCodec c) {
    return c == VideoCodec::H264Nvenc ? QStringLiteral("H.264") : QStringLiteral("AV1");
}

QString audioCodecText(AudioCodec c) {
    switch (c) {
    case AudioCodec::AacMf:
        return QStringLiteral("AAC");
    case AudioCodec::Opus:
        return QStringLiteral("Opus");
    case AudioCodec::Pcm:
        return QStringLiteral("PCM");
    case AudioCodec::Flac:
        return QStringLiteral("FLAC");
    }
    return QStringLiteral("Opus");
}

QString triggerText(DiagnosticsSplitTrigger t) {
    switch (t) {
    case DiagnosticsSplitTrigger::AutomaticDuration:
        return QStringLiteral("auto");
    case DiagnosticsSplitTrigger::AutomaticSize:
        return QStringLiteral("size");
    case DiagnosticsSplitTrigger::ManualButton:
        return QStringLiteral("manual");
    case DiagnosticsSplitTrigger::Hotkey:
        return QStringLiteral("hotkey");
    case DiagnosticsSplitTrigger::None:
        break;
    }
    return QString::fromUtf8(kDash);
}

QString healthWord(PipelineHealth h) {
    return QString::fromLatin1(ToString(h));
}

QString lifecycleText(DiagnosticsLifecycle lc, double elapsed) {
    switch (lc) {
    case DiagnosticsLifecycle::Idle:
        return QStringLiteral("No active recording");
    case DiagnosticsLifecycle::Initializing:
        return QStringLiteral("Initializing…");
    case DiagnosticsLifecycle::Recording:
        return QStringLiteral("Recording · ") + fmtElapsed(elapsed);
    case DiagnosticsLifecycle::Paused:
        return QStringLiteral("Paused · ") + fmtElapsed(elapsed);
    case DiagnosticsLifecycle::Stopping:
        return QStringLiteral("Finalizing…");
    case DiagnosticsLifecycle::Completed:
        return QStringLiteral("Completed · ") + fmtElapsed(elapsed);
    case DiagnosticsLifecycle::Failed:
        return QStringLiteral("Failed");
    }
    return QString();
}

QString healthColor(PipelineHealth h) {
    switch (h) {
    case PipelineHealth::Good:
        return QStringLiteral("#3fb950");
    case PipelineHealth::Warning:
        return QStringLiteral("#d29922");
    case PipelineHealth::Critical:
        return QStringLiteral("#f85149");
    case PipelineHealth::Idle:
    case PipelineHealth::Unavailable:
        break;
    }
    return QStringLiteral("#8b949e");
}

} // namespace

LivePipelinePanel::LivePipelinePanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("livePipelinePanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(M::kSpaceSm);

    // Status header: health + bottleneck and the lifecycle banner.
    auto* header = new QWidget(this);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(M::kSpaceMd);
    status_pill_ = new QLabel(header);
    status_pill_->setObjectName(QStringLiteral("livePipelineStatus"));
    lifecycle_label_ = new QLabel(header);
    lifecycle_label_->setObjectName(QStringLiteral("livePipelineLifecycle"));
    lifecycle_label_->setProperty("labelRole", "mono");
    header_layout->addWidget(status_pill_);
    header_layout->addStretch(1);
    header_layout->addWidget(lifecycle_label_);
    root->addWidget(header);

    auto* capture = addSection(root, QStringLiteral("Capture"));
    addRow(capture, QStringLiteral("liveCaptureFps"), QStringLiteral("Frame rate"));
    addRow(capture, QStringLiteral("liveCaptureSource"), QStringLiteral("Source"));
    addRow(capture, QStringLiteral("liveCaptureInterval"), QStringLiteral("Frame interval"));
    addRow(capture, QStringLiteral("liveCapturePresent"), QStringLiteral("Present cadence"));
    addRow(capture, QStringLiteral("liveCapturePresentMode"), QStringLiteral("Present mode"));
    addRow(capture, QStringLiteral("liveCaptureFrames"), QStringLiteral("Frames"));
    addRow(capture, QStringLiteral("liveCaptureDrops"), QStringLiteral("Drops"));

    auto* video = addSection(root, QStringLiteral("Video / Compositor"));
    addRow(video, QStringLiteral("liveCompositor"), QStringLiteral("Compositor"));
    addRow(video, QStringLiteral("liveEncoder"), QStringLiteral("Encoder"));
    addRow(video, QStringLiteral("liveEncoderCounts"), QStringLiteral("Encoder counts"));
    addRow(video, QStringLiteral("liveEncoderCodec"), QStringLiteral("Codec"));

    auto* audio = addSection(root, QStringLiteral("Audio"));
    addRow(audio, QStringLiteral("liveAudio"), QStringLiteral("Format"));
    addRow(audio, QStringLiteral("liveAudioCounts"), QStringLiteral("Audio counts"));

    auto* mux = addSection(root, QStringLiteral("Mux / Disk"));
    addRow(mux, QStringLiteral("liveMux"), QStringLiteral("Mux"));
    addRow(mux, QStringLiteral("liveMuxWrite"), QStringLiteral("Write call"));
    addRow(mux, QStringLiteral("liveDisk"), QStringLiteral("Disk"));
    addRow(mux, QStringLiteral("liveReorder"), QStringLiteral("Reorder window"));

    auto* queues = addSection(root, QStringLiteral("Queues"));
    addRow(queues, QStringLiteral("liveVideoQueue"), QStringLiteral("Video queue"));
    addRow(queues, QStringLiteral("liveAudioQueue"), QStringLiteral("Audio queue"));

    auto* segment = addSection(root, QStringLiteral("Current segment"));
    addRow(segment, QStringLiteral("liveSegment"), QStringLiteral("Segment"));
    addRow(segment, QStringLiteral("liveSplit"), QStringLiteral("Split"));

    setIdle();
}

QWidget* LivePipelinePanel::addSection(QVBoxLayout* parent_layout, const QString& title) {
    auto* title_label = new QLabel(title, this);
    title_label->setProperty("labelRole", "sectionMini");
    parent_layout->addWidget(title_label);

    auto* section = new QFrame(this);
    section->setProperty("panelRole", "panel");
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(M::kSpaceSm, M::kSpaceXs, M::kSpaceSm, M::kSpaceXs);
    layout->setSpacing(M::kSpaceXs);
    parent_layout->addWidget(section);
    return section;
}

void LivePipelinePanel::addRow(QWidget* section, const QString& key, const QString& caption) {
    auto* row = new QWidget(section);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(M::kSpaceMd);

    auto* caption_label = new QLabel(caption, row);
    caption_label->setProperty("labelRole", "body");
    caption_label->setMinimumWidth(140);
    row_layout->addWidget(caption_label);

    auto* value_label = new QLabel(QString::fromUtf8(kDash), row);
    value_label->setObjectName(key);
    value_label->setProperty("labelRole", "mono");
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row_layout->addWidget(value_label, 1);

    section->layout()->addWidget(row);
    values_.insert(key, value_label);
}

void LivePipelinePanel::setValue(const QString& key, const QString& text) {
    if (auto* label = values_.value(key, nullptr)) {
        label->setText(text);
    }
}

void LivePipelinePanel::setNeutral(const QString& lifecycle_text) {
    lifecycle_label_->setText(lifecycle_text);
    status_pill_->setText(QStringLiteral("Idle"));
    status_pill_->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(healthColor(PipelineHealth::Idle)));
    const QString dash = QString::fromUtf8(kDash);
    for (auto it = values_.begin(); it != values_.end(); ++it) {
        it.value()->setText(dash);
    }
}

void LivePipelinePanel::setIdle() {
    setNeutral(QStringLiteral("No active recording"));
}

void LivePipelinePanel::applySnapshot(const RecordingDiagnosticsSnapshot& s) {
    lifecycle_label_->setText(lifecycleText(s.lifecycle, s.elapsed_seconds));

    // Idle / Initializing: neutral values, never stale active-looking numbers.
    if (!s.valid) {
        setNeutral(lifecycleText(s.lifecycle, s.elapsed_seconds));
        if (s.lifecycle == DiagnosticsLifecycle::Initializing) {
            status_pill_->setText(QStringLiteral("Initializing…"));
        }
        return;
    }

    // Status pill: health + bottleneck (+ reason).
    QString status = healthWord(s.health);
    if (s.bottleneck == PipelineBottleneck::None) {
        status += QStringLiteral(" · No bottleneck");
    } else if (s.bottleneck == PipelineBottleneck::Unknown) {
        status += QStringLiteral(" · Gathering data");
    } else {
        status += QStringLiteral(" · Bottleneck: ") + QString::fromLatin1(ToString(s.bottleneck));
        if (!s.bottleneck_reason.empty()) {
            status += QStringLiteral(" (") + QString::fromStdString(s.bottleneck_reason) + QStringLiteral(")");
        }
    }
    status_pill_->setText(status);
    status_pill_->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(healthColor(s.health)));

    // ---- Capture ----
    const CaptureDiagnostics& cap = s.capture;
    setValue(QStringLiteral("liveCaptureFps"),
             fmtFps(cap.actual_fps) + QStringLiteral(" / ") + fmtFps(cap.target_fps) + QStringLiteral(" FPS"));
    QString src = sourceTypeText(cap.source_type);
    if (cap.source_loss)
        src += QStringLiteral(" · SOURCE LOST");
    setValue(QStringLiteral("liveCaptureSource"), src);
    setValue(QStringLiteral("liveCaptureInterval"), cap.interval_observed == MetricAvailability::Available
                                                        ? fmtMs(cap.frame_interval_ms) + QStringLiteral(" (observed)")
                                                        : fmtMs(cap.frame_interval_ms) + QStringLiteral(" (target)"));
    // Present cadence (VRR/CFR judder correlation). DXGI OD only; neutral em-dash otherwise.
    if (cap.present_cadence_availability == MetricAvailability::Available) {
        setValue(QStringLiteral("liveCapturePresent"),
                 QStringLiteral("%1 interval · %2 jitter · ×%3 coalesce")
                     .arg(fmtMs(cap.source_present_interval_ms), fmtMs(cap.source_present_jitter_ms))
                     .arg(QString::number(cap.source_coalesce_ratio, 'f', 2)));
    } else {
        setValue(QStringLiteral("liveCapturePresent"), QString::fromUtf8(kDash));
    }
    // Present mode + tearing (PresentMon ETW, ADR 0033). Elevation/opt-in-gated; neutral
    // em-dash until the in-process consumer is vendored and a present is observed.
    if (cap.present_mode_availability == MetricAvailability::Available) {
        QString mode_text = presentModeText(cap.source_present_mode);
        if (cap.source_tearing)
            mode_text += QStringLiteral(" · tearing");
        setValue(QStringLiteral("liveCapturePresentMode"), mode_text);
    } else {
        setValue(QStringLiteral("liveCapturePresentMode"), QString::fromUtf8(kDash));
    }
    setValue(QStringLiteral("liveCaptureFrames"), QStringLiteral("captured %1 · emitted %2 · dup %3")
                                                      .arg(cap.frames_captured)
                                                      .arg(cap.frames_emitted)
                                                      .arg(cap.frames_duplicated));
    setValue(QStringLiteral("liveCaptureDrops"), QStringLiteral("coalesce %1 · cfr %2 · backpressure %3")
                                                     .arg(cap.frames_dropped_coalesced)
                                                     .arg(cap.frames_dropped_cfr)
                                                     .arg(cap.frames_dropped_backpressure));

    // ---- Compositor / Encoder ----
    const CompositorDiagnostics& comp = s.compositor;
    setValue(QStringLiteral("liveCompositor"),
             comp.active
                 ? QStringLiteral("%1 avg · %2 peak (CPU submit)").arg(fmtMs(comp.average_ms), fmtMs(comp.peak_ms))
                 : QStringLiteral("Inactive (no overlay)"));

    const EncoderDiagnostics& enc = s.video_encoder;
    setValue(QStringLiteral("liveEncoder"),
             QStringLiteral("%1 avg · %2 peak · %3 FPS")
                 .arg(fmtMs(enc.average_ms), fmtMs(enc.peak_ms), fmtFps(enc.output_fps)));
    setValue(QStringLiteral("liveEncoderCounts"),
             QStringLiteral("submitted %1 · encoded %2 · backlog %3 · keyframes %4")
                 .arg(enc.frames_submitted)
                 .arg(enc.frames_encoded)
                 .arg(enc.backlog)
                 .arg(enc.forced_keyframes));
    setValue(QStringLiteral("liveEncoderCodec"), QStringLiteral("%1 %2×%3 %4")
                                                     .arg(videoCodecText(enc.codec))
                                                     .arg(enc.width)
                                                     .arg(enc.height)
                                                     .arg(enc.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR")));

    // ---- Audio ----
    const AudioDiagnostics& au = s.audio;
    if (!au.active) {
        setValue(QStringLiteral("liveAudio"), QStringLiteral("No audio"));
        setValue(QStringLiteral("liveAudioCounts"), QString::fromUtf8(kDash));
    } else {
        setValue(
            QStringLiteral("liveAudio"),
            au.sample_rate == 0
                ? QStringLiteral("%1 · %2 track(s) (initializing)").arg(audioCodecText(au.codec)).arg(au.track_count)
                : QStringLiteral("%1 · %2 Hz · %3 ch · %4 track(s)")
                      .arg(audioCodecText(au.codec))
                      .arg(au.sample_rate)
                      .arg(au.channels)
                      .arg(au.track_count));
        setValue(QStringLiteral("liveAudioCounts"), QStringLiteral("packets %1 · queue %2 · peak %3 · disc %4")
                                                        .arg(au.packets_encoded)
                                                        .arg(au.queue_depth)
                                                        .arg(au.queue_peak)
                                                        .arg(au.discontinuities));
    }

    // ---- Mux / Disk ----
    const MuxDiagnostics& mux = s.mux;
    const DiskDiagnostics& disk = s.disk;
    // The MP4 (IMFSinkWriter) path has no measurable filesystem write boundary, so its
    // throughput / write-call latency / written-bytes are Unavailable, never a fake zero.
    const bool write_boundary_measured = (disk.latency_availability == MetricAvailability::Available);
    setValue(QStringLiteral("liveMux"),
             write_boundary_measured
                 ? QStringLiteral("packets %1 · %2").arg(mux.packets_processed).arg(fmtMiB(mux.throughput_mib_s))
                 : QStringLiteral("packets %1 · throughput Unavailable").arg(mux.packets_processed));
    setValue(
        QStringLiteral("liveMuxWrite"),
        write_boundary_measured
            ? QStringLiteral("write call %1 avg · %2 peak").arg(fmtMs(disk.average_write_ms), fmtMs(disk.peak_write_ms))
            : QStringLiteral("Unavailable (no measurable write boundary)"));
    const QString target =
        disk.output_target.empty() ? QString::fromUtf8(kDash) : QString::fromStdString(disk.output_target);
    QString disk_text = write_boundary_measured
                            ? QStringLiteral("%1 · %2 written").arg(target, fmtBytesIec(disk.bytes_written))
                            : QStringLiteral("%1 · written Unavailable").arg(target);
    if (disk.write_failures > 0)
        disk_text += QStringLiteral(" · FAILURES %1").arg(disk.write_failures);
    setValue(QStringLiteral("liveDisk"), disk_text);
    setValue(QStringLiteral("liveReorder"),
             mux.availability == MetricAvailability::Unavailable
                 ? QStringLiteral("Unavailable (MP4)")
                 : QStringLiteral("%1 pkt / %2 peak · %3 / %4 peak")
                       .arg(mux.reorder_packets)
                       .arg(mux.reorder_packets_peak)
                       .arg(fmtBytesIec(mux.reorder_bytes), fmtBytesIec(mux.reorder_bytes_peak)));

    // ---- Queues ----
    const QueueDiagnostics& vq = s.video_queue;
    setValue(QStringLiteral("liveVideoQueue"),
             vq.bounded ? QStringLiteral("%1 / %2 · peak %3").arg(vq.current_depth).arg(vq.capacity).arg(vq.peak_depth)
                        : QStringLiteral("%1 · peak %2 · unbounded").arg(vq.current_depth).arg(vq.peak_depth));
    const QueueDiagnostics& aq = s.audio_queue;
    setValue(QStringLiteral("liveAudioQueue"),
             aq.bounded ? QStringLiteral("%1 / %2 · peak %3").arg(aq.current_depth).arg(aq.capacity).arg(aq.peak_depth)
                        : QStringLiteral("%1 · peak %2").arg(aq.current_depth).arg(aq.peak_depth));

    // ---- Segment / Split ----
    const SplitDiagnostics& sp = s.split;
    if (!sp.split_supported) {
        setValue(QStringLiteral("liveSegment"), QStringLiteral("Single file (no split)"));
        setValue(QStringLiteral("liveSplit"), QStringLiteral("Split unavailable for this container"));
    } else {
        setValue(QStringLiteral("liveSegment"),
                 QStringLiteral("segment %1 · completed %2").arg(sp.current_segment).arg(sp.completed_segments));
        QString split_text =
            QStringLiteral("pending: %1 · last: %2")
                .arg(sp.split_pending ? QStringLiteral("yes") : QStringLiteral("no"), triggerText(sp.last_trigger));
        if (sp.seconds_until_auto_split >= 0.0)
            split_text += QStringLiteral(" · auto in ~%1 s").arg(static_cast<int>(sp.seconds_until_auto_split));
        if (sp.split_failures > 0)
            split_text += QStringLiteral(" · failures %1").arg(sp.split_failures);
        setValue(QStringLiteral("liveSplit"), split_text);
    }
}

} // namespace exosnap::ui::widgets
