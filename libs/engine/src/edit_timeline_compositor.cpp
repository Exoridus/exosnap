#include <cstdint>
#include <exosnap/engine/edit_frame_gpu_converter.h>
#include <exosnap/engine/edit_timeline_compositor.h>
#include <exosnap/engine/gpu_surface_inventory.h>
#include <exosnap/engine/output_geometry.h>
#include <exosnap/engine/sdr_transfer.h>
#include <span>
#include <string>
#include <winrt/base.h>

#include "measured_shader_compile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

namespace exosnap::engine {
namespace {
constexpr char kVertex[] = R"(
float4 main(uint id : SV_VertexID) : SV_POSITION {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";
// Resolves one track: its junction pair is summed with complementary weights,
// each scaled by the clip's own opacity, into premultiplied color and coverage.
// Outside a source's contain rectangle the source contributes nothing, so the
// layer below remains visible in letterbox bands.
constexpr char kLayerPixel[] = R"(
Texture2D<float4> A : register(t0);
Texture2D<float4> B : register(t1);
SamplerState linearSampler : register(s0);
cbuffer Parameters : register(b0) {
    float4 rectA;
    float4 rectB;
    float4 factors;
};
float4 contained(Texture2D<float4> source, float2 position, float4 rect, float factor) {
    float2 uv = (position - rect.xy) / max(rect.zw, 1.0f);
    if (factor <= 0 || any(uv < 0) || any(uv >= 1)) return float4(0, 0, 0, 0);
    return float4(source.SampleLevel(linearSampler, uv, 0).rgb * factor, factor);
}
float4 main(float4 position : SV_POSITION) : SV_TARGET {
    return contained(A, position.xy, rectA, factors.x) + contained(B, position.xy, rectB, factors.y);
}
)";
// The accumulator holds premultiplied linear light over transparent black.
// Composing it over opaque black leaves the color unchanged.
constexpr char kEncodePixel[] = R"(
Texture2D<float4> Accumulated : register(t0);
float4 main(float4 position : SV_POSITION) : SV_TARGET {
    float3 rgb = Accumulated.Load(int3(position.xy, 0)).rgb;
    return float4(LinearToBt709(rgb.r), LinearToBt709(rgb.g), LinearToBt709(rgb.b), 1);
}
)";
struct Parameters {
    float rectangles[2][4]{};
    float factors[4]{};
};
bool Check(HRESULT status, const char* operation, std::string& error) {
    if (SUCCEEDED(status))
        return true;
    error = std::string(operation) + " failed (HRESULT " + std::to_string(static_cast<uint32_t>(status)) + ").";
    return false;
}
bool Compile(const std::string& source, const char* name, const char* profile, winrt::com_ptr<ID3DBlob>& blob,
             std::string& error) {
    return Check(MeasuredD3DCompile(source.data(), source.size(), name, nullptr, nullptr, "main", profile,
                                    D3DCOMPILE_ENABLE_STRICTNESS, 0, blob.put(), nullptr),
                 name, error);
}
} // namespace

bool EditTimelineCompositor::EnsureSurface(Surface& surface, FrameSize size, DXGI_FORMAT format, std::string& error) {
    if (surface.texture && surface.size.width == size.width && surface.size.height == size.height)
        return true;
    surface = {};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size.width;
    desc.Height = size.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (!Check(CreateTrackedTexture2D(device_, &desc, nullptr, surface.texture.put(), GpuSurfaceOwner::Editor),
               "Create timeline texture", error) ||
        !Check(device_->CreateRenderTargetView(surface.texture.get(), nullptr, surface.rtv.put()),
               "Create timeline RTV", error) ||
        !Check(device_->CreateShaderResourceView(surface.texture.get(), nullptr, surface.srv.put()),
               "Create timeline SRV", error))
        return false;
    surface.size = size;
    ++texture_creations_;
    return true;
}

bool EditTimelineCompositor::EnsureConverter(size_t index, std::string& error) {
    if (converters_[index])
        return true;
    auto converter = std::make_unique<EditFrameGpuConverter>();
    if (!converter->Init(device_, context_, error))
        return false;
    converters_[index] = std::move(converter);
    return true;
}

bool EditTimelineCompositor::Init(ID3D11Device* device, ID3D11DeviceContext* context, FrameSize output,
                                  std::string& error) {
    *this = EditTimelineCompositor{};
    if (!device || !context || !IsEncoderAlignedSize(output) || output.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        output.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        error = "Timeline output requires an even, nonzero geometry and a D3D11 device.";
        return false;
    }
    device_ = device;
    context_ = context;
    size_ = output;
    if (!EnsureSurface(output_, output, DXGI_FORMAT_B8G8R8A8_UNORM, error) ||
        !EnsureSurface(accumulator_, output, DXGI_FORMAT_R16G16B16A16_FLOAT, error))
        return false;
    winrt::com_ptr<ID3DBlob> vertex;
    winrt::com_ptr<ID3DBlob> layer;
    winrt::com_ptr<ID3DBlob> encode;
    if (!Compile(kVertex, "timeline_vs", "vs_5_0", vertex, error) ||
        !Compile(kLayerPixel, "timeline_layer_ps", "ps_5_0", layer, error) ||
        !Compile(std::string(kBt709TransferHlsl) + kEncodePixel, "timeline_encode_ps", "ps_5_0", encode, error) ||
        !Check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, vertex_.put()),
               "Create timeline vertex shader", error) ||
        !Check(
            device->CreatePixelShader(layer->GetBufferPointer(), layer->GetBufferSize(), nullptr, layer_pixel_.put()),
            "Create timeline layer shader", error) ||
        !Check(device->CreatePixelShader(encode->GetBufferPointer(), encode->GetBufferSize(), nullptr,
                                         encode_pixel_.put()),
               "Create timeline encode shader", error))
        return false;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Parameters);
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC blend{};
    auto& target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return Check(device->CreateBuffer(&buffer, nullptr, constants_.put()), "Create timeline constants", error) &&
           Check(device->CreateSamplerState(&sampler, sampler_.put()), "Create timeline sampler", error) &&
           Check(device->CreateBlendState(&blend, over_.put()), "Create timeline blend state", error);
}

void EditTimelineCompositor::Draw(ID3D11PixelShader* shader, ID3D11RenderTargetView* target,
                                  ID3D11ShaderResourceView* const* views, UINT view_count, ID3D11BlendState* blend) {
    // The source conversion is itself a draw, and Qt or a previous native pass
    // can leave scissor, color-write masks or unused shader stages enabled.
    context_->ClearState();
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(size_.width), static_cast<float>(size_.height), 0, 1};
    ID3D11Buffer* constants = constants_.get();
    ID3D11SamplerState* sampler = sampler_.get();
    context_->OMSetBlendState(blend, nullptr, 0xffffffff);
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.get(), nullptr, 0);
    context_->PSSetShader(shader, nullptr, 0);
    context_->PSSetShaderResources(0, view_count, views);
    context_->PSSetConstantBuffers(0, 1, &constants);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* empty[2]{};
    context_->PSSetShaderResources(0, view_count, empty);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
}

bool EditTimelineCompositor::Compose(std::span<const TimelineVideoLayer> layers, std::string& error) {
    if (!context_ || !output_.rtv || !accumulator_.rtv || !vertex_ || !layer_pixel_ || !encode_pixel_ || !constants_ ||
        !sampler_ || !over_) {
        error = "Timeline compositor needs initialization.";
        return false;
    }
    if (layers.size() > kMaxTimelineVideoSources) {
        error = "The timeline frame references more video sources than the compositor admits.";
        return false;
    }
    std::vector<size_t> order(layers.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t left, size_t right) { return layers[left].layer < layers[right].layer; });
    for (size_t begin = 0; begin < order.size();) {
        size_t end = begin;
        double total = 0;
        while (end < order.size() && layers[order[end]].layer == layers[order[begin]].layer) {
            const auto& layer = layers[order[end]];
            if (!std::isfinite(layer.weight) || layer.weight < 0 || layer.weight > 1 || !std::isfinite(layer.opacity) ||
                layer.opacity < 0 || layer.opacity > 1) {
                error = "Invalid timeline blend weight or opacity.";
                return false;
            }
            total += layer.weight;
            ++end;
        }
        if (end - begin > 2 || (end - begin == 2 && std::abs(total - 1) > 0.000001)) {
            error = "A timeline layer needs one source with partial coverage or a complementary junction pair.";
            return false;
        }
        begin = end;
    }
    if (!Check(device_->GetDeviceRemovedReason(), "Timeline D3D11 device", error))
        return false;
    for (size_t i = 0; i < layers.size(); ++i) {
        const auto& frame = layers[i].frame;
        context_->ClearState();
        if (!EnsureConverter(i, error) ||
            !EnsureSurface(sources_[i], {frame.width, frame.height}, DXGI_FORMAT_R16G16B16A16_FLOAT, error) ||
            !converters_[i]->Convert(frame, sources_[i].texture.get(), 1, error, true, sources_[i].rtv.get()))
            return false;
    }
    const float transparent[4]{0, 0, 0, 0};
    context_->ClearRenderTargetView(accumulator_.rtv.get(), transparent);
    for (size_t begin = 0; begin < order.size();) {
        Parameters parameters;
        ID3D11ShaderResourceView* views[2]{};
        size_t slot = 0;
        size_t end = begin;
        for (; end < order.size() && layers[order[end]].layer == layers[order[begin]].layer; ++end, ++slot) {
            const auto index = order[end];
            const auto& layer = layers[index];
            const auto rect = ResolveContainRect({layer.frame.width, layer.frame.height}, size_);
            if (!rect) {
                error = "A timeline source has no containable geometry.";
                return false;
            }
            parameters.rectangles[slot][0] = static_cast<float>(rect->x);
            parameters.rectangles[slot][1] = static_cast<float>(rect->y);
            parameters.rectangles[slot][2] = static_cast<float>(rect->width);
            parameters.rectangles[slot][3] = static_cast<float>(rect->height);
            parameters.factors[slot] = static_cast<float>(layer.weight * layer.opacity);
            views[slot] = sources_[index].srv.get();
        }
        if (!views[1])
            views[1] = views[0];
        context_->UpdateSubresource(constants_.get(), 0, nullptr, &parameters, 0, 0);
        Draw(layer_pixel_.get(), accumulator_.rtv.get(), views, 2, over_.get());
        begin = end;
    }
    ID3D11ShaderResourceView* accumulated = accumulator_.srv.get();
    Draw(encode_pixel_.get(), output_.rtv.get(), &accumulated, 1, nullptr);
    return Check(device_->GetDeviceRemovedReason(), "Timeline composition", error);
}

} // namespace exosnap::engine
