#include <exosnap/engine/preview_tap.h>

#include "hdr_tonemap.h"

namespace exosnap::engine {

PreviewTapDesc ResolveRawCaptureTapDesc(DXGI_FORMAT format, bool display_hdr_active, float sdr_white_level_nits,
                                        float display_max_luminance_nits) noexcept {
    PreviewTapDesc desc;
    if (format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        return desc; // BGRA8 / 10 bpc SDR desktop: draw as-is
    }
    if (display_hdr_active) {
        desc.transform = PreviewTapTransform::ScrgbHdr;
        desc.peak_scale = HdrPeakScale(display_hdr_active, display_max_luminance_nits);
        desc.paper_white_scale = SdrPaperWhiteScale(sdr_white_level_nits);
    } else {
        desc.transform = PreviewTapTransform::ScrgbSdr;
    }
    return desc;
}

} // namespace exosnap::engine
