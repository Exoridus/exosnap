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

    static constexpr int kTitlebarHeight = 56;
    static constexpr int kControlHeight = 36;
    static constexpr int kPrimaryCtaHeight = 44;
};

} // namespace exosnap::ui::theme
