#include "NativeGamepadProducerChecks.h"

#include "GamepadArtwork.h"
#include "NativeGamepadProducer.h"
#include <exosnap/engine/gpu_compositor.h>
#include <exosnap/engine/visual_scene_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<uint8_t> ReadEvidence(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                                  UINT bytes_per_pixel) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(device->CreateTexture2D(&description, nullptr, staging.put()));
    context->CopyResource(staging.get(), texture);
    std::vector<uint8_t> pixels(static_cast<size_t>(description.Width) * description.Height * bytes_per_pixel);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT row = 0; row < description.Height; ++row)
        std::memcpy(pixels.data() + static_cast<size_t>(row) * description.Width * bytes_per_pixel,
                    static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                    static_cast<size_t>(description.Width) * bytes_per_pixel);
    context->Unmap(staging.get(), 0);
    return pixels;
}

float DecodeHalf(const uint8_t* bytes) {
    uint16_t half;
    std::memcpy(&half, bytes, sizeof(half));
    const unsigned exponent = (half >> 10) & 31;
    if (exponent == 31)
        return std::numeric_limits<float>::quiet_NaN();
    const float magnitude = exponent == 0
                                ? std::ldexp(static_cast<float>(half & 1023), -24)
                                : std::ldexp(static_cast<float>(1024 + (half & 1023)), static_cast<int>(exponent) - 25);
    return (half & 0x8000) ? -magnitude : magnitude;
}

void CheckHdrComposition(std::unique_ptr<exosnap::quick::NativeGamepadProducer>& owner, ID3D11Device* device,
                         ID3D11DeviceContext* context, bool ghost) {
    using namespace exosnap::engine;
    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgi.put())));
    winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
    winrt::com_ptr<ID3D11Device> consumer;
    winrt::com_ptr<ID3D11DeviceContext> consumer_context;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                           consumer.put(), nullptr, consumer_context.put()));
    VisualSceneRuntime runtime({});
    const auto token = owner->Stamp().token;
    Check(runtime.CreateSource(token.address).succeeded(), "HDR check could not create the native source.");
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto registration = runtime.RegisterConsumer({consumer_context.get(), DeviceGeneration{2}});
    Check(registration.succeeded(), "HDR check could not register the native consumer.");
    const auto consumer_id = *registration.value();
    GpuPublicationLease publication;
    // Destroy Qt before releasing its publication reservation on every exit.
    auto producer = std::move(owner);
    auto staged = StageGpuData(runtime, token, producer->Texture(), context, DeviceGeneration{1});
    Check(staged.succeeded(), "HDR check could not stage the native producer texture.");
    publication = std::move(*staged.value());
    PreparedSceneDescription description;
    description.revision = ConfigRevision{1};
    description.binding_sources.push_back(token.address);
    PreparedSceneItem item;
    item.id.bytes[0] = 1;
    item.source = token.address;
    item.properties.placement = {0, 0, 1, 1};
    description.items.push_back(item);
    Check(runtime.PublishScene(description).succeeded(), "HDR check could not publish the native scene.");
    SceneReadLease scene;
    Check(runtime.TryReadScene(consumer_id, scene) == ReadResult::Updated && scene.Bindings().size() == 1,
          "HDR check could not pin the native scene.");
    const auto settle = [&](auto attempt) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            if (attempt())
                return true;
            context->Flush();
            consumer_context->Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    Check(settle([&] { return publication.TryPublish(producer->Stamp()) == PublishResult::Accepted; }),
          "HDR check could not publish a native frame.");
    GpuReadLease frame;
    Check(
        settle([&] { return runtime.TryReadGpu(consumer_id, scene.Bindings().front(), frame) == ReadResult::Updated; }),
        "HDR check could not acquire the consumer-local native frame.");
    Check(frame.Frame().identity.token == token && frame.Frame().identity.resource_id == publication.ResourceId() &&
              frame.Frame().stamp.sequence == producer->Stamp().sequence,
          "HDR check consumed the wrong native frame identity.");
    D3D11_TEXTURE2D_DESC texture_description{};
    producer->Texture()->GetDesc(&texture_description);
    const UINT width = texture_description.Width, height = texture_description.Height;
    // These staging reads qualify pixels only and are outside the measured submission loop.
    const auto source = ReadEvidence(device, context, producer->Texture(), 4);
    winrt::com_ptr<ID3D11Resource> imported;
    frame.Frame().view->GetResource(imported.put());
    Check(source == ReadEvidence(consumer.get(), consumer_context.get(), imported.as<ID3D11Texture2D>().get(), 4),
          "The native consumer changed the producer's RGBA8 pixels.");
    size_t partial = 0, transparent = 0;
    for (size_t pixel = 0; pixel < source.size() / 4; ++pixel) {
        const auto alpha = source[pixel * 4 + 3];
        partial += alpha > 0 && alpha < 255;
        transparent += alpha == 0;
    }
    Check(partial > 100 && transparent > 100, "Native HDR evidence lacks partial-alpha or transparent pixels.");
    texture_description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture_description.BindFlags = D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> background;
    winrt::com_ptr<ID3D11RenderTargetView> background_view;
    winrt::check_hresult(consumer->CreateTexture2D(&texture_description, nullptr, background.put()));
    winrt::check_hresult(consumer->CreateRenderTargetView(background.get(), nullptr, background_view.put()));
    GpuCompositor compositor;
    std::string error;
    Check(compositor.Init(consumer.get(), consumer_context.get(), width, height, error, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          240.0f),
          "HDR check could not initialize the compositor.");
    float max_error = 0;
    unsigned draws = 0;
    for (float white : {160.0f, 240.0f}) {
        compositor.SetOverlayReferenceWhiteNits(white);
        for (float level : {0.0f, 0.75f}) {
            const float clear[]{level, level, level, 1.0f};
            consumer_context->ClearRenderTargetView(background_view.get(), clear);
            for (float opacity : {1.0f, 0.5f}) {
                for (bool mirror : {false, true}) {
                    Check(compositor.BeginFrame(background.get(), error) &&
                              compositor.DrawVisualGpu(frame.Frame().view,
                                                       {0, 0, static_cast<int>(width), static_cast<int>(height)},
                                                       mirror, error, opacity),
                          "HDR check could not composite the native frame.");
                    const auto output = ReadEvidence(consumer.get(), consumer_context.get(), compositor.Result(), 8);
                    ++draws;
                    for (UINT y = 0; y < height; ++y) {
                        for (UINT x = 0; x < width; ++x) {
                            const size_t input = (static_cast<size_t>(y) * width + (mirror ? width - 1 - x : x)) * 4;
                            const size_t target = (static_cast<size_t>(y) * width + x) * 8;
                            const float alpha = source[input + 3] / 255.0f;
                            for (size_t channel = 0; channel < 4; ++channel) {
                                const float straight = alpha > 0 ? source[input + channel] / (255.0f * alpha) : 0;
                                const float linear = straight <= 0.04045f
                                                         ? straight / 12.92f
                                                         : std::pow((straight + 0.055f) / 1.055f, 2.4f);
                                const float expected = channel == 3 ? 1.0f
                                                                    : linear * (white / 80.0f) * alpha * opacity +
                                                                          level * (1 - alpha * opacity);
                                const float actual = DecodeHalf(output.data() + target + channel * 2);
                                const float difference = std::abs(actual - expected);
                                max_error = std::max(max_error, difference);
                                Check(std::isfinite(actual) && difference <= 0.004f + std::abs(expected) * 0.003f,
                                      "Real native HDR pixels differ from independent linear-alpha composition.");
                            }
                        }
                    }
                }
            }
        }
    }
    qInfo("Native %s HDR composition: %u draws, %zu partial-alpha pixels, max error %.7f, %u evidence-only readbacks",
          ghost ? "Ghost" : "Normal", draws, partial, static_cast<double>(max_error), draws + 2);
    frame.Reset();
    scene.Reset();
    publication.Retire();
    producer.reset();
    publication.Reset();
    Check(runtime.PublishScene({ConfigRevision{2}, {}, {}}).succeeded(), "HDR check could not retire its scene.");
    runtime.CollectRetired();
    runtime.UnregisterConsumer(consumer_id);
}
} // namespace

namespace exosnap::quick {

bool CheckNativeGamepadProducer(ID3D11Device* device, ID3D11DeviceContext* context, QString& error) {
    const QString mapping = QStringLiteral(":/gamepad/controllers/dualsense");
    auto history = std::make_shared<gamepad::DeviceHistory>();
    gamepad::GamepadReading reading;
    reading.device.id[0] = 1;
    reading.device.layout = gamepad::LayoutKind::DualSense;
    reading.device.vendor_id = 0x054C;
    reading.device.product_id = 0x0CE6;
    reading.capture_100ns = 1000;
    history->Select(reading.device.id);
    history->Push(reading);
    engine::VisualSourceId source;
    source.bytes[0] = 1;
    const engine::SourceToken token{{source, engine::VisualRuntimeGeneration{1}}, engine::SourceRequestRevision{1}};
    for (bool ghost : {false, true}) {
        if (NativeGamepadProducer::Create(mapping, gamepad::HistoryReadView{history}, token, device, context,
                                          QSize(2049, 320), ghost, false, error)) {
            error = QStringLiteral("Native producer accepted an unbounded output size.");
            return false;
        }
        error.clear();
        auto producer = NativeGamepadProducer::Create(mapping, gamepad::HistoryReadView{history}, token, device,
                                                      context, QSize(480, 320), ghost, false, error);
        if (!producer)
            return false;
        if (producer->Diagnostics().render_cpu.samples != 0 || producer->Diagnostics().render_gpu_span.samples != 0) {
            error = QStringLiteral("A new native producer reported measurements before rendering.");
            return false;
        }
        if (producer->RenderSample(engine::CaptureTime100ns{1500}, error) != NativeGamepadSampleResult::Ready ||
            producer->Stamp().token != token || producer->Stamp().sequence != 1 ||
            producer->Stamp().captured_at != engine::CaptureTime100ns{1500} ||
            producer->InputTime() != engine::CaptureTime100ns{1000} || !producer->Texture()) {
            error = QStringLiteral("Native producer did not preserve its existing history sample identity.");
            return false;
        }
        if (producer->Diagnostics().render_cpu.samples != 1 || producer->Diagnostics().latest_sample_100ns != 1500 ||
            producer->Diagnostics().render_cpu.latest_ms < 0) {
            error = QStringLiteral("Native render diagnostics did not preserve the measured sample.");
            return false;
        }
        reading.capture_100ns = 2000;
        reading.device.layout = gamepad::LayoutKind::XboxSeries;
        history->Push(reading);
        if (producer->RenderSample(engine::CaptureTime100ns{2000}, error) !=
                NativeGamepadSampleResult::UnsupportedModel ||
            producer->Stamp().sequence != 1) {
            error = QStringLiteral("Native producer accepted an unsupported model or published its stale pose.");
            return false;
        }
        if (producer->Diagnostics().render_cpu.samples != 1) {
            error = QStringLiteral("An unsupported model was counted as a render attempt.");
            return false;
        }
        reading.capture_100ns = 3000;
        reading.device.layout = gamepad::LayoutKind::DualSense;
        reading.device.product_id = 0x0DF2;
        history->Push(reading);
        if (producer->RenderSample(engine::CaptureTime100ns{3000}, error) !=
                NativeGamepadSampleResult::UnsupportedModel ||
            producer->Stamp().sequence != 1) {
            error = QStringLiteral("Native producer accepted an unsupported DualSense Edge model.");
            return false;
        }
        reading.capture_100ns = 3500;
        reading.device.product_id = 0x0CE6;
        reading.connected = false;
        history->Push(reading);
        if (producer->RenderSample(engine::CaptureTime100ns{3500}, error) != NativeGamepadSampleResult::Ready ||
            producer->Stamp().sequence != 2 || producer->InputTime() != engine::CaptureTime100ns{3500}) {
            error = QStringLiteral("Native producer did not sample the existing disconnect history.");
            return false;
        }
        reading.capture_100ns = 4000;
        reading.connected = true;
        history->Push(reading);
        if (producer->RenderSample(engine::CaptureTime100ns{4000}, error) != NativeGamepadSampleResult::Ready ||
            producer->Stamp().sequence != 3 || producer->InputTime() != engine::CaptureTime100ns{4000}) {
            error = QStringLiteral("Native producer did not sample the existing reconnect history.");
            return false;
        }
        CheckHdrComposition(producer, device, context, ghost);
        history = std::make_shared<gamepad::DeviceHistory>();
        reading.capture_100ns = 1000;
        history->Select(reading.device.id);
        history->Push(reading);
    }
    return true;
}

} // namespace exosnap::quick
