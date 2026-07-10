#pragma once

// RGB -> packed AYUV (4:4:4, 8-bit) conversion via a D3D11 compute shader.
//
// The D3D11 VideoProcessor cannot emit a 4:4:4 surface, so the expert 4:4:4
// H.264/HEVC encode path converts the geometry-corrected RGB intermediate
// (produced by the VideoProcessor as full-range BGRA8) into the packed AYUV
// surface NVENC consumes for YUV444 encoding (NV_ENC_BUFFER_FORMAT_AYUV).
//
// AYUV byte layout (NVENC "8 bit Packed A8Y8U8V8", word-ordered): each pixel is
// a 32-bit little-endian word with V (Cr) in the lowest 8 bits, U (Cb) in the
// next 8, Y in the next 8, and A in the highest 8. In memory that is the byte
// sequence [V, U, Y, A]. The compute shader writes it through a
// DXGI_FORMAT_R8G8B8A8_UINT unordered-access view (the documented cast-compatible
// view format for a DXGI_FORMAT_AYUV resource), storing uint4(V, U, Y, 255) so
// the channel order maps exactly onto that byte sequence.
//
// The colour matrix is BT.709; the quantization range (limited/studio 16-235 or
// full 0-255) follows the session ColorMetadata, matching what the 4:2:0
// VideoProcessor path is configured to produce so the two chroma modes agree.
// Only ALU (no driver intrinsics) is used, so WARP and real GPUs agree, which is
// what the golden tests pin.

#include <cstdint>
#include <string>
#include <unordered_map>

#include <d3d11.h>
#include <winrt/base.h>

namespace recorder_core {

class RgbToAyuvConverter {
  public:
    RgbToAyuvConverter() = default;

    RgbToAyuvConverter(const RgbToAyuvConverter&) = delete;
    RgbToAyuvConverter& operator=(const RgbToAyuvConverter&) = delete;

    // Compile the compute shader and allocate the constant buffer.
    // full_range selects the quantization range: false = limited/studio (16-235
    // luma, the encode default), true = full (0-255).
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height, bool full_range,
              std::string& err);

    // Convert one full-range BGRA8 source frame into a packed AYUV destination.
    // src must be an 8-bit RGBA/BGRA shader-resource texture; dst must be a
    // DXGI_FORMAT_AYUV (or cast-compatible R8G8B8A8) texture with
    // D3D11_BIND_UNORDERED_ACCESS. Both must match the Init() dimensions.
    bool Convert(ID3D11Texture2D* src, ID3D11Texture2D* dst, std::string& err);

  private:
    ID3D11ShaderResourceView* SrvFor(ID3D11Texture2D* tex, std::string& err);
    ID3D11UnorderedAccessView* UavFor(ID3D11Texture2D* tex, std::string& err);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;

    winrt::com_ptr<ID3D11ComputeShader> shader_;
    winrt::com_ptr<ID3D11Buffer> constants_;

    std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11ShaderResourceView>> srv_cache_;
    std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11UnorderedAccessView>> uav_cache_;
};

} // namespace recorder_core
