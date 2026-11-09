#pragma once

#include <exosnap/engine/encoder_input_requirements.h>

#include <d3d11.h>
#include <winrt/base.h>

namespace exosnap::engine {

static_assert(kMaxEncoderInputDimension == D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);

[[nodiscard]] inline DXGI_FORMAT D3D11EncoderInputFormat(EncoderInputFormat format) noexcept {
    switch (format) {
    case EncoderInputFormat::Nv12:
        return DXGI_FORMAT_NV12;
    case EncoderInputFormat::P010:
        return DXGI_FORMAT_P010;
    case EncoderInputFormat::Ayuv:
        return DXGI_FORMAT_AYUV;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] inline EncoderInputFormat EncoderInputFormatFromD3D11(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_NV12:
        return EncoderInputFormat::Nv12;
    case DXGI_FORMAT_P010:
        return EncoderInputFormat::P010;
    case DXGI_FORMAT_AYUV:
        return EncoderInputFormat::Ayuv;
    default:
        return EncoderInputFormat::Unknown;
    }
}

[[nodiscard]] inline UINT D3D11EncoderInputBindings(EncoderInputBind bind) noexcept {
    UINT flags = 0;
    if (HasEncoderInputBindings(bind, EncoderInputBind::RenderTarget))
        flags |= D3D11_BIND_RENDER_TARGET;
    if (HasEncoderInputBindings(bind, EncoderInputBind::UnorderedAccess))
        flags |= D3D11_BIND_UNORDERED_ACCESS;
    if (HasEncoderInputBindings(bind, EncoderInputBind::ShaderResource))
        flags |= D3D11_BIND_SHADER_RESOURCE;
    return flags;
}

[[nodiscard]] inline EncoderInputBind EncoderInputBindingsFromD3D11(UINT flags) noexcept {
    EncoderInputBind bind = EncoderInputBind::None;
    if (flags & D3D11_BIND_RENDER_TARGET)
        bind = bind | EncoderInputBind::RenderTarget;
    if (flags & D3D11_BIND_UNORDERED_ACCESS)
        bind = bind | EncoderInputBind::UnorderedAccess;
    if (flags & D3D11_BIND_SHADER_RESOURCE)
        bind = bind | EncoderInputBind::ShaderResource;
    constexpr UINT known = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (flags & ~known)
        bind = bind | static_cast<EncoderInputBind>(0x80000000u);
    return bind;
}

// Call only after ValidateEncoderInputForHost succeeds. No format or layout
// fallback is selected here.
[[nodiscard]] inline D3D11_TEXTURE2D_DESC D3D11EncoderInputTextureDesc(const EncoderInputRequirements& input) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = input.allocation.width;
    desc.Height = input.allocation.height;
    desc.Format = D3D11EncoderInputFormat(input.format);
    desc.MipLevels = input.mip_levels;
    desc.ArraySize = input.array_size;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11EncoderInputBindings(input.required_bind);
    desc.MiscFlags = input.misc_flags;
    return desc;
}

// Inspect once at registration. Pointer affinity is normalized through COM
// identity so two interfaces on the same D3D11 device compare correctly.
[[nodiscard]] inline EncoderInputValidation ValidateD3D11EncoderInputTexture(const EncoderInputRequirements& input,
                                                                             ID3D11Texture2D* texture) {
    const auto valid = ValidateEncoderInputRequirements(input);
    if (!valid)
        return valid;
    if (!texture)
        return {EncoderInputError::UnsupportedResourceLayout, "Encoder input texture is null."};
    auto* expected = static_cast<ID3D11Device*>(const_cast<void*>(input.device));
    winrt::com_ptr<ID3D11Device> device;
    texture->GetDevice(device.put());
    winrt::com_ptr<IUnknown> expected_identity;
    winrt::com_ptr<IUnknown> actual_identity;
    if (!device || FAILED(expected->QueryInterface(__uuidof(IUnknown), expected_identity.put_void())) ||
        FAILED(device->QueryInterface(__uuidof(IUnknown), actual_identity.put_void())) ||
        expected_identity.get() != actual_identity.get())
        return {EncoderInputError::WrongDevice, "Encoder input texture belongs to a different D3D11 device."};

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    EncoderInputSurfaceDesc surface;
    surface.storage = EncoderInputStorage::D3D11Texture2D;
    surface.format = EncoderInputFormatFromD3D11(desc.Format);
    surface.allocation = {desc.Width, desc.Height};
    surface.bind = EncoderInputBindingsFromD3D11(desc.BindFlags);
    surface.misc_flags = desc.MiscFlags;
    surface.mip_levels = desc.MipLevels;
    surface.array_size = desc.ArraySize;
    surface.subresource = 0;
    surface.sample_count = desc.SampleDesc.Count;
    surface.sample_quality = desc.SampleDesc.Quality;
    surface.gpu_default_usage = desc.Usage == D3D11_USAGE_DEFAULT;
    surface.cpu_access_flags = desc.CPUAccessFlags;
    surface.device = input.device;
    return ValidateEncoderInputSurface(input, surface);
}

} // namespace exosnap::engine
