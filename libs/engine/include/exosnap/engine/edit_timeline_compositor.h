#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <exosnap/engine/edit_frame_gpu_converter.h>
#include <exosnap/engine/output_geometry.h>

#include <array>
#include <memory>
#include <span>

namespace exosnap::engine {

struct TimelineVideoLayer {
    RawDecodedVideoFrame frame;
    double weight = 1;
};

struct TimelineVideoFrame {
    int64_t time_us = 0;
    FrameSize output;
    std::vector<TimelineVideoLayer> layers;
};

// A single caller owns the context. Layers are converted to linear BT.709 FP16,
// contained independently, summed with evaluator weights and encoded to BT.709.
// Textures/views are reused until source or output geometry changes.
class EditTimelineCompositor {
  public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, FrameSize output, std::string& error);
    bool Compose(std::span<const TimelineVideoLayer> layers, std::string& error);
    [[nodiscard]] ID3D11Texture2D* Result() const {
        return output_.texture.get();
    }
    [[nodiscard]] uint64_t TextureCreations() const {
        return texture_creations_;
    }

  private:
    struct Surface {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11RenderTargetView> rtv;
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        FrameSize size;
    };
    bool EnsureSurface(Surface& surface, FrameSize size, DXGI_FORMAT format, std::string& error);
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    FrameSize size_;
    std::array<EditFrameGpuConverter, 2> converters_;
    std::array<Surface, 2> sources_;
    Surface output_;
    winrt::com_ptr<ID3D11VertexShader> vertex_;
    winrt::com_ptr<ID3D11PixelShader> pixel_;
    winrt::com_ptr<ID3D11Buffer> constants_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    uint64_t texture_creations_ = 0;
};

} // namespace exosnap::engine
