#pragma once

#include <array>

namespace exosnap::ui::theme {

// Identifies the theme kind (dark vs light) for alpha derivation.
enum class ThemeKind { Dark, Light };

// Log palette per theme.
struct ExoLogPalette {
    const char* cat;
    const char* info;
    const char* warn;
    const char* error;
    const char* debug;
    const char* time;
};

// Complete colour token set for one theme.
// Optional overrides: bg4_override, line3_override, text1_override are used when
// the derived value would differ from the authoritative palette (dark-default).
// Set to nullptr to use derived value.
struct ExoTheme {
    const char* id;
    const char* name;
    const char* group; // "Dark" or "Light"
    ThemeKind kind;
    const char* intent; // short description

    // Surfaces
    const char* bg;
    const char* surf;
    const char* surf2;
    const char* raise;

    // Lines (these are raw rgba strings, not derived)
    const char* line;
    const char* line2;

    // Text
    const char* ink;
    const char* mut;
    const char* dim;

    // Primary accent
    const char* ac;
    const char* ac_ink;

    // Secondary accent
    const char* ac2;
    const char* ac2_ink;

    // Semantic
    const char* success;
    const char* caution;
    const char* error;
    const char* error_ink; // text/icon color for content drawn on an `error`-filled
                           // surface (mirrors ac_ink's contrast role for the accent fill)

    // Log palette
    ExoLogPalette log;

    // Optional explicit overrides for derived tokens (nullptr = use derived)
    const char* bg4_override = nullptr;         // dark: Lighten(raise,0.10); light: Darken(raise,0.04)
    const char* line3_override = nullptr;       // dark: rgba(255,255,255,0.20); light: rgba(ink,0.24)
    const char* text1_override = nullptr;       // dark-default: #C5C5C3; else: blend(ink,mut,0.42)
    const char* error_hover_override = nullptr; // dark themes: the historical stop-hover coral;
                                                // nullptr elsewhere derives Lighten(error, 0.14)
};

// The canonical theme table. dark-default MUST be first (kDefaultThemeId references it).
inline constexpr std::array<ExoTheme, 4> kExoThemes = {{
    {
        "dark-default",
        "Dark \xC2\xB7 Default",
        "Dark",
        ThemeKind::Dark,
        "The frozen Studio-Mint base \xE2\x80\x94 calm graphite, mint primary.",
        "#0E0E10",
        "#151517",
        "#1C1C1F",
        "#242428",
        "rgba(255, 255, 255, 0.07)",
        "rgba(255, 255, 255, 0.12)",
        "#F1F1EF",
        "#9C9C9A",
        "#65656A",
        "#9BD9D2",
        "#08130F",
        "#B6A7E6",
        "#0E0A1E",
        "#84CBA2",
        "#E6C57C",
        "#E0786C",
        "#1A0D0B", // error_ink (exact value kept from the historical hardcoded QSS)
        {"#7FB7D9", "#9C9C9A", "#E6C57C", "#E0786C", "#65656A", "#65656A"},
        "#2C2C31",                   // bg4_override (exact palette value kBg4)
        "rgba(255, 255, 255, 0.20)", // line3_override (exact palette value kLine3)
        "#C5C5C3",                   // text1_override (exact palette value kText1)
        "#EC8A7E",                   // error_hover_override (exact value kept from the historical hardcoded QSS)
    },
    {
        "dark-indigo",
        "Dark \xC2\xB7 Indigo",
        "Dark",
        ThemeKind::Dark,
        "Cooler and more focused \xE2\x80\x94 periwinkle indigo on blue-black.",
        "#0B0C10",
        "#14151B",
        "#1B1D25",
        "#25272F",
        "rgba(255, 255, 255, 0.07)",
        "rgba(255, 255, 255, 0.12)",
        "#F0F1F4",
        "#989AA4",
        "#62646E",
        "#9DB0F5",
        "#0A0F22",
        "#E0B486",
        "#1E1206",
        "#84CBA2",
        "#E6C57C",
        "#E0786C",
        "#1A0D0B", // error_ink (same error color as dark-default, same required ink)
        {"#5FBCA8", "#989AA4", "#E6C57C", "#E0786C", "#62646E", "#62646E"},
        nullptr,
        nullptr,
        nullptr,
        "#EC8A7E", // error_hover_override (same error color and historical hover as dark-default)
    },
    {
        "light-paper",
        "Light \xC2\xB7 Paper",
        "Light",
        ThemeKind::Light,
        "Warm daylight \xE2\x80\x94 ink on paper, petrol-blue primary.",
        "#EFEDE7",
        "#F9F8F4",
        "#FFFFFF",
        "#FFFFFF",
        "rgba(28, 24, 16, 0.10)",
        "rgba(28, 24, 16, 0.17)",
        "#20201C",
        "#5E5C54",
        "#918E83",
        "#18708F",
        "#FFFFFF",
        "#5A52C7",
        "#FFFFFF",
        "#1E9E63",
        "#B5801C",
        "#CE4B36",
        "#FFFFFF", // error_ink (darker saturated red fill needs light ink, like this theme's ac_ink)
        {"#1F6FB8", "#5E5C54", "#A6731A", "#C0432F", "#918E83", "#918E83"},
        nullptr,
        nullptr,
        nullptr,
        nullptr, // error_hover_override: derive Lighten(error, 0.14)
    },
    {
        "light-slate",
        "Light \xC2\xB7 Slate",
        "Light",
        ThemeKind::Light,
        "Cool daylight \xE2\x80\x94 neutral grey-blue with an indigo primary.",
        "#ECEEF1",
        "#F7F8FA",
        "#FFFFFF",
        "#FFFFFF",
        "rgba(15, 20, 30, 0.10)",
        "rgba(15, 20, 30, 0.17)",
        "#161A22",
        "#565C68",
        "#888F9C",
        "#3B5BD4",
        "#FFFFFF",
        "#0E8A7E",
        "#FFFFFF",
        "#1E9E63",
        "#B5801C",
        "#CE4B36",
        "#FFFFFF", // error_ink (darker saturated red fill needs light ink, like this theme's ac_ink)
        {"#0E7C86", "#565C68", "#A6731A", "#C0432F", "#888F9C", "#888F9C"},
        nullptr,
        nullptr,
        nullptr,
        nullptr, // error_hover_override: derive Lighten(error, 0.14)
    },
}};

// The default theme id — must match kExoThemes[0].id.
inline constexpr const char* kDefaultThemeId = "dark-default";

} // namespace exosnap::ui::theme
