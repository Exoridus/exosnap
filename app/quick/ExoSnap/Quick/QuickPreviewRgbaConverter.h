#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <string>

namespace exosnap::quick {

// Render-thread-only conversion used when the real capture texture is BGRA8
// or RGB10A2. Qt's public QSGD3D11Texture import describes native textures as
// RGBA8, so this keeps format adaptation on the GPU without Qt private APIs.
class QuickPreviewRgbaConverter {
  public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
                    ID3D11Texture2D* destination, UINT width, UINT height, std::string& error);
    bool convert(std::string& error);

  private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_view_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> destination_view_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace exosnap::quick
