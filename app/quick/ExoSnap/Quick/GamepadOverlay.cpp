#include "GamepadOverlay.h"

#include <QByteArray>
#include <QMetaObject>

#include <cmath>

namespace exosnap::quick {

GamepadOverlay::GamepadOverlay(QObject* parent) : QObject(parent) {
    raster_pool_.setMaxThreadCount(1);
    poll_.setInterval(16);
    connect(&poll_, &QTimer::timeout, this, &GamepadOverlay::Poll);
    raster_.setSingleShot(true);
    raster_.setInterval(150);
    connect(&raster_, &QTimer::timeout, this, &GamepadOverlay::Rebuild);
}

GamepadOverlaySettings GamepadOverlay::Settings() const {
    const std::scoped_lock lock(mutex_);
    return settings_;
}

QRectF GamepadOverlay::placement() const {
    const auto settings = Settings();
    return {settings.x, settings.y, settings.width, settings.height};
}

void GamepadOverlay::SetStatus(QString text) {
    if (status_ == text)
        return;
    status_ = std::move(text);
    emit statusChanged();
}

void GamepadOverlay::Configure(GamepadOverlaySettings settings) {
    settings = SanitizeGamepadOverlaySettings(std::move(settings));
    const auto previous = Settings();
    if (previous == settings)
        return;
    std::shared_ptr<gamepad::GameInputSource> input;
    {
        const std::scoped_lock lock(mutex_);
        input = input_;
    }
    if (settings.enabled && !input) {
        std::string error;
        input = gamepad::GameInputSource::Start(error);
        if (!input)
            SetStatus(tr("GameInput unavailable: %1").arg(QString::fromStdString(error)));
    }
    if (!settings.enabled)
        input.reset();
    if (input) {
        std::optional<gamepad::DeviceId> id;
        if (!settings.device.empty()) {
            const QByteArray bytes = QByteArray::fromHex(QByteArray::fromStdString(settings.device));
            gamepad::DeviceId decoded{};
            std::copy(bytes.begin(), bytes.end(), decoded.begin());
            id = decoded;
        }
        input->SelectDevice(id);
    }
    {
        const std::scoped_lock lock(mutex_);
        settings_ = settings;
        input_.swap(input);
        ++revision_;
    }
    if (settings.enabled) {
        poll_.start();
        if (!previous.enabled || previous.layout != settings.layout || previous.glass != settings.glass ||
            previous.glow != settings.glow || previous.width != settings.width || previous.height != settings.height ||
            !sprites_) {
            pending_atlas_key_.clear();
            raster_pool_.clear();
            raster_.start();
        }
        Poll();
    } else {
        poll_.stop();
        raster_.stop();
        pending_atlas_key_.clear();
        raster_pool_.clear();
        last_notified_.reset();
        SetStatus({});
        emit frameChanged();
    }
    if (previous != settings)
        emit settingsChanged();
}

void GamepadOverlay::setEnabled(bool value) {
    auto settings = Settings();
    settings.enabled = value;
    Configure(settings);
}
void GamepadOverlay::setLayout(int value) {
    auto settings = Settings();
    settings.layout = value;
    Configure(settings);
}
void GamepadOverlay::setGlass(bool value) {
    auto settings = Settings();
    settings.glass = value;
    Configure(settings);
}
void GamepadOverlay::setGlow(bool value) {
    auto settings = Settings();
    settings.glow = value;
    Configure(settings);
}
void GamepadOverlay::setOpacity(double value) {
    auto settings = Settings();
    settings.opacity = static_cast<float>(value);
    Configure(settings);
}
void GamepadOverlay::setDevice(QString value) {
    auto settings = Settings();
    settings.device = value.toStdString();
    Configure(settings);
}

void GamepadOverlay::setPlacement(QRectF value) {
    auto settings = Settings();
    settings.x = static_cast<float>(value.x());
    settings.y = static_cast<float>(value.y());
    settings.width = static_cast<float>(value.width());
    settings.height = static_cast<float>(value.height());
    Configure(settings);
}

void GamepadOverlay::setContentSize(QSize size) {
    if (size.width() <= 0 || size.height() <= 0)
        return;
    {
        const std::scoped_lock lock(mutex_);
        if (content_width_ == size.width() && content_height_ == size.height())
            return;
        content_width_ = size.width();
        content_height_ = size.height();
        ++revision_;
    }
    if (enabled()) {
        pending_atlas_key_.clear();
        raster_pool_.clear();
        raster_.start();
    }
}

void GamepadOverlay::Rebuild() {
    const auto settings = Settings();
    if (!settings.enabled)
        return;
    const int layout = settings.layout < 0 ? automatic_layout_ : settings.layout;
    const QString file = layout == 0   ? QStringLiteral("dualsense")
                         : layout == 1 ? QStringLiteral("xbox_series")
                                       : QStringLiteral("generic");
    const int width = std::clamp(qRound(content_width_ * settings.width), 96, 2048);
    const QString key = QStringLiteral("%1/%2/%3/%4").arg(file).arg(width).arg(settings.glass).arg(settings.glow);
    if (atlas_key_ == key || pending_atlas_key_ == key)
        return;
    pending_atlas_key_ = key;
    raster_pool_.clear();
    const QString path = QStringLiteral(":/gamepad/") + file + QStringLiteral(".svg");
    raster_pool_.start([this, key, path, width, glass = settings.glass, glow = settings.glow]() {
        QString error;
        auto sprites = RasterizeGamepad(path, width, glass, glow, error);
        QMetaObject::invokeMethod(
            this,
            [this, key, sprites = std::move(sprites), error = std::move(error)]() mutable {
                if (pending_atlas_key_ != key)
                    return;
                pending_atlas_key_.clear();
                if (!sprites) {
                    SetStatus(error);
                    return;
                }
                {
                    const std::scoped_lock lock(mutex_);
                    sprites_ = std::move(sprites);
                    ++revision_;
                }
                atlas_key_ = key;
                emit frameChanged();
            },
            Qt::QueuedConnection);
    });
}

void GamepadOverlay::Poll() {
    std::shared_ptr<gamepad::GameInputSource> input;
    {
        const std::scoped_lock lock(mutex_);
        input = input_;
    }
    const auto reading = input ? input->StateAt(gamepad::GameInputSource::NowCapture100ns()) : std::nullopt;
    int detected = automatic_layout_;
    if (reading && reading->connected) {
        detected = reading->device.layout == gamepad::LayoutKind::DualSense ? 0
                   : reading->device.layout == gamepad::LayoutKind::XboxSeries ||
                           reading->device.layout == gamepad::LayoutKind::XboxOne
                       ? 1
                       : 2;
        SetStatus(tr("Controller connected"));
    } else if (input) {
        SetStatus(tr("Waiting for controller"));
    }
    if (detected != automatic_layout_) {
        automatic_layout_ = detected;
        if (layout() < 0) {
            pending_atlas_key_.clear();
            raster_pool_.clear();
            raster_.start(0);
        }
    }
    QVariantList options{
        {QVariantMap{{QStringLiteral("value"), QString()}, {QStringLiteral("label"), tr("Automatic")}}}};
    for (const auto& device : input ? input->Devices() : std::vector<gamepad::DeviceIdentity>{}) {
        const QByteArray bytes(reinterpret_cast<const char*>(device.id.data()),
                               static_cast<qsizetype>(device.id.size()));
        const QString id = QString::fromLatin1(bytes.toHex());
        const QString family = device.layout == gamepad::LayoutKind::DualSense    ? tr("DualSense")
                               : device.layout == gamepad::LayoutKind::XboxSeries ? tr("Xbox Series")
                               : device.layout == gamepad::LayoutKind::XboxOne    ? tr("Xbox One")
                               : device.layout == gamepad::LayoutKind::SwitchPro  ? tr("Switch Pro")
                                                                                  : tr("Gamepad");
        const QString label = QStringLiteral("%1 (%2)").arg(family, id.right(6));
        options.push_back(QVariantMap{{QStringLiteral("value"), id}, {QStringLiteral("label"), label}});
    }
    const auto selected = device();
    if (!selected.isEmpty() && std::none_of(options.begin(), options.end(), [&](const QVariant& option) {
            return option.toMap().value(QStringLiteral("value")).toString() == selected;
        }))
        options.push_back(QVariantMap{{QStringLiteral("value"), selected},
                                      {QStringLiteral("label"), tr("Selected controller (disconnected)")}});
    if (devices_ != options) {
        devices_ = std::move(options);
        emit devicesChanged();
    }
    const auto next = Snapshot(gamepad::GameInputSource::NowCapture100ns());
    const bool changed = engine::SpriteLayerChanged(last_notified_.get(), next.get());
    last_notified_ = next;
    if (changed)
        emit frameChanged();
}

std::shared_ptr<const engine::SpriteLayerSnapshot> GamepadOverlay::Snapshot(uint64_t timestamp) {
    const std::scoped_lock lock(mutex_);
    if (!settings_.enabled || !sprites_)
        return nullptr;
    const auto reading = input_ ? input_->StateAt(timestamp) : std::nullopt;
    auto state = reading && reading->connected ? reading->state : gamepad::GamepadState{};
    state.buttons &= 0x3FFF;
    if (std::hypot(state.left_x, state.left_y) <= 0.08F) {
        state.left_x = 0;
        state.left_y = 0;
    }
    if (std::hypot(state.right_x, state.right_y) <= 0.08F) {
        state.right_x = 0;
        state.right_y = 0;
    }
    if (cached_ && cached_revision_ == revision_ && state == cached_state_)
        return cached_;
    const float scale =
        std::min(settings_.width * content_width_ / 480.0F, settings_.height * content_height_ / 320.0F);
    const float width = 480 * scale / content_width_;
    const float height = 320 * scale / content_height_;
    const engine::SpriteContentRect placement{settings_.x + (settings_.width - width) / 2,
                                              settings_.y + (settings_.height - height) / 2, width, height};
    cached_ = GamepadLayer(*sprites_, state, placement, settings_.opacity);
    cached_state_ = state;
    cached_revision_ = revision_;
    return cached_;
}

} // namespace exosnap::quick
