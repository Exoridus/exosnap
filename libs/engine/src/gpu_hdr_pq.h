#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <string>
#include <unordered_map>

namespace exosnap::engine {

// Native HDR10 render pass: a captured HDR desktop surface -> PQ / BT.2020
// Y'CbCr in a P010 encode texture. Two full-screen pixel passes write the P010
// luma (R16_UNORM) and chroma (R16G16_UNORM) planes directly, applying the same
// per-pixel math as the CPU reference in hdr_pq.h. The D3D11 VideoProcessor
// cannot perform any HDR colour-space conversion on the target hardware, so the
// native path bypasses it entirely and does its own colour transform + crop /
// contain-fit scale / letterbox geometry.
//
// Two input variants (chosen at Init via input_is_pq):
//   * scRGB FP16 (linear BT.709): nit-scaled, BT.709->BT.2020, PQ-encoded, then
//     converted to Y'CbCr and packed.
//   * HDR10 R10G10B10A2 (already PQ/BT.2020 R'G'B'): only the Y'CbCr conversion
//     + packing is applied (no re-transfer).
//
// Threading: all methods are VideoThread-exclusive (ADR-0009). The class does
// not own the device/context.
class HdrPqConverter {
  public:
    struct Geometry {
        // Source crop rectangle in source pixels (the region sampled from the
        // capture texture). Full frame when no Region crop is active.
        uint32_t src_crop_x = 0;
        uint32_t src_crop_y = 0;
        uint32_t src_crop_w = 0;
        uint32_t src_crop_h = 0;
        uint32_t src_width = 0;  // full capture-texture width
        uint32_t src_height = 0; // full capture-texture height
        // Destination content rectangle in the P010 encode texture (contain-fit
        // result; equals the whole frame when aspect ratios match). Pixels
        // outside it are the letterbox bars.
        uint32_t content_x = 0;
        uint32_t content_y = 0;
        uint32_t content_w = 0;
        uint32_t content_h = 0;
        uint32_t encode_width = 0;
        uint32_t encode_height = 0;
    };

    // --- Chroma geometry, as two pure decisions -----------------------------
    //
    // The chroma plane of a 4:2:0 surface is half size in both axes, and the two
    // things that follow from that were each computed in a way that is only
    // correct in the easy case.

    // The left-siting shift, in texcoord units of the OUTPUT raster.
    //
    // The chroma pass covers the content rectangle at half resolution, so each
    // chroma sample lands between two output luma columns (centre-sited). Moving
    // it onto the left column is a shift of half an OUTPUT luma pixel -- and
    // `texcoord` runs 0..1 across the content rectangle, so that is
    // 0.5 / content_w.
    //
    // It was 0.5 / src_crop_w, which is the same number only at 1:1. Downscaling
    // made the shift too small and upscaling too large, so the declared left
    // siting matched the GPU output at exactly one scale factor.
    [[nodiscard]] static constexpr float LeftSitingShiftTexels(uint32_t content_w) noexcept {
        return content_w == 0 ? 0.0f : 0.5f / static_cast<float>(content_w);
    }

    // The half-resolution viewport that covers a content rectangle.
    //
    // Both the origin and the size were divided by two with integer division. For
    // an odd content height -- 1079 rows inside an even encode height, which a
    // contain-fit produces -- 1079/2 is 539, and the chroma plane needs 540 rows
    // to reach the last luma row: that row kept the neutral clear value and the
    // bottom line of the picture came out colourless.
    //
    // So the origin floors (a chroma sample that straddles the content edge must
    // still be drawn) and the END is what rounds up, with the size derived from
    // the two. That covers the last row and column without moving the content:
    // the viewport is a render target region, and where the content sits is set
    // by the luma pass and the texcoord mapping, neither of which this touches.
    struct ChromaViewport {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t w = 0;
        uint32_t h = 0;
    };
    [[nodiscard]] static constexpr ChromaViewport ChromaViewportFor(uint32_t content_x, uint32_t content_y,
                                                                    uint32_t content_w, uint32_t content_h) noexcept {
        const uint32_t x0 = content_x / 2;
        const uint32_t y0 = content_y / 2;
        const uint32_t x1 = (content_x + content_w + 1) / 2;
        const uint32_t y1 = (content_y + content_h + 1) / 2;
        return ChromaViewport{x0, y0, x1 > x0 ? x1 - x0 : 0u, y1 > y0 ? y1 - y0 : 0u};
    }

    // input_is_pq: false for scRGB FP16 (R16G16B16A16_FLOAT), true for an
    // already-PQ HDR10 R10G10B10A2 desktop. src_format is the capture texture's
    // DXGI format (used for the source shader-resource view).
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, const Geometry& geom, bool input_is_pq,
              DXGI_FORMAT src_format, std::string& err);

    // Convert src (HDR capture surface, must have D3D11_BIND_SHADER_RESOURCE)
    // into dst (a P010 encode texture, must have D3D11_BIND_RENDER_TARGET). dst
    // is filled at the encode resolution with crop/scale/letterbox applied.
    bool Convert(ID3D11Texture2D* src, ID3D11Texture2D* dst, std::string& err);

  private:
    ID3D11ShaderResourceView* SrvFor(ID3D11Texture2D* tex, std::string& err);
    ID3D11RenderTargetView*
    PlaneRtvFor(ID3D11Texture2D* tex, DXGI_FORMAT plane_format,
                std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11RenderTargetView>>& cache, std::string& err);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    Geometry geom_{};
    DXGI_FORMAT src_format_ = DXGI_FORMAT_R16G16B16A16_FLOAT;

    winrt::com_ptr<ID3D11VertexShader> vertex_shader_;
    winrt::com_ptr<ID3D11PixelShader> luma_shader_;
    winrt::com_ptr<ID3D11PixelShader> chroma_shader_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11Buffer> constants_;

    std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11ShaderResourceView>> srv_cache_;
    std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11RenderTargetView>> luma_rtv_cache_;
    std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11RenderTargetView>> chroma_rtv_cache_;
};

} // namespace exosnap::engine
