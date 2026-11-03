#include "NativeGamepadProducerChecks.h"
#include "NativeRenderTarget.h"

#include "ControllerState.h"
#include "GamepadArtwork.h"
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QThread>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <exosnap/engine/gpu_timestamp_profiler.h>
#include <exosnap/engine/preview_shared_texture.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace exosnap::quick;
using namespace exosnap::gamepad;

namespace {
uint64_t probe_pixel_readbacks = 0;

void Require(bool condition, const QString& error) {
    if (!condition)
        throw std::runtime_error(error.toStdString());
}

GamepadReading Reading(bool active, uint64_t timestamp) {
    GamepadReading reading;
    reading.capture_100ns = timestamp;
    reading.state.controls_normalized = true;
    for (auto& control : reading.state.controls)
        control = {active ? ControlStatus::Active : ControlStatus::Released, active ? 1.0F : 0.0F, 0};
    if (active)
        reading.state.controls[static_cast<size_t>(Control::LeftStick)].y = 1;
    return reading;
}

// Evidence capture only. No staging resource or pixel Map exists in the measured render loop.
QImage Capture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(device->CreateTexture2D(&description, nullptr, staging.put()));
    context->CopyResource(staging.get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    ++probe_pixel_readbacks;
    QImage image(static_cast<int>(description.Width), static_cast<int>(description.Height),
                 QImage::Format_RGBA8888_Premultiplied);
    if (!image.isNull()) {
        for (UINT row = 0; row < description.Height; ++row)
            std::memcpy(image.scanLine(static_cast<int>(row)),
                        static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch, description.Width * 4);
    }
    context->Unmap(staging.get(), 0);
    Require(!image.isNull(), QStringLiteral("Could not allocate evidence image."));
    return image;
}

QJsonObject CheckAlpha(const QImage& image) {
    int empty = 0, opaque = 0, partial = 0, invalid = 0;
    for (int row = 0; row < image.height(); ++row) {
        const auto* pixels = image.constScanLine(row);
        for (int column = 0; column < image.width(); ++column) {
            const auto* pixel = pixels + column * 4;
            const int alpha = pixel[3];
            empty += alpha == 0;
            opaque += alpha == 255;
            partial += alpha > 0 && alpha < 255;
            invalid += std::max({pixel[0], pixel[1], pixel[2]}) > alpha + 2;
        }
    }
    Require(empty > image.width(), QStringLiteral("Native scene has no transparent background."));
    Require(opaque > 0 && partial > 0, QStringLiteral("Native scene lacks opaque or antialiased pixels."));
    Require(invalid == 0, QStringLiteral("Native scene violates the expected premultiplied alpha bounds."));
    return {{QStringLiteral("transparent_pixels"), empty},
            {QStringLiteral("opaque_pixels"), opaque},
            {QStringLiteral("partial_pixels"), partial},
            {QStringLiteral("premult_bounds_violations"), invalid}};
}

QJsonObject Distribution(std::vector<double> values) {
    if (values.empty())
        return {{QStringLiteral("samples"), 0}, {QStringLiteral("available"), false}};
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double p) { return values[static_cast<size_t>(p * (values.size() - 1))]; };
    return {{QStringLiteral("samples"), static_cast<int>(values.size())},
            {QStringLiteral("available"), true},
            {QStringLiteral("p50_ms"), percentile(.5)},
            {QStringLiteral("p95_ms"), percentile(.95)},
            {QStringLiteral("p99_ms"), percentile(.99)},
            {QStringLiteral("max_ms"), values.back()}};
}

void Save(const QImage& image, const QDir& directory, const QString& name) {
    Require(image.save(directory.filePath(name + QStringLiteral(".png"))), QStringLiteral("Could not save evidence."));
}

// Probe-owned endpoints use separate devices on one dispatch thread. This does
// not establish a product scheduling or cross-thread metadata contract.
class SharedFrame final {
  public:
    SharedFrame(ID3D11Device* producer, ID3D11Device* consumer, QSize size)
        : producer_device_(producer), consumer_device_(consumer) {
        std::string error;
        HANDLE handle = nullptr;
        const bool created =
            shared_.Create(producer, static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()),
                           DXGI_FORMAT_R8G8B8A8_UNORM, &handle, error);
        Require(created, QString::fromStdString(error));
        winrt::handle close_handle(handle);
        winrt::check_hresult(consumer->QueryInterface(IID_PPV_ARGS(consumer1_.put())));
        winrt::check_hresult(consumer1_->OpenSharedResource1(handle, IID_PPV_ARGS(opened_.put())));
        mutex_ = opened_.as<IDXGIKeyedMutex>();
        D3D11_TEXTURE2D_DESC description{};
        opened_->GetDesc(&description);
        description.MiscFlags = 0;
        winrt::check_hresult(consumer->CreateTexture2D(&description, nullptr, local_.put()));
        winrt::check_hresult(consumer->CreateShaderResourceView(local_.get(), nullptr, view_.put()));
    }

    bool Publish(ID3D11DeviceContext* context, ID3D11Texture2D* source) {
        CheckDevice(context, producer_device_);
        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        winrt::com_ptr<ID3D11Device> source_device;
        source->GetDevice(source_device.put());
        Require(source_device.get() == producer_device_ && description.Width == shared_.Width() &&
                    description.Height == shared_.Height() && description.Format == shared_.Format() &&
                    description.SampleDesc.Count == 1 && description.ArraySize == 1 && description.MipLevels == 1,
                QStringLiteral("Shared publication rejects incompatible source textures."));
        const auto result = shared_.TryPublish(context, source);
        Require(result.released_ok && (result.acquire == exosnap::engine::PreviewAcquireOutcome::Acquired ||
                                       result.acquire == exosnap::engine::PreviewAcquireOutcome::Contended),
                QStringLiteral("Shared publication failed or its mutex was abandoned; retire this transport."));
        winrt::check_hresult(producer_device_->GetDeviceRemovedReason());
        return result.published();
    }

    bool Consume(ID3D11DeviceContext* context) {
        CheckDevice(context, consumer_device_);
        const auto acquired =
            exosnap::engine::ClassifyPreviewAcquire(mutex_->AcquireSync(exosnap::engine::kPreviewSharedConsumerKey, 0));
        if (acquired == exosnap::engine::PreviewAcquireOutcome::Contended)
            return false;
        Require(acquired == exosnap::engine::PreviewAcquireOutcome::Acquired,
                QStringLiteral("Shared consumption failed or its mutex was abandoned; retire this transport."));
        context->CopyResource(local_.get(), opened_.get());
        winrt::check_hresult(mutex_->ReleaseSync(exosnap::engine::kPreviewSharedProducerKey));
        winrt::check_hresult(consumer_device_->GetDeviceRemovedReason());
        return true;
    }

    ID3D11Texture2D* Local() const {
        return local_.get();
    }

    IDXGIKeyedMutex* Mutex() const {
        return mutex_.get();
    }

  private:
    void CheckDevice(ID3D11DeviceContext* context, ID3D11Device* expected) const {
        winrt::com_ptr<ID3D11Device> actual;
        context->GetDevice(actual.put());
        Require(actual.get() == expected && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE,
                QStringLiteral("Shared-frame endpoint received the wrong device or context."));
        winrt::check_hresult(producer_device_->GetDeviceRemovedReason());
        winrt::check_hresult(consumer_device_->GetDeviceRemovedReason());
    }

    ID3D11Device* producer_device_;
    ID3D11Device* consumer_device_;
    exosnap::engine::PreviewSharedTexture shared_;
    winrt::com_ptr<ID3D11Device1> consumer1_;
    winrt::com_ptr<ID3D11Texture2D> opened_;
    winrt::com_ptr<IDXGIKeyedMutex> mutex_;
    winrt::com_ptr<ID3D11Texture2D> local_;
    winrt::com_ptr<ID3D11ShaderResourceView> view_;
};

// Bounded evidence settling only. Each transfer attempt remains zero-timeout;
// the measured loop never retries, sleeps or waits for a transfer.
template <typename Attempt> void Settle(Attempt attempt) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!attempt()) {
        Require(elapsed.elapsed() < 2000, QStringLiteral("Shared-frame evidence did not settle within two seconds."));
        QThread::msleep(1);
    }
}

QImage TransferEvidence(SharedFrame& shared, ID3D11Device* consumer, ID3D11DeviceContext* producer_context,
                        ID3D11DeviceContext* consumer_context, ID3D11Texture2D* source) {
    // Drain a previous measured publication before selecting the evidence frame.
    shared.Consume(consumer_context);
    producer_context->Flush();
    consumer_context->Flush();
    Settle([&] {
        shared.Consume(consumer_context);
        return shared.Publish(producer_context, source);
    });
    producer_context->Flush();
    Settle([&] { return shared.Consume(consumer_context); });
    return Capture(consumer, consumer_context, shared.Local());
}
} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    QCommandLineParser arguments;
    arguments.addHelpOption();
    arguments.addOption({QStringLiteral("warp"), QStringLiteral("Use WARP for correctness, not hardware timing.")});
    arguments.addOption({QStringLiteral("output"), QStringLiteral("Evidence directory."), QStringLiteral("directory")});
    arguments.process(app);
    try {
        const QString output = arguments.value(QStringLiteral("output"));
        Require(!output.isEmpty() && QDir().mkpath(output),
                QStringLiteral("An evidence output directory is required."));
        const QDir directory(output);
        const bool warp = arguments.isSet(QStringLiteral("warp"));
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL feature_level{};
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        winrt::check_hresult(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                               device.put(), &feature_level, context.put()));
        winrt::com_ptr<IDXGIAdapter> adapter;
        winrt::check_hresult(device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
        DXGI_ADAPTER_DESC1 adapter_description{};
        winrt::check_hresult(adapter.as<IDXGIAdapter1>()->GetDesc1(&adapter_description));
        const QJsonObject adapter_identity{
            {QStringLiteral("description"), QString::fromWCharArray(adapter_description.Description)},
            {QStringLiteral("luid"),
             QStringLiteral("%1:%2")
                 .arg(static_cast<quint32>(adapter_description.AdapterLuid.HighPart), 8, 16, QLatin1Char('0'))
                 .arg(adapter_description.AdapterLuid.LowPart, 8, 16, QLatin1Char('0'))},
            {QStringLiteral("vendor_id"), static_cast<qint64>(adapter_description.VendorId)},
            {QStringLiteral("device_id"), static_cast<qint64>(adapter_description.DeviceId)},
            {QStringLiteral("subsystem_id"), static_cast<qint64>(adapter_description.SubSysId)},
            {QStringLiteral("revision"), static_cast<qint64>(adapter_description.Revision)},
            {QStringLiteral("software"), (adapter_description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0}};
        Require(warp || (adapter_description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0,
                QStringLiteral("The hardware probe selected a software adapter."));
        winrt::com_ptr<ID3D11Device> consumer_device;
        winrt::com_ptr<ID3D11DeviceContext> consumer_context;
        winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                               consumer_device.put(), nullptr, consumer_context.put()));
        winrt::com_ptr<IDXGIAdapter> consumer_adapter;
        winrt::check_hresult(consumer_device.as<IDXGIDevice>()->GetAdapter(consumer_adapter.put()));
        DXGI_ADAPTER_DESC consumer_description{};
        winrt::check_hresult(consumer_adapter->GetDesc(&consumer_description));
        Require(consumer_device.get() != device.get() &&
                    consumer_description.AdapterLuid.HighPart == adapter_description.AdapterLuid.HighPart &&
                    consumer_description.AdapterLuid.LowPart == adapter_description.AdapterLuid.LowPart,
                QStringLiteral("The transport requires distinct devices on the same actual adapter."));
        QString error;
        const auto mapping =
            PrepareMappedGamepadArtwork(QStringLiteral(":/gamepad/controllers/dualsense/mapping.json"), error);
        Require(error.isEmpty(), error);
        Require(CheckNativeGamepadProducer(device.get(), context.get(), error), error);
        ControllerState state(mapping);
        QJsonArray scenarios;
        uint64_t measured_pixel_readbacks = 0;
        for (const bool ghost : {false, true}) {
            NativeRenderTarget target;
            Require(target.Initialize(device.get(), context.get(), state, QSize(480, 320), error), error);
            Require(target.Scene()->setProperty("ghost", ghost), QStringLiteral("Ghost property is unavailable."));
            winrt::com_ptr<ID3D11Device> texture_device;
            target.Texture()->GetDevice(texture_device.put());
            winrt::com_ptr<ID3D11Resource> sampled_resource;
            target.View()->GetResource(sampled_resource.put());
            winrt::com_ptr<IUnknown> target_identity;
            winrt::check_hresult(target.Texture()->QueryInterface(IID_PPV_ARGS(target_identity.put())));
            Require(texture_device.get() == device.get() &&
                        sampled_resource.as<IUnknown>().get() == target_identity.get(),
                    QStringLiteral("The render target and sampled resource are not the same native allocation."));
            sampled_resource = nullptr;
            target_identity = nullptr;
            texture_device = nullptr;
            state.ApplyReading(Reading(false, 1), 1);
            auto shared = std::make_unique<SharedFrame>(device.get(), consumer_device.get(), QSize(480, 320));
            Require(!shared->Consume(consumer_context.get()),
                    QStringLiteral("An unpublished shared slot was available to the consumer."));
            for (int frame = 0; frame < 8; ++frame) {
                Require(target.Render(error), error);
                shared->Publish(context.get(), target.Texture());
                shared->Consume(consumer_context.get());
            }
            exosnap::engine::GpuStageTimer gpu, publish_gpu, consume_gpu;
            gpu.Init(device.get());
            publish_gpu.Init(device.get());
            consume_gpu.Init(consumer_device.get());
            std::vector<double> cpu_times, gpu_times, publish_cpu_times, publish_gpu_times, consume_cpu_times,
                consume_gpu_times;
            cpu_times.reserve(120);
            gpu_times.reserve(120);
            publish_cpu_times.reserve(120);
            publish_gpu_times.reserve(120);
            consume_cpu_times.reserve(120);
            consume_gpu_times.reserve(120);
            int published_frames = 0, consumed_frames = 0, producer_misses = 0, consumer_misses = 0;
            const uint64_t readbacks_before = probe_pixel_readbacks;
            for (int frame = 0; frame < 120; ++frame) {
                state.ApplyReading(Reading((frame / 10) % 2 != 0, static_cast<uint64_t>(frame + 2)),
                                   static_cast<uint64_t>(frame + 2));
                QElapsedTimer cpu;
                gpu.Begin(context.get());
                cpu.start();
                const bool rendered = target.Render(error);
                cpu_times.push_back(static_cast<double>(cpu.nsecsElapsed()) / 1e6);
                gpu.End(context.get());
                Require(rendered, error);
                publish_gpu.Begin(context.get());
                cpu.restart();
                const bool published = shared->Publish(context.get(), target.Texture());
                publish_cpu_times.push_back(static_cast<double>(cpu.nsecsElapsed()) / 1e6);
                publish_gpu.End(context.get());
                published_frames += published;
                producer_misses += !published;
                consume_gpu.Begin(consumer_context.get());
                cpu.restart();
                const bool consumed = shared->Consume(consumer_context.get());
                consume_cpu_times.push_back(static_cast<double>(cpu.nsecsElapsed()) / 1e6);
                consume_gpu.End(consumer_context.get());
                consumed_frames += consumed;
                consumer_misses += !consumed;
                if (const auto duration = gpu.Poll(context.get()))
                    gpu_times.push_back(*duration);
                if (const auto duration = publish_gpu.Poll(context.get()))
                    publish_gpu_times.push_back(*duration);
                if (const auto duration = consume_gpu.Poll(consumer_context.get()))
                    consume_gpu_times.push_back(*duration);
            }
            const uint64_t readbacks_during_measurement = probe_pixel_readbacks - readbacks_before;
            measured_pixel_readbacks += readbacks_during_measurement;
            Require(readbacks_during_measurement == 0,
                    QStringLiteral("Probe-owned pixel readback occurred in the measured loop."));
            Require(published_frames > 0 && consumed_frames > 0,
                    QStringLiteral("Cross-device delivery made no progress without evidence readbacks."));
            const QString name = ghost ? QStringLiteral("ghost") : QStringLiteral("normal");
            state.ApplyReading(Reading(false, 200), 200);
            Require(target.Render(error), error);
            const QImage neutral = Capture(device.get(), context.get(), target.Texture());
            const QImage shared_neutral = TransferEvidence(*shared, consumer_device.get(), context.get(),
                                                           consumer_context.get(), target.Texture());
            Require(shared_neutral == neutral, QStringLiteral("Cross-device neutral pixels differ."));
            const QJsonObject alpha = CheckAlpha(neutral);
            Save(neutral, directory, name + QStringLiteral("-neutral"));
            Save(shared_neutral, directory, name + QStringLiteral("-shared-neutral"));
            state.ApplyReading(Reading(true, 201), 201);
            Require(target.Render(error), error);
            const QImage active = Capture(device.get(), context.get(), target.Texture());
            Require(active != neutral, QStringLiteral("Active controls did not change the native image."));
            Save(active, directory, name + QStringLiteral("-active"));
            Require(Capture(consumer_device.get(), consumer_context.get(), shared->Local()) == shared_neutral,
                    QStringLiteral("Producer rendering overwrote the retained consumer-local frame."));
            Settle([&] { return shared->Publish(context.get(), target.Texture()); });
            context->Flush();
            Require(!shared->Publish(context.get(), target.Texture()),
                    QStringLiteral("Producer overwrote an unconsumed shared slot."));
            Settle([&] {
                const auto result = exosnap::engine::ClassifyPreviewAcquire(
                    shared->Mutex()->AcquireSync(exosnap::engine::kPreviewSharedConsumerKey, 0));
                Require(result == exosnap::engine::PreviewAcquireOutcome::Acquired ||
                            result == exosnap::engine::PreviewAcquireOutcome::Contended,
                        QStringLiteral("Consumer-held contention setup failed."));
                return result == exosnap::engine::PreviewAcquireOutcome::Acquired;
            });
            const bool blocked_publish = shared->Publish(context.get(), target.Texture());
            const HRESULT held_release = shared->Mutex()->ReleaseSync(exosnap::engine::kPreviewSharedConsumerKey);
            winrt::check_hresult(held_release);
            Require(!blocked_publish, QStringLiteral("Producer did not drop while consumer held the keyed mutex."));
            Settle([&] { return shared->Consume(consumer_context.get()); });
            const QImage shared_active = Capture(consumer_device.get(), consumer_context.get(), shared->Local());
            Require(shared_active == active, QStringLiteral("Cross-device active pixels differ after contention."));
            Save(shared_active, directory, name + QStringLiteral("-shared-active"));

            QQmlComponent probe(qmlEngine(target.Scene()),
                                QUrl(QStringLiteral("qrc:/qt/qml/ExoSnap/Controller3DProbe/ControllerDepthProbe.qml")));
            auto* rig = target.Scene()->findChild<QObject*>(QStringLiteral("controllerRig"));
            Require(rig != nullptr, QStringLiteral("Controller rig was not found."));
            std::unique_ptr<QObject> marker(
                probe.createWithInitialProperties({{QStringLiteral("parent"), QVariant::fromValue(rig)}}));
            Require(marker != nullptr, probe.errorString());
            Require(target.Render(error), error);
            const QImage occluded = Capture(device.get(), context.get(), target.Texture());
            Require(occluded == active, QStringLiteral("A hidden depth marker changed the native image."));
            Save(occluded, directory, name + QStringLiteral("-depth"));
            const QImage shared_depth = TransferEvidence(*shared, consumer_device.get(), context.get(),
                                                         consumer_context.get(), target.Texture());
            Require(shared_depth == occluded, QStringLiteral("Cross-device depth pixels differ."));
            Save(shared_depth, directory, name + QStringLiteral("-shared-depth"));
            Require(marker->setProperty("position", QVariant::fromValue(QVector3D(0, 1, 0))),
                    QStringLiteral("Depth marker position is unavailable."));
            Require(target.Render(error), error);
            const QImage visible_marker = Capture(device.get(), context.get(), target.Texture());
            Require(visible_marker != active, QStringLiteral("The depth marker positive control was not visible."));
            marker.reset();
            Require(!target.Resize(QSize(0, 320), error), QStringLiteral("An empty texture size was accepted."));
            for (const auto size : {QSize(320, 214), QSize(640, 360), QSize(480, 320)}) {
                Require(target.Resize(size, error), error);
                Require(target.Render(error), error);
                bool stale_size_rejected = false;
                try {
                    shared->Publish(context.get(), target.Texture());
                } catch (const std::runtime_error&) {
                    stale_size_rejected = true;
                }
                Require(stale_size_rejected, QStringLiteral("A stale-size shared transport accepted a new target."));
                shared.reset();
                shared = std::make_unique<SharedFrame>(device.get(), consumer_device.get(), size);
                const QImage resized_shared = TransferEvidence(*shared, consumer_device.get(), context.get(),
                                                               consumer_context.get(), target.Texture());
                Require(resized_shared == Capture(device.get(), context.get(), target.Texture()),
                        QStringLiteral("Cross-device resize pixels differ."));
                D3D11_TEXTURE2D_DESC description{};
                target.Texture()->GetDesc(&description);
                Require(description.Width == static_cast<UINT>(size.width()) &&
                            description.Height == static_cast<UINT>(size.height()),
                        QStringLiteral("Native texture size did not follow resize."));
            }
            Require(target.Render(error), error);
            const QImage resized = Capture(device.get(), context.get(), target.Texture());
            Require(resized == active, QStringLiteral("Returning to the initial size changed the native pose."));
            scenarios.append(QJsonObject{{QStringLiteral("mode"), name},
                                         {QStringLiteral("alpha"), alpha},
                                         {QStringLiteral("render_submission_cpu"), Distribution(cpu_times)},
                                         {QStringLiteral("render_gpu"), Distribution(gpu_times)},
                                         {QStringLiteral("publish_submission_cpu"), Distribution(publish_cpu_times)},
                                         {QStringLiteral("publish_attempt_gpu"), Distribution(publish_gpu_times)},
                                         {QStringLiteral("consume_submission_cpu"), Distribution(consume_cpu_times)},
                                         {QStringLiteral("consume_attempt_gpu"), Distribution(consume_gpu_times)},
                                         {QStringLiteral("published_frames"), published_frames},
                                         {QStringLiteral("consumed_frames"), consumed_frames},
                                         {QStringLiteral("producer_misses"), producer_misses},
                                         {QStringLiteral("consumer_misses"), consumer_misses},
                                         {QStringLiteral("probe_pixel_readbacks_in_measured_loop"),
                                          static_cast<qint64>(readbacks_during_measurement)},
                                         {QStringLiteral("measured_frames"), 120}});
        }
        QJsonObject report{
            {QStringLiteral("driver"), warp ? QStringLiteral("warp") : QStringLiteral("hardware")},
            {QStringLiteral("adapter"), adapter_identity},
            {QStringLiteral("feature_level"), static_cast<int>(feature_level)},
            {QStringLiteral("same_device_target"), true},
            {QStringLiteral("distinct_consumer_device_same_adapter"), true},
            {QStringLiteral("consumer_adapter_description"), QString::fromWCharArray(consumer_description.Description)},
            {QStringLiteral("shared_slots"), 1},
            {QStringLiteral("consumer_local_slots"), 1},
            {QStringLiteral("gpu_copies_per_delivered_frame"), 2},
            {QStringLiteral("shared_and_consumer_local_bytes"), 2 * 480 * 320 * 4},
            {QStringLiteral("probe_pixel_readbacks_in_measured_loop"), static_cast<qint64>(measured_pixel_readbacks)},
            {QStringLiteral("probe_pixel_readbacks_total"), static_cast<qint64>(probe_pixel_readbacks)},
            {QStringLiteral("pixel_readback_counter_scope"),
             QStringLiteral("Successful pixel Maps in this probe's Capture helper; excludes Qt/driver internals.")},
            {QStringLiteral("cpu_timing_scope"), QStringLiteral("Render submission only; excludes ApplyReading.")},
            {QStringLiteral("gpu_timing_scope"),
             QStringLiteral("Queue timestamp spans include submission gaps; publish/consume attempts include misses.")},
            {QStringLiteral("scheduling_scope"),
             QStringLiteral("Two devices on one dispatch thread; no product cross-thread scheduling proof.")},
            {QStringLiteral("target_width"), 480},
            {QStringLiteral("target_height"), 320},
            {QStringLiteral("target_bytes"), 480 * 320 * 4},
            {QStringLiteral("scenarios"), scenarios},
            {QStringLiteral("scope"), QStringLiteral("Native producer correctness and unpaced submission; "
                                                     "not compositor, recording, HDR, or device-loss qualification.")}};
        QFile receipt(directory.filePath(QStringLiteral("native-render.json")));
        Require(receipt.open(QIODevice::WriteOnly), receipt.errorString());
        const auto json = QJsonDocument(report).toJson();
        Require(receipt.write(json) == json.size(), receipt.errorString());
        qInfo("Native controller target proof passed (%s); receipt: %s", warp ? "WARP correctness" : "hardware",
              qPrintable(receipt.fileName()));
        return 0;
    } catch (const winrt::hresult_error& error) {
        qCritical("Native controller target proof failed: 0x%08x", static_cast<unsigned>(error.code().value));
    } catch (const std::exception& error) {
        qCritical("Native controller target proof failed: %s", error.what());
    }
    return 1;
}
