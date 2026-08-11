#include "QuickPreviewRgbaConverter.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

namespace exosnap::quick {
namespace {

constexpr char kVertexShader[] = R"(
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

constexpr char kPixelShader[] = R"(
Texture2D<float4> sourceTexture : register(t0);

float4 main(float4 position : SV_POSITION) : SV_TARGET {
    const float4 color = sourceTexture.Load(int3(int2(position.xy), 0));
    return float4(color.rgb, 1.0f);
}
)";

void setHResultError(std::string& error, const char* operation, HRESULT result) {
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "%s failed 0x%08lX", operation, static_cast<unsigned long>(result));
    error = buffer;
}

} // namespace

bool QuickPreviewRgbaConverter::initialize(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
                                           ID3D11Texture2D* destination, UINT width, UINT height, std::string& error) {
    if (device == nullptr || context == nullptr || source == nullptr || destination == nullptr || width == 0 ||
        height == 0) {
        error = "QuickPreviewRgbaConverter::initialize received invalid arguments";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vertex_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> pixel_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> compiler_error;
    HRESULT result = D3DCompile(kVertexShader, std::strlen(kVertexShader), "quick_preview_rgba_vs", nullptr, nullptr,
                                "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, vertex_blob.GetAddressOf(),
                                compiler_error.GetAddressOf());
    if (FAILED(result)) {
        setHResultError(error, "D3DCompile(Quick preview vertex shader)", result);
        return false;
    }
    compiler_error.Reset();
    result =
        D3DCompile(kPixelShader, std::strlen(kPixelShader), "quick_preview_rgba_ps", nullptr, nullptr, "main", "ps_5_0",
                   D3DCOMPILE_ENABLE_STRICTNESS, 0, pixel_blob.GetAddressOf(), compiler_error.GetAddressOf());
    if (FAILED(result)) {
        setHResultError(error, "D3DCompile(Quick preview pixel shader)", result);
        return false;
    }
    result = device->CreateVertexShader(vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(), nullptr,
                                        vertex_shader_.GetAddressOf());
    if (FAILED(result)) {
        setHResultError(error, "CreateVertexShader(Quick preview)", result);
        return false;
    }
    result = device->CreatePixelShader(pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(), nullptr,
                                       pixel_shader_.GetAddressOf());
    if (FAILED(result)) {
        setHResultError(error, "CreatePixelShader(Quick preview)", result);
        return false;
    }
    result = device->CreateShaderResourceView(source, nullptr, source_view_.GetAddressOf());
    if (FAILED(result)) {
        setHResultError(error, "CreateShaderResourceView(Quick preview source)", result);
        return false;
    }
    result = device->CreateRenderTargetView(destination, nullptr, destination_view_.GetAddressOf());
    if (FAILED(result)) {
        setHResultError(error, "CreateRenderTargetView(Quick preview RGBA target)", result);
        return false;
    }

    context_ = context;
    width_ = width;
    height_ = height;
    return true;
}

bool QuickPreviewRgbaConverter::convert(std::string& error) {
    if (context_ == nullptr || source_view_ == nullptr || destination_view_ == nullptr) {
        error = "QuickPreviewRgbaConverter::convert called before initialize";
        return false;
    }

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MaxDepth = 1.0F;
    ID3D11RenderTargetView* destination = destination_view_.Get();
    ID3D11ShaderResourceView* source = source_view_.Get();
    context_->OMSetRenderTargets(1, &destination, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, &source);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* no_source = nullptr;
    ID3D11RenderTargetView* no_target = nullptr;
    context_->PSSetShaderResources(0, 1, &no_source);
    context_->OMSetRenderTargets(1, &no_target, nullptr);
    return true;
}

} // namespace exosnap::quick
