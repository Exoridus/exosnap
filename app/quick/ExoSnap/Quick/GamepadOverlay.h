#pragma once

#include "GamepadSprites.h"
#include "extensions/GamepadModule.h"
#include "models/GamepadOverlaySettings.h"
#include <exosnap/engine/error_types.h>
#include <exosnap/engine/visual_diagnostics.h>

#include <atomic>
#include <optional>
#include <string>

#include <QObject>
#include <QRectF>
#include <QSize>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

class GamepadOverlay final : public QObject, public engine::SpriteSourceProvider {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("GamepadOverlay is provided by the application")
    Q_PROPERTY(bool enabled READ enabled NOTIFY settingsChanged FINAL)
    Q_PROPERTY(bool outputVisible READ outputVisible NOTIFY settingsChanged FINAL)
    Q_PROPERTY(int layout READ layout NOTIFY settingsChanged FINAL)
    Q_PROPERTY(double opacity READ opacity NOTIFY settingsChanged FINAL)
    Q_PROPERTY(QRectF placement READ placement NOTIFY settingsChanged FINAL)
    Q_PROPERTY(QString device READ device NOTIFY settingsChanged FINAL)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged FINAL)
    Q_PROPERTY(int playerIndex READ playerIndex NOTIFY playerIndexChanged FINAL)
    Q_PROPERTY(bool playerConnected READ playerConnected NOTIFY playerIndexChanged FINAL)
    Q_PROPERTY(bool playerLedSupported READ playerLedSupported NOTIFY playerIndexChanged FINAL)
    Q_PROPERTY(QString playerIndexStatus READ playerIndexStatus NOTIFY playerIndexChanged FINAL)
    Q_PROPERTY(QVariantList layouts READ layouts CONSTANT FINAL)
    Q_PROPERTY(QVariantList views READ views NOTIFY viewsChanged FINAL)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged FINAL)

  public:
    explicit GamepadOverlay(QObject* parent = nullptr);
    ~GamepadOverlay() override;
    void WaitForVisualControl();
    void Configure(GamepadOverlaySettings settings);
    GamepadControlHooks CreateControlHooks(std::shared_ptr<engine::VisualSceneRuntime>);
    [[nodiscard]] GamepadOverlaySettings Settings() const;
    // GUI-thread snapshot of the matching active native generation.
    [[nodiscard]] std::optional<engine::NativeGamepadDiagnostics> NativeDiagnostics(const engine::SourceToken&) const;
    [[nodiscard]] std::shared_ptr<const engine::SpriteLayerSnapshot> Snapshot(uint64_t frame_time_qpc_100ns) override;
    [[nodiscard]] std::shared_ptr<const engine::SpriteLayerSnapshot> PreviewSnapshot(uint64_t frame_time_qpc_100ns);

    /// Selected raw reading at or before the frame time, without the 2D display deadzone.
    [[nodiscard]] std::optional<gamepad::GamepadReading> ReadingAt(uint64_t frame_time_qpc_100ns) const;

    bool enabled() const {
        return Settings().enabled;
    }
    bool outputVisible() const {
        return Settings().output_visible;
    }
    int layout() const {
        return Settings().layout;
    }
    double opacity() const {
        return Settings().opacity;
    }
    QRectF placement() const;
    QString device() const {
        return QString::fromStdString(Settings().device);
    }
    QVariantList devices() const {
        return devices_;
    }
    QVariantList layouts() const;
    QVariantList views() const;
    QVariantList viewsForDevice(const QString&) const;
    int playerIndex() const noexcept {
        return player_index_;
    }
    bool playerConnected() const noexcept {
        return player_connected_;
    }
    bool playerLedSupported() const noexcept {
        return player_led_supported_;
    }
    const QString& playerIndexStatus() const noexcept {
        return player_index_status_;
    }
    const QString& status() const {
        return status_;
    }

    void setEnabled(bool value);
    void setOutputVisible(bool value);
    void setLayout(int value);
    void setOpacity(double value);
    void setPlacement(QRectF value);
    void setDevice(const QString& value);
    // Selects a device for this run only, overriding the saved device. Empty ends it.
    void setSessionDevice(const QString& value);
    // Invoked by the future per-overlay context menu; no automatic hardware writes.
    Q_INVOKABLE void setPlayerIndex(int index);
    Q_INVOKABLE void setContentSize(QSize size);

  signals:
    void settingsChanged();
    void devicesChanged();
    void playerIndexChanged();
    void viewsChanged();
    void statusChanged();
    void frameChanged();
    void visualControlSettled();
    void visualBindingInvalidated();

  private:
    friend class GamepadOverlayTest;
    static gamepad::GamepadState DisplayState(gamepad::GamepadState state);
    static int DetectedLayout(const gamepad::DeviceIdentity& device);
    void ConfigureBackend(GamepadOverlaySettings);
    void Poll();
    void Rebuild();
    void PublishVisualData();
    void RetireInput();
    void FinishInputRetirement();
    void UpdateNative(const std::optional<gamepad::GamepadReading>&, uint64_t timestamp);
    void RetireNative();
    void ReportVisualStatus(SourcePhase, std::optional<engine::EngineError> error = std::nullopt);
    void SetStatus(QString text);
    std::shared_ptr<const engine::SpriteLayerSnapshot> BuildSnapshot(uint64_t timestamp, bool preview);
    mutable std::mutex mutex_;
    GamepadOverlaySettings settings_;
    int content_width_ = 1920;
    int content_height_ = 1080;
    std::unique_ptr<gamepad::SdlGamepadSource> input_;
    std::unique_ptr<gamepad::SdlGamepadSource> retiring_input_;
    gamepad::HistoryReadView input_history_;
    QString input_start_error_;
    std::optional<SourcePhase> reported_phase_;
    std::string reported_error_;
    std::shared_ptr<const GamepadSpriteSet> sprites_;
    std::shared_ptr<const engine::SpriteLayerSnapshot> cached_;
    std::shared_ptr<const engine::SpriteLayerSnapshot> last_notified_;
    gamepad::GamepadState cached_state_;
    uint64_t revision_ = 0;
    uint64_t cached_revision_ = 0;
    int automatic_layout_ = 2;
    QString atlas_key_;
    QString pending_atlas_key_;
    QVariantList devices_;
    QString session_device_;
    int player_index_ = -1;
    bool player_connected_ = false;
    bool player_led_supported_ = false;
    QString player_index_status_;
    QString status_;
    QTimer poll_;
    QTimer raster_;
    // Join workers before their data and QObject delivery context are destroyed.
    QThreadPool raster_pool_;
    uint64_t raster_requested_serial_ = 0;
    std::atomic<uint64_t> raster_completed_serial_{0};
    std::shared_ptr<engine::VisualSceneRuntime> visual_runtime_;
    engine::SourceToken visual_token_;
    engine::SourceToken published_token_;
    const GamepadSpriteSet* published_sprites_ = nullptr;
    std::function<void(SourceStatus)> visual_status_;
    bool visual_waiting_ = false;
    bool source_demand_ = false;
    bool native_enabled_ = false;
    struct NativeState;
    std::unique_ptr<NativeState> native_;
    uint64_t native_generation_ = 0;
    bool native_failed_ = false;
    bool selected_native_supported_ = false;
    QStringList native_devices_;
};

} // namespace exosnap::quick
