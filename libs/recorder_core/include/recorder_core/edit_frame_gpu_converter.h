#pragma once

#include <recorder_core/edit_player_engine.h> // RawDecodedVideoFrame

#include <d3d11.h>
#include <winrt/base.h>

#include <string>
#include <unordered_map>

namespace recorder_core {

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
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    // Task 2 fills in: shaders, per-plane textures/SRVs, sampler, constant
    // buffers. Left undeclared here on purpose -- Task 2 owns this file's
    // private section entirely; nothing outside this class reaches into it.
};

} // namespace recorder_core
