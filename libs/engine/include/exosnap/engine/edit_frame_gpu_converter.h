#pragma once

#include <exosnap/engine/edit_player_engine.h> // RawDecodedVideoFrame

#include <d3d11.h>
#include <winrt/base.h>

#include <string>

namespace exosnap::engine {

// GPU replacement for yuv_to_bgra.h's CPU conversion, for the editor playback
// path only. Same shape as HdrToneMapper (gpu_hdr_tonemap.h): borrowed
// device/context, Init/Convert, lazy SRV/RTV caching. Uploads a
// RawDecodedVideoFrame's planes as textures and renders the matching
// conversion (+ HDR10 PQ tone-map when frame.is_pq_source) into dst.
//
// Threading: single-thread, like HdrToneMapper -- the caller's render thread
// owns every call.
class EditFrameGpuConverter {
  public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& err);

    // Uploads frame's planes (reusing/resizing cached per-plane textures only
    // when format or dimensions change from the previous call) and renders
    // the conversion into dst, a BGRA8 (DXGI_FORMAT_B8G8R8A8_UNORM) render
    // target of exactly frame.width x frame.height. hdr_peak_scale is the
    // display peak in reference-white multiples (HdrPeakScale()); ignored
    // unless frame.is_pq_source.
    bool Convert(const RawDecodedVideoFrame& frame, ID3D11Texture2D* dst, float hdr_peak_scale, std::string& err);

  private:
    // One uploaded source plane (Y, U or V). The textures are owned by this
    // class -- unlike HdrToneMapper's srv_cache_, which is keyed by an
    // externally-owned texture pointer -- so the cache key is the plane role
    // (the array index) plus the size/format the plane was last created with.
    struct PlaneTexture {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    // Recreates plane `index` only when the requested size/format differs from
    // what is cached, then uploads `rows` rows of `src` at `src_stride_bytes`.
    bool UploadPlane(int index, const uint8_t* src, UINT src_stride_bytes, UINT width, UINT height, DXGI_FORMAT format,
                     std::string& err);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    winrt::com_ptr<ID3D11VertexShader> vertex_shader_;
    winrt::com_ptr<ID3D11PixelShader> yuv_shader_; // SDR matrix/range conversion
    winrt::com_ptr<ID3D11PixelShader> pq_shader_;  // HDR10 PQ tone-map
    winrt::com_ptr<ID3D11Buffer> yuv_constants_;
    winrt::com_ptr<ID3D11Buffer> pq_constants_;

    PlaneTexture planes_[3]; // 0 = Y, 1 = U/Cb, 2 = V/Cr

    // Last values written into the constant buffers, so an unchanged clip does
    // not re-upload them every frame. Deliberately the *inputs*, not the
    // derived coefficients: comparing three enum/bool fields is cheaper and
    // cannot drift from the derivation.
    bool constants_valid_ = false;
    MatrixCoefficients last_matrix_ = MatrixCoefficients::Bt709;
    ColorRange last_range_ = ColorRange::Limited;
    DecodedPixelFormat last_format_ = DecodedPixelFormat::Yuv420P8;
    float last_peak_scale_ = 0.0f;
};

} // namespace exosnap::engine
