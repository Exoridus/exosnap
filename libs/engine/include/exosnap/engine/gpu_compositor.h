#pragma once

#include <exosnap/engine/overlay_shader.h>
#include <exosnap/engine/overlay_sprites.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/sdr_white_level.h>
#include <exosnap/engine/webcam_placement.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace exosnap::engine {

// D3D11 shader compositor for webcam, cursor and sprite overlays.
//
// Threading: single-thread. Every call comes from the thread that owns the
// immediate context it was initialised with (the recording's VideoThread, or
// the Quick render thread for the preview). The class does not take ownership
// of the device/context.
class GpuCompositor {
  public:
    // The key parameters and the shader that consumes them live in
    // <exosnap/engine/overlay_shader.h>.
    using ChromaKeyParams = exosnap::engine::ChromaKeyParams;

    // render_format: format of the composite render target. Must match the
    // capture source's frame format (BeginFrame copies the background via
    // CopyResource, which requires identical formats). BGRA8 for WGC and
    // 8-bit SDR OD capture; R8G8B8A8 for a preview source already in Qt's
    // texture layout; R10G10B10A2 for a 10 bpc SDR desktop;
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
    // Replace the overlay reference white on an initialised compositor, with the
    // same bounds Init applies (EffectiveOverlayReferenceWhiteNits) and the same
    // FP16-only meaning.
    //
    // The captured display's SDR content brightness can change while a recording
    // runs, and from that moment the desktop underneath the overlays is composed
    // at the new level. An overlay left at the old one is the only thing in the
    // frame that did not move with it.
    void SetOverlayReferenceWhiteNits(float overlay_reference_white_nits) noexcept;

    bool BeginFrame(ID3D11Texture2D* background, std::string& err);

    // opacity: uniform overlay opacity [0,1] multiplied onto the sprite's alpha
    // after chroma keying (1.0 = fully opaque). Values outside [0,1] are clamped.
    bool DrawWebcam(const uint8_t* bgra, int width, int height, const WebcamPixelRect& rect, bool mirror,
                    const ChromaKeyParams& chroma, std::string& err, float opacity = 1.0f,
                    std::optional<uint64_t> generation = std::nullopt);
    // Draws a camera frame in any WebcamPixelFormat. A YUV frame is uploaded in
    // its own layout and converted to RGB on the GPU, once per generation; the
    // sprite then composites exactly like a BGRA one. An incomplete payload
    // draws nothing and is not an error, matching a provider with no frame yet.
    bool DrawWebcam(const WebcamFrameSnapshot& frame, const WebcamPixelRect& rect, bool mirror,
                    const ChromaKeyParams& chroma, std::string& err, float opacity = 1.0f);
    bool DrawCursor(const uint8_t* bgra, int width, int height, const WebcamPixelRect& rect, std::string& err);

    // The inverting plane of a mask cursor, drawn after DrawCursor and over the
    // same rectangle. `bgra` is opaque white where the pixel inverts and zero
    // elsewhere, which the blend turns into "1 - destination" there and leaves
    // the destination untouched everywhere else -- no pixel is clipped, so a
    // cursor mixing inverting and ordinary opaque pixels composites correctly.
    //
    // On the native HDR10 path this draws the pixels opaque instead; see
    // MaskCursorComposition, whose value for this compositor MaskComposition()
    // reports.
    bool DrawCursorInvert(const uint8_t* bgra, int width, int height, const WebcamPixelRect& rect, std::string& err);

    // Draws one sprite layer, its quads in list order, with the cursor's
    // compositing rules (sprite alpha, sRGB, HDR overlay reference white).
    // `content` is the rectangle of the composite the quad destinations are
    // relative to. Each layer_index keeps its own atlas texture, uploaded again
    // only when the atlas generation or size differs from the last one drawn
    // there. A layer without a complete atlas draws nothing and is not an error.
    bool DrawSpriteLayer(size_t layer_index, const SpriteLayerSnapshot& layer, const WebcamPixelRect& content,
                         std::string& err);

    // Which inversion semantics this compositor's render format allows.
    [[nodiscard]] MaskCursorComposition MaskComposition() const noexcept {
        return SelectMaskCursorComposition(hdr_linear_);
    }

    [[nodiscard]] ID3D11Texture2D* Result() const noexcept {
        return composite_tex_.get();
    }

    [[nodiscard]] uint64_t WebcamUploadCount() const noexcept {
        return webcam_upload_count_;
    }

    [[nodiscard]] uint64_t SpriteAtlasUploadCount() const noexcept {
        return sprite_atlas_upload_count_;
    }

    [[nodiscard]] uint64_t SpriteQuadDrawCount() const noexcept {
        return sprite_quad_draw_count_;
    }

  private:
    struct TextureResource {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        winrt::com_ptr<ID3D11RenderTargetView> rtv;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    // Recreates `resource` only when its size or format changes.
    bool EnsureTexture(TextureResource& resource, UINT width, UINT height, DXGI_FORMAT format, bool render_target,
                       std::string& err);
    bool ConvertWebcamYuv(const WebcamFrameSnapshot& frame, std::string& err);
    bool UploadTexture(TextureResource& resource, const uint8_t* bgra, int width, int height, UINT row_pitch,
                       std::string& err);
    // uv_rect: u0, v0, u1, v1 of the texture the rectangle shows; null = all of it.
    bool DrawTexture(ID3D11ShaderResourceView* srv, const WebcamPixelRect& rect, bool mirror,
                     const ChromaKeyParams& chroma, bool force_opaque, float opacity, std::string& err,
                     ID3D11BlendState* blend = nullptr, const float* uv_rect = nullptr);

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
    winrt::com_ptr<ID3D11BlendState> invert_blend_state_;
    winrt::com_ptr<ID3D11Buffer> constants_;

    TextureResource webcam_tex_;
    // YUV webcam path: the raw planes as uploaded, and the shaders that turn
    // them into webcam_tex_. Compiled on the first YUV frame, so a BGRA-only
    // session never pays for them.
    TextureResource webcam_luma_;   // NV12 Y (R8_UINT) or YUY2 pairs (R8G8B8A8_UINT)
    TextureResource webcam_chroma_; // NV12 interleaved CbCr (R8G8_UINT)
    winrt::com_ptr<ID3D11PixelShader> nv12_shader_;
    winrt::com_ptr<ID3D11PixelShader> yuy2_shader_;
    winrt::com_ptr<ID3D11Buffer> yuv_constants_;
    std::optional<uint64_t> webcam_generation_;
    uint64_t webcam_upload_count_ = 0;
    TextureResource cursor_tex_;
    TextureResource cursor_invert_tex_;

    struct SpriteAtlasTexture {
        TextureResource texture;
        std::optional<uint64_t> generation;
    };
    std::vector<SpriteAtlasTexture> sprite_atlases_;
    uint64_t sprite_atlas_upload_count_ = 0;
    uint64_t sprite_quad_draw_count_ = 0;
};

} // namespace exosnap::engine
