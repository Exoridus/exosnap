#include "OverlayTelemetryAdapter.h"

#include <utility>

namespace exosnap::quick {
namespace {

QString unavailableValue(const QString& value) {
    return value == QString(QChar(0x2014)) ? QString() : value;
}

QString healthText(engine::PipelineHealth health, engine::PipelineBottleneck bottleneck) {
    if (health == engine::PipelineHealth::Good)
        return OverlayTelemetryAdapter::tr("OK");
    if (health != engine::PipelineHealth::Warning && health != engine::PipelineHealth::Critical)
        return {};
    switch (bottleneck) {
    case engine::PipelineBottleneck::Capture:
    case engine::PipelineBottleneck::Compositor:
        return OverlayTelemetryAdapter::tr("CAP!");
    case engine::PipelineBottleneck::VideoEncoder:
        return OverlayTelemetryAdapter::tr("ENC!");
    case engine::PipelineBottleneck::Muxer:
    case engine::PipelineBottleneck::Disk:
        return OverlayTelemetryAdapter::tr("DISK!");
    case engine::PipelineBottleneck::Audio:
        return OverlayTelemetryAdapter::tr("AUDIO!");
    case engine::PipelineBottleneck::Gpu:
        return OverlayTelemetryAdapter::tr("GPU!");
    case engine::PipelineBottleneck::None:
    case engine::PipelineBottleneck::Unknown:
        return OverlayTelemetryAdapter::tr("WARN!");
    }
    return {};
}

} // namespace

OverlayTelemetryAdapter::OverlayTelemetryAdapter(QObject* parent)
    : OverlayTelemetryAdapter([] { return Clock::now(); }, parent) {
}

OverlayTelemetryAdapter::OverlayTelemetryAdapter(Now now, QObject* parent) : QObject(parent), now_(std::move(now)) {
    deadline_.setSingleShot(true);
    deadline_.setTimerType(Qt::PreciseTimer);
    connect(&deadline_, &QTimer::timeout, this, &OverlayTelemetryAdapter::publishPending);
}

void OverlayTelemetryAdapter::submit(const OverlayTelemetryInputs& input) {
    const bool recording = input.recording_active;
    const bool diagnostics = input.diagnostics_active;
    const bool audio = diagnostics && input.diagnostics.muted_sources;
    const bool health = diagnostics && input.diagnostics.health;
    latest_ = {
        {QStringLiteral("overlayState"), static_cast<int>(input.state)},
        {QStringLiteral("elapsedText"), recording && input.recording.elapsed ? input.elapsed : QString()},
        {QStringLiteral("outputSizeText"),
         (recording && input.recording.output_size) || (diagnostics && input.diagnostics.size)
             ? unavailableValue(input.size)
             : QString()},
        {QStringLiteral("sourceNameText"), recording && input.recording.source_name ? input.source_name : QString()},
        {QStringLiteral("fpsText"), diagnostics && input.diagnostics.fps ? unavailableValue(input.fps) : QString()},
        {QStringLiteral("dropText"), diagnostics && input.diagnostics.drop ? unavailableValue(input.drops) : QString()},
        {QStringLiteral("driftText"),
         diagnostics && input.diagnostics.drift ? unavailableValue(input.drift) : QString()},
        {QStringLiteral("healthText"), health ? healthText(input.health, input.bottleneck) : QString()},
        {QStringLiteral("healthWarning"), health && (input.health == engine::PipelineHealth::Warning ||
                                                     input.health == engine::PipelineHealth::Critical)},
        {QStringLiteral("micMuted"), audio && input.mic_muted},
        {QStringLiteral("sysMuted"), audio && input.sys_muted},
        {QStringLiteral("micDegraded"), audio && input.mic_degraded},
        {QStringLiteral("sysDegraded"), audio && input.sys_degraded},
    };
    QVariantMap immediate{
        {QStringLiteral("state"), static_cast<int>(input.state)},
        {QStringLiteral("recording"), recording},
        {QStringLiteral("diagnostics"), diagnostics},
        {QStringLiteral("source"), input.source_identity},
        {QStringLiteral("recordingContent"), models::TokensForRecordingOverlayContent(input.recording)},
        {QStringLiteral("diagnosticsContent"), models::TokensForDiagnosticsOverlayContent(input.diagnostics)},
        {QStringLiteral("healthText"), latest_.value(QStringLiteral("healthText"))},
        {QStringLiteral("healthWarning"), latest_.value(QStringLiteral("healthWarning"))},
        {QStringLiteral("micMuted"), audio && input.mic_muted},
        {QStringLiteral("sysMuted"), audio && input.sys_muted},
        {QStringLiteral("micDegraded"), audio && input.mic_degraded},
        {QStringLiteral("sysDegraded"), audio && input.sys_degraded},
    };
    const bool bypass = immediate != immediate_;
    immediate_ = std::move(immediate);
    if (latest_ == published_) {
        deadline_.stop();
        return;
    }
    if (!last_publication_ || bypass || now_() - *last_publication_ >= kMinimumInterval) {
        publish();
        return;
    }
    schedulePending();
}

const QVariantMap& OverlayTelemetryAdapter::snapshot() const noexcept {
    return published_;
}

bool OverlayTelemetryAdapter::hasPendingPublication() const noexcept {
    return deadline_.isActive();
}

void OverlayTelemetryAdapter::publishPending() {
    if (latest_ == published_) {
        deadline_.stop();
        return;
    }
    if (last_publication_ && now_() - *last_publication_ < kMinimumInterval) {
        schedulePending();
        return;
    }
    publish();
}

void OverlayTelemetryAdapter::schedulePending() {
    const auto remaining = *last_publication_ + kMinimumInterval - now_();
    deadline_.start(std::chrono::ceil<std::chrono::milliseconds>(remaining));
}

void OverlayTelemetryAdapter::publish() {
    deadline_.stop();
    published_ = latest_;
    last_publication_ = now_();
    emit snapshotChanged();
}

} // namespace exosnap::quick
