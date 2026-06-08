#pragma once

#include <array>

namespace exosnap::ui::theme {

// Curated Hybrid v3 accent variants.
//
// Scope note (HYBRID-PORT-R1A): this table is data only. The first Qt port uses the default
// Studio Mint accent (see kDefaultAccentId / ExoSnapPalette::kAccent). A user-facing accent
// switcher / Tweaks panel is intentionally out of scope for this slice; this structure exists
// so a later phase can recolor the app from a single source without re-deriving the palette.
struct ExoSnapAccent final {
    const char* id;   // stable identifier
    const char* name; // display name
    const char* base; // accent fill color
    const char* ink;  // dark ink for text rendered on top of the accent fill
};

inline constexpr std::array<ExoSnapAccent, 7> kExoSnapAccents = {{
    {"mint", "Studio Mint", "#9BD9D2", "#08130F"},
    {"amber", "Warm Amber", "#E2B473", "#1A1206"},
    {"coral", "Soft Coral", "#F2B3A2", "#1E0D08"},
    {"azure", "Electric Azure", "#63B5F2", "#04121E"},
    {"violet", "Violet", "#AB9DF2", "#0E0A1E"},
    {"lime", "Signal Lime", "#BFE36E", "#121A06"},
    {"graphite", "Graphite", "#CBCFCC", "#0E1110"},
}};

// Default accent for the first Qt port. Must stay in sync with ExoSnapPalette::kAccent.
inline constexpr const char* kDefaultAccentId = "mint";

} // namespace exosnap::ui::theme
