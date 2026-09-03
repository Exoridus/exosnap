#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <string>
#include <unordered_map>

namespace exosnap::engine {

// scRGB FP16 -> SDR BT.709 tone-map render pass. Reads an FP16 shader-resource
// texture (scRGB: linear, BT.709 primaries, 1.0 = 80 cd/m^2 reference white) and
// writes a BGRA8 render target of identical dimensions, applying the documented
// per-channel highlight roll-off + sRGB OETF from hdr_tonemap.h. The result is
// an ordinary SDR desktop surface that the existing VideoProcessor path converts
// to NV12/P010 unchanged.
//
// Threading: an instance is single-thread — every method runs on the thread that
// owns the passed device context. The engine's instance is VideoThread-exclusive
// (ADR-0009); the DXGI preview runs its own instance on its render thread to
// display the engine's FP16 tap. The class does not take ownership of the
// device/context.
class HdrToneMapper {
  public:
    // peak_scale: display peak luminance in reference-white multiples that maps
    // to output 1.0 (see HdrPeakScale in hdr_tonemap.h). Ignored when
    // sdr_scrgb_source is true.
    // sdr_scrgb_source: the source is an SDR desktop that merely happens to be
    // delivered as linear scRGB (Advanced Color Management). It carries no HDR
    // headroom, so the pass skips the highlight roll-off and applies the sRGB
    // OETF alone (see OdCaptureMode::SdrScrgb). The transfer is the same on both
    // paths; only the roll-off differs.
    // paper_white_scale: the OS SDR reference white in reference-white multiples
    // (SdrPaperWhiteScale in hdr_tonemap.h). On an HDR desktop Windows renders
    // SDR content at that level rather than at scRGB's nominal 80 nits, so the
    // pass divides by it before rolling off; without that an sRGB mid-grey comes
    // out far too bright. Ignored when sdr_scrgb_source is true, where 1.0
    // already means the display's own reference white. Defaults to the
    // scene-referred identity.
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height, float peak_scale,
              bool sdr_scrgb_source, std::string& err, float paper_white_scale = 1.0f, bool pq_source = false);

    // Tone-map src (FP16, must have D3D11_BIND_SHADER_RESOURCE) into dst (BGRA8,
    // must have D3D11_BIND_RENDER_TARGET). Both must be width x height. SRVs and
    // RTVs are created lazily and cached by texture pointer (the capture path
    // reuses a small fixed set of source/destination textures per session).
    bool Convert(ID3D11Texture2D* src, ID3D11Texture2D* dst, std::string& err);

  private:
    ID3D11ShaderResourceView* SrvFor(ID3D11Texture2D* tex, std::string& err);
    ID3D11RenderTargetView* RtvFor(ID3D11Texture2D* tex, std::string& err);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;

    winrt::com_ptr<ID3D11VertexShader> vertex_shader_;
    winrt::com_ptr<ID3D11PixelShader> pixel_shader_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11Buffer> constants_;

    std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11ShaderResourceView>> srv_cache_;
    std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11RenderTargetView>> rtv_cache_;
};

} // namespace exosnap::engine
