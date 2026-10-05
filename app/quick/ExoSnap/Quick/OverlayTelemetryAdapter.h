#pragma once

#include "models/OverlayContentPolicy.h"

#include <exosnap/engine/pipeline_diagnostics.h>

#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <chrono>
#include <functional>
#include <optional>

namespace exosnap::quick {

struct OverlayTelemetryInputs {
    models::RecordingOverlayContent recording;
    models::DiagnosticsOverlayContent diagnostics;
    models::RecordingOverlayState state = models::RecordingOverlayState::Hidden;
    bool recording_active = false;
    bool diagnostics_active = false;
    QString source_identity;
    QString elapsed;
    QString size;
    QString source_name;
    QString fps;
    QString drops;
    QString drift;
    bool mic_muted = false;
    bool sys_muted = false;
    bool mic_degraded = false;
    bool sys_degraded = false;
    engine::PipelineHealth health = engine::PipelineHealth::Unavailable;
    engine::PipelineBottleneck bottleneck = engine::PipelineBottleneck::None;
};

// Projects authoritative formatted telemetry into the visible HUD snapshot.
// Ordinary changes share a pending-only deadline; state and actionable feedback
// bypass it. Collection and native presentation scheduling remain independent.
class OverlayTelemetryAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("OverlayTelemetryAdapter is provided by the application")
    Q_PROPERTY(QVariantMap snapshot READ snapshot NOTIFY snapshotChanged FINAL)

  public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;
    static constexpr auto kMinimumInterval = std::chrono::milliseconds(200);

    explicit OverlayTelemetryAdapter(QObject* parent = nullptr);
    explicit OverlayTelemetryAdapter(Now now, QObject* parent = nullptr);

    void submit(const OverlayTelemetryInputs& inputs);
    [[nodiscard]] const QVariantMap& snapshot() const noexcept;
    [[nodiscard]] bool hasPendingPublication() const noexcept;
    // Evaluates the monotonic deadline. Also usable with a controlled test clock.
    void publishPending();

  signals:
    void snapshotChanged();

  private:
    void publish();
    void schedulePending();
    Now now_;
    QTimer deadline_;
    QVariantMap published_;
    QVariantMap latest_;
    QVariantMap immediate_;
    std::optional<Clock::time_point> last_publication_;
};

} // namespace exosnap::quick
