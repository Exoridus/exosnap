#include <cstdint>
#include <exosnap/engine/edit_timeline_compositor.h>
#include <exosnap/engine/gpu_surface_inventory.h>
#include <exosnap/engine/output_geometry.h>
#include <exosnap/engine/sdr_transfer.h>
#include <span>
#include <string>
#include <winrt/base.h>

#include "measured_shader_compile.h"

#include <cmath>
#include <cstring>

namespace exosnap::engine {
namespace {
constexpr char kVertex[] = R"(
float4 main(uint id : SV_VertexID) : SV_POSITION {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";
constexpr char kPixel[] = R"(
Texture2D<float4> A : register(t0);
Texture2D<float4> B : register(t1);
SamplerState linearSampler : register(s0);
cbuffer Parameters : register(b0) {
    float4 rectA;
    float4 rectB;
    float4 weights;
};
float3 contained(Texture2D<float4> source, float2 position, float4 rect) {
    float2 uv = (position - rect.xy) / max(rect.zw, 1.0f);
    if (any(uv < 0) || any(uv >= 1)) return float3(0, 0, 0);
    return source.SampleLevel(linearSampler, uv, 0).rgb;
}
float4 main(float4 position : SV_POSITION) : SV_TARGET {
    float3 rgb = contained(A, position.xy, rectA) * weights.x;
    if (weights.y > 0) rgb += contained(B, position.xy, rectB) * weights.y;
    return float4(LinearToBt709(rgb.r), LinearToBt709(rgb.g), LinearToBt709(rgb.b), 1);
}
)";
struct Parameters {
    float rectangles[2][4]{};
    float weights[4]{};
};
bool Check(HRESULT status, const char* operation, std::string& error) {
    if (SUCCEEDED(status))
        return true;
    error = std::string(operation) + " failed (HRESULT " + std::to_string(static_cast<uint32_t>(status)) + ").";
    return false;
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
    if (!EnsureSurface(output_, output, DXGI_FORMAT_B8G8R8A8_UNORM, error))
        return false;
    for (auto& converter : converters_)
        if (!converter.Init(device, context, error))
            return false;
    winrt::com_ptr<ID3DBlob> vertex;
    winrt::com_ptr<ID3DBlob> pixel;
    const std::string pixel_source = std::string(kBt709TransferHlsl) + kPixel;
    if (!Check(MeasuredD3DCompile(kVertex, std::strlen(kVertex), "timeline_vs", nullptr, nullptr, "main", "vs_5_0",
                                  D3DCOMPILE_ENABLE_STRICTNESS, 0, vertex.put(), nullptr),
               "Compile timeline vertex", error) ||
        !Check(MeasuredD3DCompile(pixel_source.data(), pixel_source.size(), "timeline_ps", nullptr, nullptr, "main",
                                  "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, pixel.put(), nullptr),
               "Compile timeline pixel", error) ||
        !Check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, vertex_.put()),
               "Create timeline vertex shader", error) ||
        !Check(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, pixel_.put()),
               "Create timeline pixel shader", error))
        return false;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Parameters);
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    return Check(device->CreateBuffer(&buffer, nullptr, constants_.put()), "Create timeline constants", error) &&
           Check(device->CreateSamplerState(&sampler, sampler_.put()), "Create timeline sampler", error);
}

bool EditTimelineCompositor::Compose(std::span<const TimelineVideoLayer> layers, std::string& error) {
    if (!context_ || !output_.texture || !output_.rtv || !vertex_ || !pixel_ || !constants_ || !sampler_ ||
        layers.size() > 2) {
        error = "Timeline compositor needs initialization and at most two sources.";
        return false;
    }
    if (!Check(device_->GetDeviceRemovedReason(), "Timeline D3D11 device", error))
        return false;
    // The source conversion is itself a draw. Qt or a previous native pass can
    // leave scissor, color-write masks or unused shader stages enabled.
    context_->ClearState();
    Parameters parameters;
    double total = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
        const auto& layer = layers[i];
        if (!std::isfinite(layer.weight) || layer.weight < 0 || layer.weight > 1) {
            error = "Invalid timeline blend weight.";
            return false;
        }
        total += layer.weight;
        const FrameSize source{layer.frame.width, layer.frame.height};
        const auto rect = ResolveContainRect(source, size_);
        if (!rect || !EnsureSurface(sources_[i], source, DXGI_FORMAT_R16G16B16A16_FLOAT, error) ||
            !converters_[i].Convert(layer.frame, sources_[i].texture.get(), 1, error, true, sources_[i].rtv.get()))
            return false;
        parameters.rectangles[i][0] = static_cast<float>(rect->x);
        parameters.rectangles[i][1] = static_cast<float>(rect->y);
        parameters.rectangles[i][2] = static_cast<float>(rect->width);
        parameters.rectangles[i][3] = static_cast<float>(rect->height);
        parameters.weights[i] = static_cast<float>(layer.weight);
    }
    if (!layers.empty() && std::abs(total - 1) > 0.000001) {
        error = "Timeline video weights must sum to one.";
        return false;
    }
    const float black[4]{0, 0, 0, 1};
    context_->ClearRenderTargetView(output_.rtv.get(), black);
    if (layers.empty())
        return true;
    context_->UpdateSubresource(constants_.get(), 0, nullptr, &parameters, 0, 0);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(size_.width), static_cast<float>(size_.height), 0, 1};
    ID3D11RenderTargetView* target = output_.rtv.get();
    ID3D11ShaderResourceView* views[2]{sources_[0].srv.get(), sources_[1].srv.get()};
    ID3D11Buffer* constants = constants_.get();
    ID3D11SamplerState* sampler = sampler_.get();
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->RSSetState(nullptr);
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.get(), nullptr, 0);
    context_->PSSetShader(pixel_.get(), nullptr, 0);
    context_->PSSetShaderResources(0, 2, views);
    context_->PSSetConstantBuffers(0, 1, &constants);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* empty[2]{};
    context_->PSSetShaderResources(0, 2, empty);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    return Check(device_->GetDeviceRemovedReason(), "Timeline composition", error);
}

} // namespace exosnap::engine
