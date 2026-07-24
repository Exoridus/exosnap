#include "yuv_convert.h"

namespace recorder_core {

void ConvertI420ToNv12(const uint8_t* i420, uint32_t width, uint32_t height, std::vector<uint8_t>& out_nv12) {
    const size_t lumaSize = static_cast<size_t>(width) * height;
    const size_t chromaW = width / 2;
    const size_t chromaH = height / 2;
    const size_t chromaPlaneSize = chromaW * chromaH;

    out_nv12.resize(lumaSize + 2 * chromaPlaneSize);

    // Luma: identical layout in both formats.
    std::copy_n(i420, lumaSize, out_nv12.begin());

    const uint8_t* uPlane = i420 + lumaSize;
    const uint8_t* vPlane = uPlane + chromaPlaneSize;
    uint8_t* uv = out_nv12.data() + lumaSize;
    for (size_t i = 0; i < chromaPlaneSize; ++i) {
        uv[2 * i] = uPlane[i];
        uv[2 * i + 1] = vPlane[i];
    }
}

} // namespace recorder_core
