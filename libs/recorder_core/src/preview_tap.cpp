#include <recorder_core/preview_tap.h>

#include "hdr_tonemap.h"

namespace recorder_core {

PreviewTapDesc ResolveRawCaptureTapDesc(DXGI_FORMAT format, bool display_hdr_active,
                                        float display_max_luminance_nits) noexcept {
    PreviewTapDesc desc;
    if (format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        return desc; // BGRA8 / 10 bpc SDR desktop: draw as-is
    }
    if (display_hdr_active) {
        desc.transform = PreviewTapTransform::ScrgbHdr;
        desc.peak_scale = HdrPeakScale(display_hdr_active, display_max_luminance_nits);
    } else {
        desc.transform = PreviewTapTransform::ScrgbSdr;
    }
    return desc;
}

} // namespace recorder_core
