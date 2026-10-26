#pragma once

#include "GamepadSprites.h"
#include "models/GamepadOverlaySettings.h"

#include <QObject>
#include <QRectF>
#include <QSize>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

class GamepadOverlay final : public QObject, public engine::SpriteSourceProvider {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("GamepadOverlay is provided by the application")
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged FINAL)
    Q_PROPERTY(int layout READ layout WRITE setLayout NOTIFY settingsChanged FINAL)
    Q_PROPERTY(bool glass READ glass WRITE setGlass NOTIFY settingsChanged FINAL)
    Q_PROPERTY(bool glow READ glow WRITE setGlow NOTIFY settingsChanged FINAL)
    Q_PROPERTY(double opacity READ opacity WRITE setOpacity NOTIFY settingsChanged FINAL)
    Q_PROPERTY(QRectF placement READ placement WRITE setPlacement NOTIFY settingsChanged FINAL)
    Q_PROPERTY(QString device READ device WRITE setDevice NOTIFY settingsChanged FINAL)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged FINAL)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged FINAL)

  public:
    explicit GamepadOverlay(QObject* parent = nullptr);
    void Configure(GamepadOverlaySettings settings);
    [[nodiscard]] GamepadOverlaySettings Settings() const;
    [[nodiscard]] std::shared_ptr<const engine::SpriteLayerSnapshot> Snapshot(uint64_t frame_time_qpc_100ns) override;

    bool enabled() const {
        return Settings().enabled;
    }
    int layout() const {
        return Settings().layout;
    }
    bool glass() const {
        return Settings().glass;
    }
    bool glow() const {
        return Settings().glow;
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
    QString status() const {
        return status_;
    }

    void setEnabled(bool value);
    void setLayout(int value);
    void setGlass(bool value);
    void setGlow(bool value);
    void setOpacity(double value);
    void setPlacement(QRectF value);
    void setDevice(QString value);
    Q_INVOKABLE void setContentSize(QSize size);

  signals:
    void settingsChanged();
    void devicesChanged();
    void statusChanged();
    void frameChanged();

  private:
    friend class GamepadOverlayTest;
    void Poll();
    void Rebuild();
    void SetStatus(QString text);
    mutable std::mutex mutex_;
    GamepadOverlaySettings settings_;
    int content_width_ = 1920;
    int content_height_ = 1080;
    std::shared_ptr<gamepad::GameInputSource> input_;
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
    QString status_;
    QTimer poll_;
    QTimer raster_;
    // Join workers before their data and QObject delivery context are destroyed.
    QThreadPool raster_pool_;
};

} // namespace exosnap::quick
