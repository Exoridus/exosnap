#pragma once

#include <array>
#include <string_view>

// The appearance model.
//
// Two base appearances and a small curated accent palette, chosen
// independently. It replaces four complete themes (`dark-default`,
// `dark-indigo`, `light-paper`, `light-slate`), each of which pinned one hue to
// one set of neutrals: choosing indigo meant also accepting a different
// background, and the two light themes existed mainly because neither could
// carry the other's accent. Splitting the two axes removes that coupling and
// leaves one Light base to actually get right instead of two to keep in step.
//
// The accents are deliberately all cool. Coral, amber and green are the
// product's semantic colours — recording/error, caution, ready/success — and an
// accent sharing one of those hues would make ordinary selection look like a
// state. Semantic meaning wins over palette breadth.

namespace exosnap::ui::theme {

// Identifies the appearance kind for alpha/tint derivation.
enum class ThemeKind { Dark, Light };

// One base appearance: every neutral and every semantic colour. Accent-free by
// construction, so an accent can never leak into a surface or a state colour.
struct ExoAppearance {
    const char* id;
    const char* name;
    ThemeKind kind;
    const char* intent; // short description, shown beside the option

    // Surfaces, from the page outwards. Four distinct rungs in both
    // appearances: an application background, a primary surface, a raised
    // control surface, and the hover/selected step.
    const char* bg;
    const char* surf;
    const char* surf2;
    const char* raise;

    // Lines (raw rgba strings so the alpha rides on whatever is behind them).
    const char* line;
    const char* line2;

    // Text, from primary to dimmest. `text1` is the secondary rung; it is
    // explicit rather than derived because both appearances want it tuned
    // rather than blended.
    const char* ink;
    const char* text1;
    const char* mut;
    const char* dim;

    // Semantic. Never derived from the accent — see the note above the
    // namespace.
    const char* success;
    const char* caution;
    const char* error;
    const char* error_ink; // ink for content drawn on an `error`-filled surface

    // The same three states, as READABLE TEXT.
    //
    // A semantic colour has two jobs that do not share a contrast bar. As a
    // ring, a dot or a tinted ground it is a graphical object and WCAG 1.4.11
    // asks 3:1 of it; as a word — a badge label, a severity glyph inside its
    // own tinted card — it is text and 1.4.3 asks 4.5:1. In Dark the three
    // values above clear both, so these repeat them. In Light they clear 3:1
    // and land at 3.3–3.9:1 as text, which is why a separate rung exists at
    // all: only the lightness moved, far enough to clear 4.5:1 on every
    // surface the product draws them on — including the tinted warning/error
    // grounds, which are the darkest of them and therefore the binding case.
    const char* success_text;
    const char* caution_text;
    const char* error_text;
};

// One curated accent, resolved per appearance. Two entries rather than one
// colour with a derivation rule: a hue that reads correctly at 60% lightness on
// a near-black surface is unreadable on a near-white one, and a rule that tried
// to bridge that produced a different hue in each appearance.
struct ExoAccent {
    const char* id;
    const char* name;
    const char* intent;
    const char* dark;     // the accent on the Dark appearance
    const char* dark_ink; // ink for content on a filled accent surface there
    const char* light;
    const char* light_ink;
};

inline constexpr std::array<ExoAppearance, 2> kExoAppearances = {{
    {
        "dark",
        "Dark",
        ThemeKind::Dark,
        "Calm graphite \xE2\x80\x94 the shipped default.",
        "#0E0E10",
        "#151517",
        "#1C1C1F",
        "#242428",
        "rgba(255, 255, 255, 0.07)",
        "rgba(255, 255, 255, 0.12)",
        "#F1F1EF",
        "#C5C5C3",
        "#9C9C9A",
        // Two steps up from the historical #65656A. `dim` is what an unavailable
        // control draws its icon in, and against the raised control surface
        // (#1C1C1F) the old value landed at 2.93:1 — just under the 3:1 the
        // contrast gate holds a non-text UI element to. The nudge clears it on
        // every surface it lands on without touching the dim/muted separation.
        "#67676C",
        "#84CBA2",
        "#E6C57C",
        "#E0786C",
        "#1A0D0B",
        // Dark needs no separate text rung: the three above measure 5.2:1 to
        // 11.6:1 as text on every surface, tinted grounds included.
        "#84CBA2",
        "#E6C57C",
        "#E0786C",
    },
    {
        // One Light base, rebuilt rather than inherited from either of the two
        // light themes it replaces. Both of those set `surf2` AND `raise` to
        // pure white, which collapsed the raised-control and hover rungs into
        // one and is why the light UI read as flat: a control, the card holding
        // it and the card's hover state were all the same colour.
        //
        // The four rungs below are distinct and none of them is pure white. The
        // page is a light cool neutral rather than paper, so a near-white
        // control has something to sit on; hover steps back DOWN towards the
        // page, which is the light-mode convention (there is no headroom above
        // white to step up into).
        "light",
        "Light",
        ThemeKind::Light,
        "Cool daylight \xE2\x80\x94 a grey ground with near-white surfaces on it.",
        // The ground is grey and the things ON it are near-white, which is the
        // arrangement a light UI needs to stop reading as one bright sheet. The
        // page carries most of the window's area and sat only two steps below
        // its cards, so the whole surface glared and the cards had nothing to
        // sit against. Dropping the ground rather than dimming the cards keeps
        // the content bright and takes the glare out of the frame around it.
        //
        // Separation comes from the value steps alone. No elevation shadows:
        // this is a tool surface, and a drop shadow under every card would be
        // decoration standing in for a hierarchy the steps already carry.
        "#DCE0E7",
        "#F4F5F8",
        "#FCFCFD",
        "#E8EAF0",
        "rgba(20, 26, 38, 0.12)",
        "rgba(20, 26, 38, 0.22)",
        "#171B24",
        "#3D4351",
        "#545A68",
        // `dim` is what an unavailable control draws its icon in, and it is
        // measured against the PAGE background -- so it follows the ground down
        // every time the ground moves. At the value it held before the ground
        // was darkened it lands at 2.97:1, just under the 3:1 the contrast gate
        // holds a non-text UI element to.
        "#6F7787",
        // success / caution darkened from #1E9E63 / #B5801C, and error from
        // #CE4B36. All three are drawn as rings, dots and pill grounds on the
        // page background, where they sat at 2.82 / 2.85 against the 3:1 bar;
        // error additionally carries white ink on a filled Stop pill, which was
        // 4.48 against the 4.5 text bar. Hue and saturation are unchanged — only
        // lightness moved, and only far enough to clear the bar with a margin.
        "#1C915B",
        "#A7761A",
        "#C94631",
        "#FFFFFF",
        // The text rung. Same hue and saturation as the three above; only the
        // lightness moved, and only as far as the binding case needs — the
        // tinted error ground (#E2CFCF), where each of these lands at 4.5:1
        // to 4.55:1. On the ordinary surfaces they sit at 5.5:1 to 6.7:1.
        "#146842",
        "#795513",
        "#A13827",
    },
}};

// Aqua MUST be first (kDefaultAccentId references it).
inline constexpr std::array<ExoAccent, 4> kExoAccents = {{
    {
        "aqua",
        "Aqua",
        "Studio mint \xE2\x80\x94 the ExoSnap default.",
        "#9BD9D2",
        "#08130F",
        "#127C74",
        "#FFFFFF",
    },
    {
        "sky",
        "Sky",
        "Petrol blue \xE2\x80\x94 cooler and quieter.",
        "#7FB7D9",
        "#06131C",
        "#18708F",
        "#FFFFFF",
    },
    {
        "violet",
        "Violet",
        "Periwinkle \xE2\x80\x94 more contrast against the neutrals.",
        "#B6A7E6",
        "#0E0A1E",
        "#6A4FC7",
        "#FFFFFF",
    },
    {
        "magenta",
        "Magenta",
        "Warm pink \xE2\x80\x94 the most assertive of the four.",
        "#E3A0CE",
        "#1C0A16",
        "#A63C7E",
        "#FFFFFF",
    },
}};

inline constexpr const char* kDefaultAppearanceId = "dark";
inline constexpr const char* kDefaultAccentId = "aqua";

// Pre-0.9 complete-theme ids, mapped to the closest (appearance, accent) pair.
//
// Mapped by the accent HUE the user actually saw, not by the theme's position
// in the old list — the two axes are independent now, so preserving the colour
// someone chose matters more than preserving which row it came from. That is
// why `light-paper` lands on Sky rather than on the default Aqua: its accent
// token was #18708F, petrol blue, which is exactly Sky's light value.
struct ExoThemeMigration {
    const char* legacy_theme_id;
    const char* appearance_id;
    const char* accent_id;
};

inline constexpr std::array<ExoThemeMigration, 4> kExoThemeMigrations = {{
    {"dark-default", "dark", "aqua"},
    {"dark-indigo", "dark", "violet"},
    {"light-paper", "light", "sky"},
    {"light-slate", "light", "violet"},
}};

// Anything unrecognised — an empty string, a hand-edited value, an id from a
// build that never existed — resolves to the shipped default. A settings store
// can therefore always be read into a valid pair, which is the property that
// keeps a damaged preference from producing an unstyled window.
[[nodiscard]] inline constexpr const char* MigratedAppearanceId(std::string_view legacy_theme_id) noexcept {
    for (const ExoThemeMigration& migration : kExoThemeMigrations) {
        if (legacy_theme_id == migration.legacy_theme_id) {
            return migration.appearance_id;
        }
    }
    return kDefaultAppearanceId;
}

[[nodiscard]] inline constexpr const char* MigratedAccentId(std::string_view legacy_theme_id) noexcept {
    for (const ExoThemeMigration& migration : kExoThemeMigrations) {
        if (legacy_theme_id == migration.legacy_theme_id) {
            return migration.accent_id;
        }
    }
    return kDefaultAccentId;
}

} // namespace exosnap::ui::theme
