#pragma once

#include <d3d11_1.h>

namespace exosnap::engine {

inline void ConfigureSdrVideoProcessorColor(ID3D11VideoContext1* context, ID3D11VideoProcessor* processor,
                                            bool full_range) {
    context->VideoProcessorSetStreamColorSpace1(processor, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    context->VideoProcessorSetOutputColorSpace1(processor, full_range ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                                                                      : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
}

} // namespace exosnap::engine
