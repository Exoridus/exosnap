#pragma once

#include <exosnap/engine/encoder_input_requirements.h>

namespace exosnap::engine {

[[nodiscard]] inline EncoderInputRequirements MakeNvencInputRequirements(VideoCodec codec, BitDepth depth,
                                                                         ChromaSubsampling chroma, FrameSize size,
                                                                         uint32_t slots, const void* device) noexcept {
    EncoderInputRequirements input;
    input.storage = EncoderInputStorage::D3D11Texture2D;
    if (depth == BitDepth::Bit8 && chroma == ChromaSubsampling::Cs420)
        input.format = EncoderInputFormat::Nv12;
    else if (depth == BitDepth::Bit10 && chroma == ChromaSubsampling::Cs420 && codec != VideoCodec::H264)
        input.format = EncoderInputFormat::P010;
    else if (depth == BitDepth::Bit8 && chroma == ChromaSubsampling::Cs444 && codec != VideoCodec::Av1)
        input.format = EncoderInputFormat::Ayuv;
    input.codec = codec;
    input.bit_depth = depth;
    input.chroma = chroma;
    input.allocation = size;
    input.visible = {0, 0, size.width, size.height};
    input.alignment = {2, 2};
    input.required_bind =
        chroma == ChromaSubsampling::Cs444 ? EncoderInputBind::UnorderedAccess : EncoderInputBind::RenderTarget;
    input.allowed_bind = input.required_bind | EncoderInputBind::ShaderResource;
    input.pool = {slots, slots, slots};
    input.device = device;
    return input;
}

} // namespace exosnap::engine
