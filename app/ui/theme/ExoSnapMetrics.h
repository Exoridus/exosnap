#pragma once

namespace exosnap::ui::theme {

struct ExoSnapMetrics final {
    static constexpr int kSpaceXs = 4;
    static constexpr int kSpaceSm = 8;
    static constexpr int kSpaceMd = 14;
    static constexpr int kSpaceLg = 16;
    static constexpr int kSpaceXl = 24;
    static constexpr int kSpace2xl = 32;

    // Hybrid v3 radius scale (softer corners; selects ~9-10, cards ~14).
    static constexpr int kRadiusSm = 8;
    static constexpr int kRadiusMd = 10;
    static constexpr int kRadiusLg = 14;

    // 40 px, between the Windows 11 caption height (32) and Chrome's tab strip.
    // 32 would fit the six nav tabs, wordmark, status pill and bell with no
    // reserve at all — one longer status label overflows it.
    static constexpr int kTitlebarHeight = 40;
    static constexpr int kControlHeight = 36;
    static constexpr int kPrimaryCtaHeight = 44;

    // Hard minimum for the main window. Every layout breakpoint in the product
    // is designed against it, and the geometry restore clamps to it — so the
    // window class and the clamp have to read the same number rather than each
    // carrying its own literal.
    static constexpr int kMinWindowWidth = 860;
    static constexpr int kMinWindowHeight = 700;

    // What the window opens at when nothing has been persisted yet — a
    // PREFERRED size, not a forced one: it is centred on the primary screen and
    // then clamped against that screen's work area, and any valid persisted
    // rect wins over it outright.
    //
    // 1280x720 rather than the previous 1040x760: the product is preview-first,
    // and 1040 gave a 16:9 preview roughly 990 px wide inside a window that was
    // taller than it was useful. The same 16:9 subject gets ~1230 px here, and
    // 720 still leaves the three Record bands their full rhythm above the
    // 700 px minimum. It is also the one window size a user can name, which
    // matters when a support report says "at default size".
    static constexpr int kPreferredWindowWidth = 1280;
    static constexpr int kPreferredWindowHeight = 720;
};

} // namespace exosnap::ui::theme
