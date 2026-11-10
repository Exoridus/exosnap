#include "GamepadOverlay.h"
#include "../Controller3D/NativeGamepadProducer.h"
#include "GamepadSprites.h"
#include "models/GamepadOverlaySettings.h"

#include <exosnap/engine/gpu_timestamp_profiler.h>
#include <exosnap/engine/overlay_sprites.h>
#include <gamepad/gamepad.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QScopeGuard>
#include <QSize>
#include <QStringList>
#include <QThread>
#include <QVariantList>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace exosnap::quick {

struct GamepadOverlay::NativeState {
    engine::GpuPublicationLease lease;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::unique_ptr<NativeGamepadProducer> producer;
    engine::GpuStageTimer publish_timer;
    engine::NativeGamepadDiagnostics diagnostics;
    QSize size;
    QString pack;
    GamepadView view = GamepadView::TwoDimensional;
    bool published = false;
};

namespace {
bool CreateNativeDevice(const LUID& luid, winrt::com_ptr<ID3D11Device>& device,
                        winrt::com_ptr<ID3D11DeviceContext>& context) {
    winrt::com_ptr<IDXGIFactory4> factory;
    winrt::com_ptr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
        return false;
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.put()))))
        return false;
    return SUCCEEDED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(),
                                       nullptr, context.put()));
}
} // namespace

GamepadOverlay::GamepadOverlay(QObject* parent) : QObject(parent) {
    raster_pool_.setMaxThreadCount(1);
    poll_.setInterval(16);
    connect(&poll_, &QTimer::timeout, this, &GamepadOverlay::Poll);
    raster_.setSingleShot(true);
    raster_.setInterval(150);
    connect(&raster_, &QTimer::timeout, this, &GamepadOverlay::Rebuild);
}

GamepadOverlay::~GamepadOverlay() {
    WaitForVisualControl();
}

void GamepadOverlay::WaitForVisualControl() {
    source_demand_ = false;
    native_enabled_ = false;
    RetireNative();
    RetireInput();
    if (retiring_input_) {
        retiring_input_->WaitForStop();
        retiring_input_.reset();
    }
    poll_.stop();
    raster_.stop();
    pending_atlas_key_.clear();
    visual_status_ = {};
    raster_pool_.clear();
    raster_pool_.waitForDone();
    raster_completed_serial_.store(raster_requested_serial_, std::memory_order_release);
}
GamepadOverlaySettings GamepadOverlay::Settings() const {
    const std::scoped_lock lock(mutex_);
    return settings_;
}

QVariantList GamepadOverlay::layouts() const {
    const QStringList labels{tr("PlayStation 5 DualSense"),
                             tr("Xbox Series"),
                             tr("Generic dual-stick"),
                             tr("PlayStation 4 DualShock 4"),
                             tr("Xbox One"),
                             tr("Switch Pro"),
                             tr("Joy-Cons in grip"),
                             tr("Separate Joy-Cons"),
                             tr("Joy-Con L"),
                             tr("Joy-Con R"),
                             tr("Joy-Con L (horizontal)"),
                             tr("Joy-Con R (horizontal)"),
                             tr("Steam Controller (2015)"),
                             tr("Steam Deck (full)"),
                             tr("Steam Deck (compact)"),
                             tr("Steam Controller (2026, generic artwork)"),
                             tr("Xbox 360"),
                             tr("GameCube"),
                             tr("PlayStation 5 DualSense Edge"),
                             tr("Xbox Elite")};
    QVariantList result{QVariantMap{{QStringLiteral("value"), -1},
                                    {QStringLiteral("label"), tr("Automatic layout")},
                                    {QStringLiteral("selectable"), true}}};
    for (qsizetype index = 0; index < labels.size(); ++index) {
        result.push_back(QVariantMap{{QStringLiteral("value"), index},
                                     {QStringLiteral("label"), labels[index]},
                                     {QStringLiteral("selectable"), true}});
    }
    return result;
}

QVariantList GamepadOverlay::viewsForDevice(const QString& device) const {
    const bool supported =
        device.isEmpty() ? Settings().device.empty() && selected_native_supported_ : native_devices_.contains(device);
    return {QVariantMap{{QStringLiteral("value"), QStringLiteral("2d")},
                        {QStringLiteral("label"), tr("2D")},
                        {QStringLiteral("selectable"), true}},
            QVariantMap{{QStringLiteral("value"), QStringLiteral("3d-normal")},
                        {QStringLiteral("label"), tr("3D Normal")},
                        {QStringLiteral("selectable"), supported}},
            QVariantMap{{QStringLiteral("value"), QStringLiteral("3d-ghost")},
                        {QStringLiteral("label"), tr("3D Ghost")},
                        {QStringLiteral("selectable"), supported}}};
}

QVariantList GamepadOverlay::views() const {
    return viewsForDevice(device());
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
    if (!visual_runtime_) {
        ConfigureBackend(std::move(settings));
        return;
    }
    settings = SanitizeGamepadOverlaySettings(std::move(settings));
    const auto previous = Settings();
    if (previous == settings)
        return;
    {
        const std::scoped_lock lock(mutex_);
        settings_ = settings;
        ++revision_;
    }
    if (native_enabled_ && (previous.width != settings.width || previous.height != settings.height)) {
        pending_atlas_key_.clear();
        raster_pool_.clear();
        raster_.start();
    }
    emit settingsChanged();
}

void GamepadOverlay::RetireInput() {
    {
        const std::scoped_lock lock(mutex_);
        if (!input_)
            return;
        Q_ASSERT(!retiring_input_);
        retiring_input_ = std::move(input_);
        input_history_ = {};
        cached_.reset();
        last_notified_.reset();
    }
    retiring_input_->RequestStop();
    published_sprites_ = nullptr;
    poll_.start();
}

void GamepadOverlay::FinishInputRetirement() {
    if (retiring_input_ && retiring_input_->Stopped()) {
        retiring_input_->WaitForStop();
        retiring_input_.reset();
        emit visualControlSettled();
    }
}

void GamepadOverlay::ConfigureBackend(GamepadOverlaySettings settings) {
    settings = SanitizeGamepadOverlaySettings(std::move(settings));
    const auto previous = Settings();
    const bool wanted = settings.enabled && (!visual_runtime_ || source_demand_);
    if (!visual_runtime_ && previous == settings && wanted == native_enabled_ &&
        (!wanted || input_ || !input_start_error_.isEmpty()))
        return;
    if (!wanted) {
        RetireInput();
        input_start_error_.clear();
    } else if (!input_ && !retiring_input_ && input_start_error_.isEmpty()) {
        std::string error;
        auto input = gamepad::SdlGamepadSource::Start(error);
        if (!input) {
            input_start_error_ = QString::fromStdString(error);
            SetStatus(tr("Controller input unavailable: %1").arg(input_start_error_));
        } else {
            const std::scoped_lock lock(mutex_);
            input_history_ = input->HistoryView();
            input_ = std::move(input);
        }
    }
    if (input_) {
        std::optional<gamepad::DeviceId> id;
        const std::string requested = session_device_.isEmpty() ? settings.device : session_device_.toStdString();
        if (!requested.empty()) {
            const QByteArray bytes = QByteArray::fromHex(QByteArray::fromStdString(requested));
            gamepad::DeviceId decoded{};
            std::copy_n(bytes.begin(), std::min(static_cast<size_t>(bytes.size()), decoded.size()), decoded.begin());
            id = decoded;
        }
        input_->SelectDevice(id);
    }
    {
        const std::scoped_lock lock(mutex_);
        settings_ = settings;
        ++revision_;
    }
    const bool was_running = native_enabled_;
    native_enabled_ = wanted;
    if (wanted) {
        poll_.start();
        if (!was_running || previous.layout != settings.layout || previous.width != settings.width ||
            previous.height != settings.height || !sprites_) {
            pending_atlas_key_.clear();
            raster_pool_.clear();
            raster_.start();
        }
        Poll();
    } else {
        RetireNative();
        if (!retiring_input_)
            poll_.stop();
        raster_.stop();
        pending_atlas_key_.clear();
        raster_pool_.clear();
        {
            const std::scoped_lock lock(mutex_);
            cached_.reset();
            last_notified_.reset();
            sprites_.reset();
        }
        atlas_key_.clear();
        SetStatus({});
        emit frameChanged();
    }
    if (previous != settings)
        emit settingsChanged();
}
GamepadControlHooks GamepadOverlay::CreateControlHooks(std::shared_ptr<engine::VisualSceneRuntime> runtime) {
    visual_runtime_ = std::move(runtime);
    ConfigureBackend(Settings());
    return {[this](const engine::SourceToken& token, GamepadOverlaySettings source,
                   std::function<void(SourceStatus)> status) {
                RetireNative();
                native_failed_ = false;
                visual_token_ = token;
                visual_status_ = std::move(status);
                visual_waiting_ = false;
                reported_phase_.reset();
                reported_error_.clear();
                input_start_error_.clear();
                if (input_ && input_->State() == gamepad::InputSourceState::Failed)
                    RetireInput();
                source_demand_ = true;
                auto desired = Settings();
                desired.device = std::move(source.device);
                desired.layout = source.layout;
                desired.view = source.view;
                ConfigureBackend(std::move(desired));
                PublishVisualData();
            },
            [this] {
                source_demand_ = false;
                ConfigureBackend(Settings());
                visual_status_ = {};
                published_sprites_ = nullptr;
                const auto serial = ++raster_requested_serial_;
                raster_pool_.start([this, serial] {
                    raster_completed_serial_.store(serial, std::memory_order_release);
                    QMetaObject::invokeMethod(this, [this] { emit visualControlSettled(); }, Qt::QueuedConnection);
                });
            },
            [this] {
                return !native_enabled_ && !input_ && !retiring_input_ &&
                       raster_completed_serial_.load(std::memory_order_acquire) == raster_requested_serial_;
            }};
}

void GamepadOverlay::ReportVisualStatus(SourcePhase phase, std::optional<engine::EngineError> error) {
    const auto context = error ? error->context : std::string{};
    if (!visual_status_ || (reported_phase_ == phase && reported_error_ == context))
        return;
    reported_phase_ = phase;
    reported_error_ = context;
    visual_status_({visual_token_, phase, std::move(error)});
}

void GamepadOverlay::PublishVisualData() {
    if (!visual_runtime_ || !source_demand_ || !visual_status_)
        return;
    if (!input_start_error_.isEmpty()) {
        engine::EngineError error;
        error.code = engine::EngineErrorCode::UnavailableSource;
        error.owner = engine::EngineModule::Runtime;
        error.context = input_start_error_.toStdString();
        error.retryability = engine::Retryability::AfterUserAction;
        ReportVisualStatus(SourcePhase::Failed, std::move(error));
        return;
    }
    if (!input_ || input_->State() == gamepad::InputSourceState::Starting) {
        ReportVisualStatus(SourcePhase::Preparing);
        return;
    }
    if (Settings().view != GamepadView::TwoDimensional)
        return;
    if (!sprites_ || !pending_atlas_key_.isEmpty() || raster_.isActive()) {
        ReportVisualStatus(SourcePhase::Waiting);
        return;
    }
    if (published_token_ != visual_token_ || published_sprites_ != sprites_.get()) {
        auto staged = engine::StageGamepadData(*visual_runtime_, visual_token_, sprites_, input_history_);
        if (!staged.succeeded()) {
            ReportVisualStatus(SourcePhase::Waiting, *staged.error());
            return;
        }
        published_token_ = visual_token_;
        published_sprites_ = sprites_.get();
    }
    const auto reading = ReadingAt(gamepad::NowCapture100ns());
    ReportVisualStatus(reading && reading->connected ? SourcePhase::Ready : SourcePhase::Waiting);
}
std::optional<engine::NativeGamepadDiagnostics>
GamepadOverlay::NativeDiagnostics(const engine::SourceToken& token) const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!native_ || !native_->producer || token != visual_token_)
        return std::nullopt;
    auto result = native_->diagnostics;
    const auto& rendered = native_->producer->Diagnostics();
    result.render_cpu = rendered.render_cpu;
    result.render_gpu_span = rendered.render_gpu_span;
    result.latest_sample_100ns = rendered.latest_sample_100ns;
    return result;
}

void GamepadOverlay::RetireNative() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!native_)
        return;
    native_->lease.Retire();
    published_token_ = {};
    // Consumers must see the bindingless scene before admission waits on their old scene pins.
    emit visualBindingInvalidated();
    native_->producer.reset();
    native_->lease.Reset();
    native_.reset();
    if (visual_runtime_)
        visual_runtime_->CollectRetired();
}

void GamepadOverlay::UpdateNative(const std::optional<gamepad::GamepadReading>& reading, uint64_t timestamp) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto settings = Settings();
    if (!visual_runtime_ || !source_demand_ || !visual_status_ || settings.view == GamepadView::TwoDimensional) {
        RetireNative();
        return;
    }
    if (!input_start_error_.isEmpty() || !input_ || input_->State() != gamepad::InputSourceState::Ready) {
        RetireNative();
        return;
    }
    const auto waiting = [this](QString message, std::optional<engine::EngineError> error = {}) {
        SetStatus(std::move(message));
        ReportVisualStatus(SourcePhase::Waiting, std::move(error));
    };
    const QString pack = reading && reading->connected ? NativeControllerPack(reading->device) : QString();
    if (pack.isEmpty()) {
        RetireNative();
        waiting(tr("3D view requires a connected controller with a 3D model"));
        return;
    }
    if (native_failed_) {
        waiting(tr("3D view unavailable. Retry or select 2D."));
        return;
    }
    const QSize size(qRound(std::clamp(static_cast<double>(content_width_) * settings.width, 96.0, 2048.0)),
                     qRound(std::clamp(static_cast<double>(content_height_) * settings.height, 64.0, 2048.0)));
    if (native_ && (native_->lease.Retired() || native_->size != size || native_->view != settings.view ||
                    native_->pack != pack)) {
        RetireNative();
        waiting(tr("Preparing 3D view"));
        return;
    }
    if (!native_) {
        visual_runtime_->CollectRetired();
        auto reservation = engine::ReserveGpuData(*visual_runtime_, visual_token_, size.width(), size.height());
        if (!reservation.succeeded()) {
            waiting(tr("Waiting for a compatible 3D output"));
            return;
        }
        native_ = std::make_unique<NativeState>();
        native_->lease = std::move(*reservation.value());
        native_->size = size;
        native_->view = settings.view;
        native_->pack = pack;
        native_->diagnostics.generation = ++native_generation_;
        LUID luid{};
        QString error;
        if (native_->lease.AdapterLuid(luid) && CreateNativeDevice(luid, native_->device, native_->context)) {
            native_->producer = NativeGamepadProducer::Create(pack, input_history_, visual_token_,
                                                              native_->device.get(), native_->context.get(), size,
                                                              settings.view == GamepadView::NativeGhost, false, error);
        }
        if (!native_->producer) {
            const bool retired = native_->lease.Retired();
            RetireNative();
            native_failed_ = !retired;
            waiting(tr("3D view unavailable. Retry or select 2D."),
                    engine::EngineError{.code = engine::EngineErrorCode::UnavailableSource,
                                        .context = error.isEmpty() ? "Native controller device unavailable"
                                                                   : error.toStdString()});
            return;
        }
        native_->publish_timer.Init(native_->device.get());
        auto staged = engine::StageGpuData(*visual_runtime_, native_->lease, native_->producer->Texture(),
                                           native_->context.get(), engine::NextDeviceGeneration());
        if (!staged.succeeded()) {
            const bool changed = staged.error()->code == engine::EngineErrorCode::StaleRevision;
            RetireNative();
            native_failed_ = !changed;
            waiting(tr("3D view unavailable. Retry or select 2D."), *staged.error());
            return;
        }
    }
    if (const auto span = native_->publish_timer.Poll(native_->context.get()))
        native_->diagnostics.publish_gpu_span.Add(*span);
    QString error;
    const auto sampled = native_->producer->RenderSample(engine::CaptureTime100ns{timestamp}, error);
    if (sampled == NativeGamepadSampleResult::Busy)
        return;
    if (sampled != NativeGamepadSampleResult::Ready) {
        RetireNative();
        native_failed_ = sampled == NativeGamepadSampleResult::Failed;
        waiting(tr("3D view unavailable. Retry or select 2D."),
                engine::EngineError{.code = engine::EngineErrorCode::UnavailableSource,
                                    .context =
                                        error.isEmpty() ? "Unsupported native controller model" : error.toStdString()});
        return;
    }
    QElapsedTimer cpu;
    cpu.start();
    native_->publish_timer.Begin(native_->context.get());
    const auto published = native_->lease.TryPublish(native_->producer->Stamp());
    native_->publish_timer.End(native_->context.get());
    native_->diagnostics.publish_cpu.Add(static_cast<double>(cpu.nsecsElapsed()) / 1e6);
    if (published == engine::PublishResult::Accepted)
        ++native_->diagnostics.accepted_publications;
    if (published == engine::PublishResult::NoFreeSlot)
        ++native_->diagnostics.no_free_slot_publications;
    if (published == engine::PublishResult::Revoked || published == engine::PublishResult::Invalid) {
        RetireNative();
        waiting(tr("Preparing 3D view"));
        return;
    }
    if (published == engine::PublishResult::Accepted && !native_->published) {
        native_->published = true;
        published_token_ = visual_token_;
        ReportVisualStatus(SourcePhase::Ready);
    }
    if (published == engine::PublishResult::Accepted)
        emit frameChanged();
    SetStatus(tr("Controller connected"));
}

void GamepadOverlay::setEnabled(bool value) {
    auto settings = Settings();
    settings.enabled = value;
    Configure(settings);
}
void GamepadOverlay::setOutputVisible(bool value) {
    auto settings = Settings();
    settings.output_visible = value;
    Configure(settings);
}
void GamepadOverlay::setLayout(int value) {
    auto settings = Settings();
    settings.layout = value;
    Configure(settings);
}
void GamepadOverlay::setOpacity(double value) {
    auto settings = Settings();
    settings.opacity = static_cast<float>(value);
    Configure(settings);
}
void GamepadOverlay::setDevice(const QString& value) {
    auto settings = Settings();
    settings.device = value.toStdString();
    Configure(settings);
}

void GamepadOverlay::setSessionDevice(const QString& value) {
    if (session_device_ == value)
        return;
    session_device_ = value;
    ConfigureBackend(Settings());
}

void GamepadOverlay::setPlayerIndex(int index) {
    const auto reading = ReadingAt(gamepad::NowCapture100ns());
    if (!input_ || !reading || !reading->connected || !gamepad::SelectablePlayerIndex(index)) {
        player_index_status_ = tr("Select a connected controller and a valid player P1-P4 or Unassigned");
        emit playerIndexChanged();
        return;
    }
    if (!input_->RequestPlayerIndex(reading->device.id, index)) {
        player_index_status_ = tr("Controller player-index change is unavailable");
        emit playerIndexChanged();
        return;
    }
    player_index_status_ = tr("Updating player index...");
    emit playerIndexChanged();
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
    if (native_enabled_) {
        pending_atlas_key_.clear();
        raster_pool_.clear();
        raster_.start();
    }
}

void GamepadOverlay::Rebuild() {
    const auto settings = Settings();
    if (!native_enabled_ || (visual_runtime_ && settings.view != GamepadView::TwoDimensional))
        return;
    const int layout = settings.layout < 0 ? automatic_layout_ : settings.layout;
    const QString file = QString::fromLatin1(kGamepadOverlayArtworkIds[layout].data());
    const int width = std::clamp(qRound(content_width_ * settings.width), 96, 2048);
    const QString key = QStringLiteral("%1/%2").arg(file).arg(width);
    if (atlas_key_ == key || pending_atlas_key_ == key)
        return;
    pending_atlas_key_ = key;
    raster_pool_.clear();
    const QString path = QStringLiteral(":/gamepad/controllers/") + file + QStringLiteral("/mapping.json");
    const auto token = visual_token_;
    const auto serial = ++raster_requested_serial_;
    raster_pool_.start([this, key, path, width, token, serial]() {
        const auto settled = qScopeGuard([this, serial] {
            raster_completed_serial_.store(serial, std::memory_order_release);
            QMetaObject::invokeMethod(this, [this] { emit visualControlSettled(); }, Qt::QueuedConnection);
        });
        QString error;
        auto sprites = RasterizeGamepad(path, width, false, false, error);
        QMetaObject::invokeMethod(
            this,
            [this, key, token, sprites = std::move(sprites), error = std::move(error)]() mutable {
                if (!native_enabled_ || pending_atlas_key_ != key)
                    return;
                pending_atlas_key_.clear();
                if (visual_runtime_ && token != visual_token_) {
                    if (native_enabled_)
                        raster_.start(0);
                    return;
                }
                if (!sprites) {
                    SetStatus(error);
                    return;
                }
                {
                    const std::scoped_lock lock(mutex_);
                    cached_.reset();
                    last_notified_.reset();
                    sprites_ = std::move(sprites);
                    ++revision_;
                }
                atlas_key_ = key;
                PublishVisualData();
                emit frameChanged();
            },
            Qt::QueuedConnection);
    });
}

int GamepadOverlay::DetectedLayout(const gamepad::DeviceIdentity& device) {
    if (device.vendor_id == 0x054C && device.product_id == 0x0DF2)
        return 18;
    if (device.vendor_id == 0x045E && (device.product_id == 0x02E3 || device.product_id == 0x0B00 ||
                                       device.product_id == 0x0B05 || device.product_id == 0x0B22))
        return 19;
    switch (device.layout) {
    case gamepad::LayoutKind::DualSense:
        return 0;
    case gamepad::LayoutKind::XboxSeries:
        return 1;
    case gamepad::LayoutKind::DualShock4:
        return 3;
    case gamepad::LayoutKind::XboxOne:
        return 4;
    case gamepad::LayoutKind::SwitchPro:
        return 5;
    case gamepad::LayoutKind::JoyConPair:
        return 7;
    case gamepad::LayoutKind::JoyConLeft:
        return 8;
    case gamepad::LayoutKind::JoyConRight:
        return 9;
    case gamepad::LayoutKind::Xbox360:
        return 16;
    case gamepad::LayoutKind::GameCube:
        return 17;
    default:
        return 2;
    }
}

void GamepadOverlay::Poll() {
    FinishInputRetirement();
    if (native_enabled_ && !input_ && !retiring_input_ && input_start_error_.isEmpty()) {
        ConfigureBackend(Settings());
        return;
    }
    if (input_ && input_->State() == gamepad::InputSourceState::Failed) {
        input_start_error_ = QString::fromStdString(input_->Error());
        RetireInput();
    }
    PublishVisualData();
    if (!native_enabled_ && !retiring_input_) {
        poll_.stop();
        return;
    }
    const auto timestamp = gamepad::NowCapture100ns();
    const auto reading = ReadingAt(timestamp);
    const bool flat = Settings().view == GamepadView::TwoDimensional;
    if (input_) {
        if (const auto result = input_->TakePlayerIndexResult()) {
            player_index_status_ = result->success ? QString() : QString::fromStdString(result->error);
            emit playerIndexChanged();
        }
    }
    const bool player_connected = reading && reading->connected;
    const int player_index = player_connected ? reading->device.player_index : -1;
    const bool player_led_supported = player_connected && reading->device.player_led_supported;
    if (player_connected_ != player_connected || player_index_ != player_index ||
        player_led_supported_ != player_led_supported) {
        player_connected_ = player_connected;
        player_index_ = player_index;
        player_led_supported_ = player_led_supported;
        emit playerIndexChanged();
    }
    int detected = automatic_layout_;
    if (reading && reading->connected) {
        detected = DetectedLayout(reading->device);
        if (flat)
            SetStatus(reading->device.persistent ? tr("Controller connected")
                                                 : tr("Controller connected (session-only identity)"));
    } else if (!input_start_error_.isEmpty()) {
        SetStatus(tr("Controller input unavailable: %1").arg(input_start_error_));
        if (!retiring_input_)
            poll_.stop();
    } else if (input_ && input_->State() == gamepad::InputSourceState::Starting) {
        SetStatus(tr("Preparing controller input"));
    } else if (input_ && flat) {
        SetStatus(device().isEmpty() && session_device_.isEmpty() ? tr("Waiting for controller")
                                                                  : tr("Controller disconnected"));
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
    QStringList native_devices;
    for (const auto& device : input_ ? input_->Devices() : std::vector<gamepad::DeviceIdentity>{}) {
        const QByteArray bytes(reinterpret_cast<const char*>(device.id.data()),
                               static_cast<qsizetype>(device.id.size()));
        const QString id = QString::fromLatin1(bytes.toHex());
        if (NativeGamepadProducer::Supports(device))
            native_devices.push_back(id);
        const QString family = device.layout == gamepad::LayoutKind::DualSense     ? tr("DualSense")
                               : device.layout == gamepad::LayoutKind::XboxSeries  ? tr("Xbox Series")
                               : device.layout == gamepad::LayoutKind::XboxOne     ? tr("Xbox One")
                               : device.layout == gamepad::LayoutKind::SwitchPro   ? tr("Switch Pro")
                               : device.layout == gamepad::LayoutKind::DualShock4  ? tr("DualShock 4")
                               : device.layout == gamepad::LayoutKind::Xbox360     ? tr("Xbox 360")
                               : device.layout == gamepad::LayoutKind::JoyConLeft  ? tr("Joy-Con L")
                               : device.layout == gamepad::LayoutKind::JoyConRight ? tr("Joy-Con R")
                               : device.layout == gamepad::LayoutKind::JoyConPair  ? tr("Joy-Con pair")
                               : device.layout == gamepad::LayoutKind::GameCube    ? tr("GameCube")
                                                                                   : tr("Gamepad");
        QString player = tr("Unassigned");
        if (device.player_index >= 0 && device.player_index < 4)
            player = QStringLiteral("P%1").arg(device.player_index + 1);
        else if (device.player_index >= 4)
            player = tr("SDL player %1").arg(device.player_index + 1);
        QString label = QStringLiteral("%1 (%2)").arg(family, id.right(6));
        if (!device.persistent)
            label = tr("%1 (%2, this session only)").arg(family, id.right(6));
        label += QStringLiteral(" · ") + player;
        options.push_back(QVariantMap{{QStringLiteral("value"), id},
                                      {QStringLiteral("label"), label},
                                      {QStringLiteral("sessionOnly"), !device.persistent}});
    }
    const auto selected = session_device_.isEmpty() ? device() : session_device_;
    if (!selected.isEmpty() && std::none_of(options.begin(), options.end(), [&](const QVariant& option) {
            return option.toMap().value(QStringLiteral("value")).toString() == selected;
        }))
        options.push_back(QVariantMap{{QStringLiteral("value"), selected},
                                      {QStringLiteral("label"), tr("Selected controller (disconnected)")},
                                      {QStringLiteral("sessionOnly"), !session_device_.isEmpty()}});
    for (auto& option : options) {
        auto entry = option.toMap();
        entry.insert(QStringLiteral("selectable"), true);
        option = entry;
    }
    if (devices_ != options) {
        devices_ = std::move(options);
        emit devicesChanged();
    }
    const bool supported = reading && reading->connected && NativeGamepadProducer::Supports(reading->device);
    if (selected_native_supported_ != supported || native_devices_ != native_devices) {
        selected_native_supported_ = supported;
        native_devices_ = std::move(native_devices);
        emit viewsChanged();
    }
    UpdateNative(reading, timestamp);
    if (visual_runtime_) {
        const auto state = reading && reading->connected ? DisplayState(reading->state) : gamepad::GamepadState{};
        if (state != cached_state_ || cached_revision_ != revision_) {
            cached_state_ = state;
            cached_revision_ = revision_;
            emit frameChanged();
        }
        return;
    }
    const auto next = PreviewSnapshot(gamepad::NowCapture100ns());
    const bool changed = engine::SpriteLayerChanged(last_notified_.get(), next.get());
    last_notified_ = next;
    if (changed)
        emit frameChanged();
}

std::shared_ptr<const engine::SpriteLayerSnapshot> GamepadOverlay::Snapshot(uint64_t timestamp) {
    return BuildSnapshot(timestamp, false);
}

std::shared_ptr<const engine::SpriteLayerSnapshot> GamepadOverlay::PreviewSnapshot(uint64_t timestamp) {
    return BuildSnapshot(timestamp, true);
}

std::optional<gamepad::GamepadReading> GamepadOverlay::ReadingAt(uint64_t timestamp) const {
    gamepad::HistoryReadView history;
    {
        const std::scoped_lock lock(mutex_);
        history = input_history_;
    }
    gamepad::GamepadReading reading;
    return history.TryStateAt(timestamp, reading) == gamepad::HistoryReadResult::Found
               ? std::optional<gamepad::GamepadReading>(std::move(reading))
               : std::nullopt;
}
gamepad::GamepadState GamepadOverlay::DisplayState(gamepad::GamepadState state) {
    return engine::GamepadDisplayState(std::move(state));
}

std::shared_ptr<const engine::SpriteLayerSnapshot> GamepadOverlay::BuildSnapshot(uint64_t timestamp, bool preview) {
    const std::scoped_lock lock(mutex_);
    if (!settings_.enabled || !sprites_ || (!preview && !settings_.output_visible))
        return nullptr;
    gamepad::GamepadReading sampled;
    const auto result = input_history_.TryStateAt(timestamp, sampled);
    if (result == gamepad::HistoryReadResult::Busy)
        return cached_;
    const auto reading = result == gamepad::HistoryReadResult::Found
                             ? std::optional<gamepad::GamepadReading>(std::move(sampled))
                             : std::nullopt;
    auto state = reading && reading->connected ? reading->state : gamepad::GamepadState{};
    state = DisplayState(std::move(state));
    if (cached_ && cached_revision_ == revision_ && state == cached_state_)
        return cached_;
    const float scale =
        std::min(settings_.width * content_width_ / 480.0F, settings_.height * content_height_ / 320.0F);
    const float width = 480 * scale / content_width_;
    const float height = 320 * scale / content_height_;
    const engine::SpriteContentRect placement{settings_.x + (settings_.width - width) / 2,
                                              settings_.y + (settings_.height - height) / 2, width, height};
    cached_ = GamepadLayer(*sprites_, state, placement, settings_.opacity * (settings_.output_visible ? 1.0F : 0.35F));
    cached_state_ = state;
    cached_revision_ = revision_;
    return cached_;
}

} // namespace exosnap::quick
