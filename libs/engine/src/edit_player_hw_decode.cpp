#include "edit_player_hw_decode.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace exosnap::engine {

bool IsSupportedHwReadbackFormat(int av_pix_fmt) noexcept {
    return av_pix_fmt == AV_PIX_FMT_NV12 || av_pix_fmt == AV_PIX_FMT_P010LE;
}

namespace {

// Splits one semi-planar 8-bit UV row (U0 V0 U1 V1 ...) into separate U/V
// rows. No rescale: NV12 samples are already plain 8-bit codes.
void DeinterleaveChromaRow8(const uint8_t* uv, uint8_t* u, uint8_t* v, int chroma_width) {
    for (int x = 0; x < chroma_width; ++x) {
        u[x] = uv[2 * x];
        v[x] = uv[2 * x + 1];
    }
}

// Splits one semi-planar P010 UV row into separate U/V rows, rescaling each
// 16-bit code from its <<6 left-justified range back to a plain 10-bit code.
void DeinterleaveChromaRow10(const uint8_t* uv_bytes, uint8_t* u_bytes, uint8_t* v_bytes, int chroma_width) {
    const auto* uv = reinterpret_cast<const uint16_t*>(uv_bytes);
    auto* u = reinterpret_cast<uint16_t*>(u_bytes);
    auto* v = reinterpret_cast<uint16_t*>(v_bytes);
    for (int x = 0; x < chroma_width; ++x) {
        u[x] = static_cast<uint16_t>(uv[2 * x] >> 6);
        v[x] = static_cast<uint16_t>(uv[2 * x + 1] >> 6);
    }
}

} // namespace

AVFrame* DeinterleaveHwReadbackFrame(const AVFrame* src) {
    if (src == nullptr || !IsSupportedHwReadbackFormat(src->format) || src->width <= 0 || src->height <= 0)
        return nullptr;

    const bool ten_bit = src->format == AV_PIX_FMT_P010LE;

    AVFrame* dst = av_frame_alloc();
    if (dst == nullptr)
        return nullptr;
    dst->format = ten_bit ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    dst->width = src->width;
    dst->height = src->height;
    if (av_frame_get_buffer(dst, 0) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }

    const int chroma_width = (src->width + 1) / 2;
    const int chroma_height = (src->height + 1) / 2;

    if (ten_bit) {
        // Luma is P010-left-justified too, not just chroma -- both need the
        // same >>6 rescale (see edit_player_hw_decode.h).
        for (int y = 0; y < src->height; ++y) {
            const auto* srow =
                reinterpret_cast<const uint16_t*>(src->data[0] + static_cast<size_t>(y) * src->linesize[0]);
            auto* drow = reinterpret_cast<uint16_t*>(dst->data[0] + static_cast<size_t>(y) * dst->linesize[0]);
            for (int x = 0; x < src->width; ++x)
                drow[x] = static_cast<uint16_t>(srow[x] >> 6);
        }
    } else {
        for (int y = 0; y < src->height; ++y)
            std::memcpy(dst->data[0] + static_cast<size_t>(y) * dst->linesize[0],
                        src->data[0] + static_cast<size_t>(y) * src->linesize[0], static_cast<size_t>(src->width));
    }

    for (int y = 0; y < chroma_height; ++y) {
        const uint8_t* uv_row = src->data[1] + static_cast<size_t>(y) * src->linesize[1];
        uint8_t* u_row = dst->data[1] + static_cast<size_t>(y) * dst->linesize[1];
        uint8_t* v_row = dst->data[2] + static_cast<size_t>(y) * dst->linesize[2];
        if (ten_bit)
            DeinterleaveChromaRow10(uv_row, u_row, v_row, chroma_width);
        else
            DeinterleaveChromaRow8(uv_row, u_row, v_row, chroma_width);
    }

    return dst;
}

} // namespace exosnap::engine
