#include "diagnostics/PresentModeMapping.h"

namespace exosnap::diagnostics {

namespace {
// PresentMon PresentData PresentMode enum (VERIFIED, PresentData/PresentMonTraceConsumer.hpp@v1.10.0):
//   1 Hardware_Legacy_Flip, 2 Hardware_Legacy_Copy_To_Front_Buffer       -> exclusive fullscreen
//   3 Hardware_Independent_Flip, 8 Hardware_Composed_Independent_Flip     -> independent flip
//   4 Composed_Flip, 5 Composed_Copy_GPU_GDI, 6 Composed_Copy_CPU_GDI     -> composed
//   (there is no code 7; 0 == Unknown)
PresentMode ClassifyMode(int code) {
    switch (code) {
    case 1:
    case 2:
        return PresentMode::ExclusiveFullscreen;
    case 3:
    case 8:
        return PresentMode::IndependentFlip;
    case 4:
    case 5:
    case 6:
        return PresentMode::Composed;
    default:
        return PresentMode::Unknown;
    }
}
} // namespace

PresentSample MapPresentEvent(const RawPresentEvent& ev) {
    PresentSample s;
    if (!ev.valid) {
        return s; // available == false, mode == Unknown
    }
    s.available = true;
    s.mode = ClassifyMode(ev.present_mode_code);
    // Tearing iff the producer flagged it OR it presented uncapped (sync interval 0).
    s.tearing = ev.tearing_flag || ev.sync_interval == 0;
    s.present_interval_ms = ev.interval_ms;
    return s;
}

} // namespace exosnap::diagnostics
