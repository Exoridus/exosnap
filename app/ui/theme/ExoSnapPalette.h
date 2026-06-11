#pragma once

namespace exosnap::ui::theme {

// ExoSnap Hybrid v3 palette — Studio calm dark base with a single Studio Mint accent.
// Token names are intentionally preserved (kBg0..kBg4, kLine1..kLine3, kText0..kText3,
// kAccent*, kOk/kWarn/kErr/kInfo) so the existing token-driven QSS keeps resolving without
// a structural rewrite. Only the values change to the Hybrid roles below.
//
// Hybrid role mapping:
//   bg     -> kBg0   surface     -> kBg1   raised surface -> kBg2 / kBg3
//   line   -> kLine1 line2       -> kLine2
//   ink    -> kText0 muted       -> kText2 dim            -> kText3
//   accent -> kAccent (Studio Mint), accent ink -> kAccentInk
//   ready  -> kOk    warning     -> kWarn  recording/coral-> kErr
struct ExoSnapPalette final {
    // Backgrounds / surfaces (neutral, cool-dark).
    static constexpr const char* kBg0 = "#0E0E10"; // app background (bg)
    static constexpr const char* kBg1 = "#151517"; // surface (titlebar, sidebar)
    static constexpr const char* kBg2 = "#1C1C1F"; // raised surface (cards, panels)
    static constexpr const char* kBg3 = "#242428"; // raise (inputs, buttons)
    static constexpr const char* kBg4 = "#2C2C31"; // raise hover / pressed

    // Hairlines / borders (white alpha so they read on every surface).
    static constexpr const char* kLine1 = "rgba(255, 255, 255, 0.07)"; // line
    static constexpr const char* kLine2 = "rgba(255, 255, 255, 0.12)"; // line2
    static constexpr const char* kLine3 = "rgba(255, 255, 255, 0.20)"; // stronger hover border

    // Text ramp.
    static constexpr const char* kText0 = "#F1F1EF"; // ink (primary)
    static constexpr const char* kText1 = "#C5C5C3"; // body / secondary
    static constexpr const char* kText2 = "#9C9C9A"; // muted
    static constexpr const char* kText3 = "#65656A"; // dim / disabled

    // Accent — Studio Mint, FIXED for 1.0 (D1). Derived alphas are frozen to the
    // design's generated set: dim 0.14 · soft 0.24 · b1 0.42 · b2 0.60. Keep all
    // accent usage token-driven (no mint literals at call sites) so the deferred
    // 1.1 accent picker stays a config feature, not a refactor.
    static constexpr const char* kAccent = "#9BD9D2";
    static constexpr const char* kAccentInk = "#08130F"; // dark ink for text on accent fill
    static constexpr const char* kAccentDim = "rgba(155, 217, 210, 0.14)";
    static constexpr const char* kAccentSoft = "rgba(155, 217, 210, 0.24)";
    static constexpr const char* kAccentLine = "rgba(155, 217, 210, 0.42)";         // b1
    static constexpr const char* kAccentBorderStrong = "rgba(155, 217, 210, 0.60)"; // b2
    static constexpr const char* kAccentHover = "#B4E4DE";
    static constexpr const char* kAccentPressed = "#84C7C0";

    // Semantic colors (always distinct from the primary accent).
    static constexpr const char* kOk = "#84CBA2";   // ready / healthy
    static constexpr const char* kWarn = "#E6C57C"; // warning / paused
    static constexpr const char* kErr = "#E0786C";  // recording / stop / error (coral)
    static constexpr const char* kInfo = "#7FBEE8"; // informational (azure)
};

} // namespace exosnap::ui::theme
