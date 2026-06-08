#pragma once

#include <recorder_core/webcam_placement.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>

namespace recorder_core {

// D3D11 shader compositor for webcam and cursor overlays.
//
// Threading: all methods are VideoThread-exclusive per ADR-0009. The class does
// not take ownership of the device/context and must not be used from UI code.
class GpuCompositor {
  public:
    struct ChromaKeyParams {
        bool enabled = false;
        uint8_t r = 0;
        uint8_t g = 177;
        uint8_t b = 64;
        float tolerance = 0.30f;
        float softness = 0.05f;
    };

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height, std::string& err);
    bool BeginFrame(ID3D11Texture2D* background, std::string& err);

    bool DrawWebcam(const uint8_t* bgra, int width, int height, const WebcamPixelRect& rect, bool mirror,
                    const ChromaKeyParams& chroma, std::string& err);
    bool DrawCursor(const uint8_t* bgra, int width, int height, const WebcamPixelRect& rect, std::string& err);

    [[nodiscard]] ID3D11Texture2D* Result() const noexcept {
        return composite_tex_.get();
    }

  private:
    struct TextureResource {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        UINT width = 0;
        UINT height = 0;
    };

    struct PixelConstants {
        float key_color[4]; // rgb + tolerance
        float params[4];    // mirror, chroma enabled, force opaque, softness
    };

    bool UploadTexture(TextureResource& resource, const uint8_t* bgra, int width, int height, UINT row_pitch,
                       std::string& err);
    bool DrawTexture(ID3D11ShaderResourceView* srv, const WebcamPixelRect& rect, bool mirror,
                     const ChromaKeyParams& chroma, bool force_opaque, std::string& err);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;

    winrt::com_ptr<ID3D11Texture2D> composite_tex_;
    winrt::com_ptr<ID3D11RenderTargetView> composite_rtv_;
    winrt::com_ptr<ID3D11VertexShader> vertex_shader_;
    winrt::com_ptr<ID3D11PixelShader> pixel_shader_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11BlendState> blend_state_;
    winrt::com_ptr<ID3D11Buffer> constants_;

    TextureResource webcam_tex_;
    TextureResource cursor_tex_;
};

} // namespace recorder_core
