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

// Upper bound of decoded sources one composited frame may reference. Export
// planning rejects a timeline that needs more, so composition never discovers
// the limit halfway through an encode.
inline constexpr size_t kMaxTimelineVideoSources = 8;

// Upper bound of summed source pixels one composited frame may convert: four
// UHD frames. Every admitted source holds an FP16 linear surface of its own size.
inline constexpr uint64_t kMaxTimelineVideoSourcePixels = uint64_t{4} * 3840 * 2160;

struct TimelineVideoLayer {
    RawDecodedVideoFrame frame;
    // Track-local junction weight. Two sources of one layer are complementary:
    // their weights sum to one. A lone enabled endpoint may retain partial coverage.
    double weight = 1;
    // Clip opacity multiplied by the clip's edge-fade gain.
    double opacity = 1;
    // Composition order: a higher layer is drawn in front of every lower one.
    uint32_t layer = 0;
};

struct TimelineVideoFrame {
    int64_t time_us = 0;
    FrameSize output;
    std::vector<TimelineVideoLayer> layers;
};

// A single caller owns the context. Every source is converted to linear BT.709
// FP16 and contained independently. Each layer resolves its junction pair to one
// premultiplied color and coverage, then the layers are composited back to
// front with source-over into a linear accumulator that is finally encoded to
// BT.709 over black. Letterbox bands are transparent, so a lower layer shows
// through them. Textures and views are reused until source or output geometry
// changes.
class EditTimelineCompositor {
  public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, FrameSize output, std::string& error);
    // Rejects more than kMaxTimelineVideoSources sources, more than two sources
    // in one layer and noncomplementary pairs. A single source may have any
    // weight in [0, 1], retaining its coverage when its partner is disabled.
    // An empty span produces black.
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
    bool EnsureConverter(size_t index, std::string& error);
    void Draw(ID3D11PixelShader* shader, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* const* views,
              UINT view_count, ID3D11BlendState* blend);
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    FrameSize size_;
    std::array<std::unique_ptr<EditFrameGpuConverter>, kMaxTimelineVideoSources> converters_;
    std::array<Surface, kMaxTimelineVideoSources> sources_;
    Surface accumulator_;
    Surface output_;
    winrt::com_ptr<ID3D11VertexShader> vertex_;
    winrt::com_ptr<ID3D11PixelShader> layer_pixel_;
    winrt::com_ptr<ID3D11PixelShader> encode_pixel_;
    winrt::com_ptr<ID3D11Buffer> constants_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11BlendState> over_;
    uint64_t texture_creations_ = 0;
};

} // namespace exosnap::engine
