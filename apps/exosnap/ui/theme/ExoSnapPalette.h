#pragma once

namespace exosnap::ui::theme {

struct ExoSnapPalette final {
    static constexpr const char* kBg0 = "#0E0C0A";
    static constexpr const char* kBg1 = "#141210";
    static constexpr const char* kBg2 = "#191714";
    static constexpr const char* kBg3 = "#211E1B";
    static constexpr const char* kBg4 = "#272420";

    static constexpr const char* kLine1 = "#272421";
    static constexpr const char* kLine2 = "#322E2B";
    static constexpr const char* kLine3 = "#4B4741";

    static constexpr const char* kText0 = "#EDE9E2";
    static constexpr const char* kText1 = "#AEA8A0";
    static constexpr const char* kText2 = "#7A756E";
    static constexpr const char* kText3 = "#54504B";

    static constexpr const char* kAccent = "#E9B361";
    static constexpr const char* kAccentDim = "rgba(233, 179, 97, 0.22)";
    static constexpr const char* kAccentLine = "rgba(233, 179, 97, 0.42)";
    static constexpr const char* kAccentHover = "#FBBE61";
    static constexpr const char* kAccentPressed = "#D39F54";

    static constexpr const char* kOk = "#78DA95";
    static constexpr const char* kWarn = "#C29653";
    static constexpr const char* kErr = "#F05B54";
    static constexpr const char* kInfo = "#78B9DA";
};

} // namespace exosnap::ui::theme
