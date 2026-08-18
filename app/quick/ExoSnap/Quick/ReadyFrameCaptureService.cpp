#include "ReadyFrameCaptureService.h"

#include "../../../../libs/recorder_core/src/gpu_compositor.h"

#include <recorder_core/cursor_sprite.h>
#include <recorder_core/gpu_hdr_tonemap.h>
#include <recorder_core/webcam_placement.h>

#include <QMetaObject>
#include <QThreadPool>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>

namespace exosnap::quick {
namespace {

using Microsoft::WRL::ComPtr;

constexpr char kCropVertexShader[] = R"(
struct VertexOutput {
    float4 position : SV_POSITION;
};
VertexOutput main(uint id : SV_VertexID) {
    VertexOutput output;
    const float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
)";

constexpr char kCropPixelShader[] = R"(
Texture2D<float4> sourceTexture : register(t0);
cbuffer CropConstants : register(b0) {
    uint2 sourceOffset;
    uint2 padding;
};
float4 main(float4 position : SV_POSITION) : SV_TARGET {
    const uint2 pixel = uint2(position.xy) + sourceOffset;
    const float4 color = sourceTexture.Load(int3(pixel, 0));
    return float4(color.rgb, 1.0f);
}
)";

// Scoped owner of one shared-texture HANDLE. Non-copyable and non-movable: a
// copy would leave two owners closing the same handle, and Windows recycles
// handle values, so the second CloseHandle can close an UNRELATED object rather
// than merely failing. The surrounding code passes handles around as `void*`,
// where an accidental by-value capture is one keystroke away — the same reason
// CaptureSourceHub, AudioDeviceNotifier and LiveVerifyControlServer delete
// theirs. Nothing here needs to move one, so no move operations are defined
// (the destructor already suppresses the implicit ones).
struct OwnedHandle {
    explicit OwnedHandle(void* value) : value(static_cast<HANDLE>(value)) {
    }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    ~OwnedHandle() {
        if (value != nullptr)
            CloseHandle(value);
    }
    HANDLE value = nullptr;
};

static_assert(!std::is_copy_constructible_v<OwnedHandle>, "an owned HANDLE must never be copied");
static_assert(!std::is_copy_assignable_v<OwnedHandle>, "an owned HANDLE must never be copied");
static_assert(!std::is_move_constructible_v<OwnedHandle>,
              "no consumer moves one; add a move that clears the source first");

struct CropConstants {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t padding[2]{};
};

void setError(std::string& error, const char* operation, HRESULT result) {
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "%s failed 0x%08lX", operation, static_cast<unsigned long>(result));
    error = buffer;
}

bool createDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context, std::string& error) {
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.GetAddressOf(), &level, context.GetAddressOf());
    if (FAILED(result)) {
        setError(error, "D3D11CreateDevice(Ready frame)", result);
        return false;
    }
    return true;
}

bool cropToBgra(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, const QRect& crop,
                ComPtr<ID3D11Texture2D>& output, std::string& error) {
    D3D11_TEXTURE2D_DESC output_desc{};
    output_desc.Width = static_cast<UINT>(crop.width());
    output_desc.Height = static_cast<UINT>(crop.height());
    output_desc.MipLevels = 1;
    output_desc.ArraySize = 1;
    output_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    output_desc.SampleDesc.Count = 1;
    output_desc.Usage = D3D11_USAGE_DEFAULT;
    output_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT result = device->CreateTexture2D(&output_desc, nullptr, output.GetAddressOf());
    if (FAILED(result)) {
        setError(error, "CreateTexture2D(Ready frame output)", result);
        return false;
    }

    ComPtr<ID3D11ShaderResourceView> source_view;
    ComPtr<ID3D11RenderTargetView> output_view;
    result = device->CreateShaderResourceView(source, nullptr, source_view.GetAddressOf());
    if (SUCCEEDED(result))
        result = device->CreateRenderTargetView(output.Get(), nullptr, output_view.GetAddressOf());
    if (FAILED(result)) {
        setError(error, "CreateView(Ready frame crop)", result);
        return false;
    }

    ComPtr<ID3DBlob> vertex_blob;
    ComPtr<ID3DBlob> pixel_blob;
    result = D3DCompile(kCropVertexShader, std::strlen(kCropVertexShader), "ready_frame_crop_vs", nullptr, nullptr,
                        "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, vertex_blob.GetAddressOf(), nullptr);
    if (SUCCEEDED(result)) {
        result = D3DCompile(kCropPixelShader, std::strlen(kCropPixelShader), "ready_frame_crop_ps", nullptr, nullptr,
                            "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, pixel_blob.GetAddressOf(), nullptr);
    }
    if (FAILED(result)) {
        setError(error, "D3DCompile(Ready frame crop)", result);
        return false;
    }

    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11Buffer> constants;
    result = device->CreateVertexShader(vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(), nullptr,
                                        vertex_shader.GetAddressOf());
    if (SUCCEEDED(result)) {
        result = device->CreatePixelShader(pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(), nullptr,
                                           pixel_shader.GetAddressOf());
    }
    D3D11_BUFFER_DESC constant_desc{};
    constant_desc.ByteWidth = sizeof(CropConstants);
    constant_desc.Usage = D3D11_USAGE_DEFAULT;
    constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result))
        result = device->CreateBuffer(&constant_desc, nullptr, constants.GetAddressOf());
    if (FAILED(result)) {
        setError(error, "CreateShader(Ready frame crop)", result);
        return false;
    }

    const CropConstants crop_constants{static_cast<uint32_t>(crop.x()), static_cast<uint32_t>(crop.y()), {0, 0}};
    context->UpdateSubresource(constants.Get(), 0, nullptr, &crop_constants, 0, 0);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(crop.width());
    viewport.Height = static_cast<float>(crop.height());
    viewport.MaxDepth = 1.0F;
    ID3D11RenderTargetView* output_target = output_view.Get();
    ID3D11ShaderResourceView* source_resource = source_view.Get();
    ID3D11Buffer* constant_buffer = constants.Get();
    context->OMSetRenderTargets(1, &output_target, nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(pixel_shader.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &source_resource);
    context->PSSetConstantBuffers(0, 1, &constant_buffer);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* no_source = nullptr;
    ID3D11RenderTargetView* no_target = nullptr;
    context->PSSetShaderResources(0, 1, &no_source);
    context->OMSetRenderTargets(1, &no_target, nullptr);
    return true;
}

bool drawWebcam(recorder_core::GpuCompositor& compositor, const WebcamSettings& settings, const QImage& input,
                int width, int height, std::string& error) {
    if (!settings.enabled || input.isNull())
        return true;
    const QImage frame = input.convertToFormat(QImage::Format_ARGB32);
    recorder_core::WebcamPlacement placement;
    placement.x = settings.overlay.x_norm;
    placement.y = settings.overlay.y_norm;
    placement.w = settings.overlay.w_norm;
    placement.h = settings.overlay.h_norm;
    placement.mirror = settings.mirror;
    const recorder_core::WebcamPixelRect rect =
        recorder_core::MapWebcamPlacementToContent(placement, 0, 0, width, height);
    if (!rect.IsValid())
        return true;

    const auto key = settings.chroma_key.active_color();
    recorder_core::ChromaKeyParams chroma;
    chroma.enabled = settings.chroma_key.enabled;
    chroma.r = key.r;
    chroma.g = key.g;
    chroma.b = key.b;
    chroma.tolerance = settings.chroma_key.tolerance;
    chroma.softness = settings.chroma_key.softness;
    chroma.spill_reduction = settings.chroma_key.spill_reduction;
    return compositor.DrawWebcam(frame.constBits(), frame.width(), frame.height(), rect, settings.mirror, chroma, error,
                                 settings.opacity);
}

bool drawCursor(recorder_core::GpuCompositor& compositor, const ReadyFrameSource& source, const QRect& crop,
                std::string& error) {
    if (source.cursor_already_composited || source.target.kind != recorder_core::CaptureTarget::Kind::Monitor)
        return true;
    CURSORINFO cursor_info{};
    cursor_info.cbSize = sizeof(cursor_info);
    if (GetCursorInfo(&cursor_info) == FALSE || (cursor_info.flags & CURSOR_SHOWING) == 0 ||
        cursor_info.hCursor == nullptr)
        return true;

    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (GetMonitorInfoW(reinterpret_cast<HMONITOR>(source.target.native_id), &monitor_info) == FALSE)
        return true;
    const int monitor_width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
    const int monitor_height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
    if (monitor_width <= 0 || monitor_height <= 0)
        return true;

    recorder_core::Win32CursorBitmap bitmap;
    if (!recorder_core::CaptureWin32CursorBitmap(cursor_info.hCursor, bitmap))
        return true;
    const int32_t x = recorder_core::ScaleCoordinateToSource(cursor_info.ptScreenPos.x - monitor_info.rcMonitor.left,
                                                             static_cast<int32_t>(source.width), monitor_width) -
                      bitmap.hotspot_x - crop.x();
    const int32_t y = recorder_core::ScaleCoordinateToSource(cursor_info.ptScreenPos.y - monitor_info.rcMonitor.top,
                                                             static_cast<int32_t>(source.height), monitor_height) -
                      bitmap.hotspot_y - crop.y();
    const recorder_core::CursorSpriteClip clip =
        recorder_core::ClipCursorSprite(x, y, bitmap.width, bitmap.height, crop.width(), crop.height());
    if (!clip.visible)
        return true;

    std::vector<uint8_t> clipped(static_cast<size_t>(clip.w) * clip.h * 4);
    for (int32_t row = 0; row < clip.h; ++row) {
        const size_t source_offset =
            (static_cast<size_t>(clip.bitmap_off_y + row) * bitmap.width + clip.bitmap_off_x) * 4;
        std::memcpy(clipped.data() + static_cast<size_t>(row) * clip.w * 4, bitmap.bgra.data() + source_offset,
                    static_cast<size_t>(clip.w) * 4);
    }
    const recorder_core::WebcamPixelRect rect{clip.x, clip.y, clip.w, clip.h};
    return compositor.DrawCursor(clipped.data(), clip.w, clip.h, rect, error);
}

bool readback(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, uint32_t width,
              uint32_t height, std::vector<uint8_t>& bgra, std::string& error) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT result = device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
    if (FAILED(result)) {
        setError(error, "CreateTexture2D(Ready frame staging)", result);
        return false;
    }
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        setError(error, "Map(Ready frame)", result);
        return false;
    }
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    bgra.resize(row_bytes * height);
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(bgra.data() + static_cast<size_t>(row) * row_bytes,
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch, row_bytes);
    }
    context->Unmap(staging.Get(), 0);
    return true;
}

void captureOnWorker(ReadyFrameSource source, ReadyFrameComposition composition,
                     ReadyFrameCaptureService::Callback callback) {
    OwnedHandle handle(source.shared_handle);
    auto fail = [&callback](std::string error) { callback(false, 0, 0, {}, QString::fromStdString(std::move(error))); };
    if (handle.value == nullptr || source.width == 0 || source.height == 0) {
        fail("No Ready preview frame is available");
        return;
    }

    std::string error;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!createDevice(device, context, error)) {
        fail(std::move(error));
        return;
    }
    ComPtr<ID3D11Device1> device1;
    HRESULT result = device.As(&device1);
    ComPtr<ID3D11Texture2D> shared;
    if (SUCCEEDED(result))
        result = device1->OpenSharedResource1(handle.value, IID_PPV_ARGS(shared.GetAddressOf()));
    if (FAILED(result)) {
        setError(error, "OpenSharedResource1(Ready frame)", result);
        fail(std::move(error));
        return;
    }

    ComPtr<IDXGIKeyedMutex> keyed_mutex;
    result = shared.As(&keyed_mutex);
    if (FAILED(result) || keyed_mutex->AcquireSync(1, 500) != S_OK) {
        fail("Ready preview frame was busy");
        return;
    }
    D3D11_TEXTURE2D_DESC shared_desc{};
    shared->GetDesc(&shared_desc);
    D3D11_TEXTURE2D_DESC local_desc = shared_desc;
    local_desc.Usage = D3D11_USAGE_DEFAULT;
    local_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    local_desc.CPUAccessFlags = 0;
    local_desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> local;
    result = device->CreateTexture2D(&local_desc, nullptr, local.GetAddressOf());
    if (SUCCEEDED(result))
        context->CopyResource(local.Get(), shared.Get());
    keyed_mutex->ReleaseSync(0);
    if (FAILED(result)) {
        setError(error, "CreateTexture2D(Ready frame local)", result);
        fail(std::move(error));
        return;
    }

    ID3D11Texture2D* display_source = local.Get();
    ComPtr<ID3D11Texture2D> tone_mapped;
    if (source.tap.transform != recorder_core::PreviewTapTransform::None) {
        D3D11_TEXTURE2D_DESC tone_desc{};
        tone_desc.Width = source.width;
        tone_desc.Height = source.height;
        tone_desc.MipLevels = 1;
        tone_desc.ArraySize = 1;
        tone_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        tone_desc.SampleDesc.Count = 1;
        tone_desc.Usage = D3D11_USAGE_DEFAULT;
        tone_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        result = device->CreateTexture2D(&tone_desc, nullptr, tone_mapped.GetAddressOf());
        recorder_core::HdrToneMapper mapper;
        const bool sdr_scrgb = source.tap.transform == recorder_core::PreviewTapTransform::ScrgbSdr;
        if (FAILED(result) ||
            !mapper.Init(device.Get(), context.Get(), source.width, source.height, source.tap.peak_scale, sdr_scrgb,
                         error) ||
            !mapper.Convert(local.Get(), tone_mapped.Get(), error)) {
            if (error.empty())
                setError(error, "CreateTexture2D(Ready frame tone map)", result);
            fail(std::move(error));
            return;
        }
        display_source = tone_mapped.Get();
    }

    const QRect crop =
        ReadyFrameCaptureService::SourceCropPixels(source.width, source.height, composition.normalized_source_rect);
    ComPtr<ID3D11Texture2D> bgra;
    if (!cropToBgra(device.Get(), context.Get(), display_source, crop, bgra, error)) {
        fail(std::move(error));
        return;
    }

    recorder_core::GpuCompositor compositor;
    if (!compositor.Init(device.Get(), context.Get(), crop.width(), crop.height(), error) ||
        !compositor.BeginFrame(bgra.Get(), error) ||
        !drawWebcam(compositor, composition.webcam, composition.webcam_frame, crop.width(), crop.height(), error) ||
        (composition.video.capture_cursor && !drawCursor(compositor, source, crop, error))) {
        fail(std::move(error));
        return;
    }

    std::vector<uint8_t> pixels;
    if (!readback(device.Get(), context.Get(), compositor.Result(), static_cast<uint32_t>(crop.width()),
                  static_cast<uint32_t>(crop.height()), pixels, error)) {
        fail(std::move(error));
        return;
    }
    callback(true, static_cast<uint32_t>(crop.width()), static_cast<uint32_t>(crop.height()), std::move(pixels), {});
}

} // namespace

void ReadyFrameCaptureService::Capture(QThreadPool& pool, const ReadyFrameSource& source,
                                       ReadyFrameComposition composition, Callback callback) {
    // The payload is captured by copy, not moved into a mutable lambda: only a
    // const-callable closure converts to the std::function overload of
    // QThreadPool::start. Nothing here is expensive to copy — QImage is
    // implicitly shared and the rest is a handful of small settings structs.
    // Ownership of source.shared_handle still reaches exactly one worker, which
    // closes it.
    pool.start([source, composition, callback]() { captureOnWorker(source, composition, callback); });
}

QRect ReadyFrameCaptureService::SourceCropPixels(uint32_t width, uint32_t height, const QRectF& normalized_rect) {
    if (width == 0 || height == 0)
        return {};
    const QRectF normalized = normalized_rect.normalized();
    const double left = std::clamp(normalized.left(), 0.0, 1.0);
    const double top = std::clamp(normalized.top(), 0.0, 1.0);
    const double right = std::clamp(normalized.right(), left, 1.0);
    const double bottom = std::clamp(normalized.bottom(), top, 1.0);
    int x = std::clamp(static_cast<int>(std::floor(left * width)), 0, static_cast<int>(width) - 1);
    int y = std::clamp(static_cast<int>(std::floor(top * height)), 0, static_cast<int>(height) - 1);
    int right_px = std::clamp(static_cast<int>(std::ceil(right * width)), x + 1, static_cast<int>(width));
    int bottom_px = std::clamp(static_cast<int>(std::ceil(bottom * height)), y + 1, static_cast<int>(height));
    return {x, y, right_px - x, bottom_px - y};
}

} // namespace exosnap::quick
