#pragma once

#include <recorder_core/sdr_white_level.h>
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
        uint8_t g = 255;
        uint8_t b = 0;
        float tolerance = 0.40f;
        float softness = 0.15f;
        float spill_reduction = 0.30f;
    };

    // render_format: format of the composite render target. Must match the
    // capture source's frame format (BeginFrame copies the background via
    // CopyResource, which requires identical formats). BGRA8 for WGC and
    // 8-bit SDR OD capture; R10G10B10A2 for a 10 bpc SDR desktop;
    // R16G16B16A16_FLOAT for the native HDR10 path, where the background is
    // linear scRGB and overlays are composited in linear light (sRGB-decoded and
    // scaled to the HDR overlay reference white). The overlay shader samples
    // normalized floats and writes through the RTV; webcam/cursor upload textures
    // stay BGRA8 regardless.
    // overlay_reference_white_nits: the linear-light level SDR overlay sprites
    // are scaled to on the FP16 native-HDR path (ignored for SDR formats).
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height, std::string& err,
              DXGI_FORMAT render_format = DXGI_FORMAT_B8G8R8A8_UNORM,
              float overlay_reference_white_nits = kDefaultSdrWhiteLevelNits);
    bool BeginFrame(ID3D11Texture2D* background, std::string& err);

    // opacity: uniform overlay opacity [0,1] multiplied onto the sprite's alpha
    // after chroma keying (1.0 = fully opaque). Values outside [0,1] are clamped.
    bool DrawWebcam(const uint8_t* bgra, int width, int height, const WebcamPixelRect& rect, bool mirror,
                    const ChromaKeyParams& chroma, std::string& err, float opacity = 1.0f);
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
        float key_color[4]; // r, g, b (0-1) + tolerance
        // x=mirror, y=mode(0=cursor/1=chroma/2=opaque), z=spillReduction, w=softness
        float params[4];
        // x=hdrLinear (0/1), y=refWhiteScale, z=opacity, w=reserved
        float params2[4];
    };

    bool UploadTexture(TextureResource& resource, const uint8_t* bgra, int width, int height, UINT row_pitch,
                       std::string& err);
    bool DrawTexture(ID3D11ShaderResourceView* srv, const WebcamPixelRect& rect, bool mirror,
                     const ChromaKeyParams& chroma, bool force_opaque, float opacity, std::string& err);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    DXGI_FORMAT render_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool hdr_linear_ = false; // true for the FP16 native-HDR path (linear-light compositing)
    float overlay_ref_white_nits_ = kDefaultSdrWhiteLevelNits;

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
