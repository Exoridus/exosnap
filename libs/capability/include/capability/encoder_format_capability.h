#pragma once

#include "config_types.h"
#include "support_level.h"

namespace exosnap::capability {

// Unknown means no query ran; Unavailable means the query could not answer.
// Unsupported requires a successful negative query or an unimplemented path.
enum class EncoderFormatSupport { Unknown = 0, Unavailable = 1, Unsupported = 2, Supported = 3 };

// Evidence for the exact input paths implemented by the encoder. P010 also
// requires the 10-bit cap and Main10/Main profile; AYUV requires the 4:4:4 cap
// and High 4:4:4/FREXT profile. Codec advertisement alone proves none of these.
struct CodecFormatCapabilities {
    EncoderFormatSupport nv12 = EncoderFormatSupport::Unknown;
    EncoderFormatSupport p010 = EncoderFormatSupport::Unknown;
    EncoderFormatSupport ayuv = EncoderFormatSupport::Unknown;

    bool operator==(const CodecFormatCapabilities&) const noexcept = default;
};

[[nodiscard]] inline EncoderFormatSupport QueryEncoderFormatSupport(VideoCodec codec,
                                                                    const CodecFormatCapabilities& formats,
                                                                    ChromaSubsampling chroma, BitDepth depth) noexcept {
    if (chroma == ChromaSubsampling::Cs420) {
        if (depth == BitDepth::Bit8)
            return formats.nv12;
        if (depth == BitDepth::Bit10 && (codec == VideoCodec::Hevc || codec == VideoCodec::Av1))
            return formats.p010;
    } else if (chroma == ChromaSubsampling::Cs444 && depth == BitDepth::Bit8 &&
               (codec == VideoCodec::H264 || codec == VideoCodec::Hevc)) {
        return formats.ayuv;
    }
    return EncoderFormatSupport::Unsupported;
}

// Product annotations keep the existing UI support levels. The structured
// evidence above retains the distinction between unknown, unavailable and
// unsupported, while all three prevent selection in a device-scoped view.
[[nodiscard]] SupportAnnotation EncoderFormatAnnotation(EncoderFormatSupport support);

} // namespace exosnap::capability
