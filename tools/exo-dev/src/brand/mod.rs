//! Shared parsing for the brand generators: `icons` and `marks` both read the
//! canonical geometry out of `app/assets/brand/marks/brand.svg` and
//! `app/ui/brand/BrandMark.h`, so this module is the one place that has to
//! agree with those two files' shape.
//!
//! Every parse function here panics on a source that does not have the shape
//! it expects, rather than returning an error the caller could paper over. A
//! renamed constant or a changed asset shape is a design change that has to
//! be looked at here, not one that silently produces a wrong icon.

#[cfg(feature = "dev-tools")]
pub mod icons;
pub mod marks;

use std::collections::HashMap;

use regex::Regex;

/// One `<circle>` of the canonical mark, as `brand.svg` states it.
#[derive(Debug, Clone)]
pub struct Circle {
    pub r: f64,
    pub stroke: Option<String>,
    pub stroke_width: f64,
    pub fill: String,
    pub opacity: f64,
}

/// The brand mark's three circles, in paint order.
///
/// Panics if `source` does not contain exactly three `<circle>` elements.
pub fn parse_mark_circles(source: &str) -> Vec<Circle> {
    let circle_re = Regex::new(r"<circle ([^/]*)/>").expect("valid regex");
    let attr_re = Regex::new(r#"([\w-]+)="([^"]+)""#).expect("valid regex");

    let circles: Vec<Circle> = circle_re
        .captures_iter(source)
        .map(|m| {
            let body = m.get(1).expect("group 1").as_str();
            let attrs: HashMap<String, String> = attr_re
                .captures_iter(body)
                .map(|a| (a[1].to_string(), a[2].to_string()))
                .collect();
            let r = attrs
                .get("r")
                .unwrap_or_else(|| panic!("brand.svg: <circle> without r: {body}"))
                .parse()
                .unwrap_or_else(|_| panic!("brand.svg: <circle> with a non-numeric r: {body}"));
            let stroke_width = attrs
                .get("stroke-width")
                .map(|v| v.parse().unwrap_or(0.0))
                .unwrap_or(0.0);
            let opacity = attrs
                .get("opacity")
                .map(|v| v.parse().unwrap_or(1.0))
                .unwrap_or(1.0);
            Circle {
                r,
                stroke: attrs.get("stroke").cloned(),
                stroke_width,
                fill: attrs
                    .get("fill")
                    .cloned()
                    .unwrap_or_else(|| "none".to_string()),
                opacity,
            }
        })
        .collect();

    assert_eq!(
        circles.len(),
        3,
        "brand.svg: expected 3 circles, found {}",
        circles.len()
    );
    circles
}

/// The multipliers a raster of `px` device pixels is drawn with. Mirrors
/// `OpticalProfileFor()` in `app/ui/brand/BrandMark.h`.
#[derive(Debug, Clone, Copy, Default)]
pub struct OpticalProfile {
    pub ring_stroke_scale: f64,
    pub outer_opacity_scale: f64,
    pub content_scale: f64,
}

/// The constants of `app/ui/brand/BrandMark.h`, as attributes.
#[derive(Debug, Clone)]
pub struct BrandMark {
    scalars: HashMap<String, f64>,
    profiles: HashMap<String, OpticalProfile>,
}

impl BrandMark {
    pub fn value(&self, name: &str) -> f64 {
        *self
            .scalars
            .get(name)
            .unwrap_or_else(|| panic!("BrandMark.h: missing constant {name}"))
    }

    pub fn profile(&self, name: &str) -> OpticalProfile {
        *self
            .profiles
            .get(name)
            .unwrap_or_else(|| panic!("BrandMark.h: missing optical profile {name}"))
    }

    /// Mirror of `OpticalProfileFor()`: the one rule this parser implements
    /// rather than reads, because it is control flow and not a number.
    pub fn profile_for(&self, px: i32) -> OpticalProfile {
        if (px as f64) <= self.value("kSmallProfileMaxPx") {
            self.profile("kSmallProfile")
        } else if (px as f64) <= self.value("kMediumProfileMaxPx") {
            self.profile("kMediumProfile")
        } else {
            self.profile("kLargeProfile")
        }
    }
}

/// The constants of `app/ui/brand/BrandMark.h`, as attributes.
///
/// Panics if no scalar constant and no optical profile could be parsed at
/// all, meaning the header's shape has changed underneath this parser.
pub fn parse_brand_mark(source: &str) -> BrandMark {
    let scalar_re = Regex::new(r"(?m)^inline constexpr (?:double|int) (k\w+) = ([-\d.]+);")
        .expect("valid regex");
    let profile_re =
        Regex::new(r"(?s)inline constexpr OpticalProfile (k\w+)\{(.*?)\};").expect("valid regex");
    let field_re = Regex::new(r"\.(\w+)\s*=\s*([-\d.]+)").expect("valid regex");

    let scalars: HashMap<String, f64> = scalar_re
        .captures_iter(source)
        .map(|m| (m[1].to_string(), m[2].parse().expect("numeric constant")))
        .collect();

    let profiles: HashMap<String, OpticalProfile> = profile_re
        .captures_iter(source)
        .map(|m| {
            let name = m[1].to_string();
            let body = &m[2];
            let fields: HashMap<String, f64> = field_re
                .captures_iter(body)
                .map(|f| (f[1].to_string(), f[2].parse().expect("numeric field")))
                .collect();
            let profile = OpticalProfile {
                ring_stroke_scale: *fields
                    .get("ring_stroke_scale")
                    .unwrap_or_else(|| panic!("BrandMark.h: {name} is missing ring_stroke_scale")),
                outer_opacity_scale: *fields.get("outer_opacity_scale").unwrap_or_else(|| {
                    panic!("BrandMark.h: {name} is missing outer_opacity_scale")
                }),
                content_scale: *fields
                    .get("content_scale")
                    .unwrap_or_else(|| panic!("BrandMark.h: {name} is missing content_scale")),
            };
            (name, profile)
        })
        .collect();

    assert!(
        !scalars.is_empty() && !profiles.is_empty(),
        "BrandMark.h: no constants parsed, has the header's shape changed?"
    );
    BrandMark { scalars, profiles }
}

/// One colour out of `ExoSnapThemes.h`'s appearance table, by position.
///
/// The table is a C++ aggregate, so the fields are positional: `index`
/// counts the quoted colour values after the appearance id. Fragile enough
/// to be worth the panic below, and still better than a second copy of the
/// palette.
pub fn parse_theme_colour(source: &str, appearance_id: &str, index: usize) -> String {
    let marker = format!("\"{appearance_id}\",");
    let start = source
        .find(&marker)
        .unwrap_or_else(|| panic!("ExoSnapThemes.h: appearance '{appearance_id}' not found"));
    let colour_re = Regex::new(r#""(#[0-9A-Fa-f]{6}|rgba\([^"]*\))""#).expect("valid regex");
    colour_re
        .captures_iter(&source[start..])
        .nth(index)
        .unwrap_or_else(|| {
            panic!("ExoSnapThemes.h: appearance '{appearance_id}' has no colour at index {index}")
        })[1]
        .to_string()
}

/// A number as an authored SVG carries it: rounded to `decimals` places, then
/// trimmed of trailing zeroes so a value that is exactly one reads as `1`.
///
/// Every value this crate formats stays well under 1e6 (the design grid is
/// 32 units), so plain fixed-point rounding and trimming reproduces the
/// generated marks' `%g`-style formatting exactly, without needing general
/// scientific-notation fallback.
pub(crate) fn num(value: f64, decimals: i32) -> String {
    let text = format!("{:.*}", decimals.max(0) as usize, value);
    if !text.contains('.') {
        return text;
    }
    let trimmed = text.trim_end_matches('0');
    let trimmed = trimmed.trim_end_matches('.');
    if trimmed.is_empty() || trimmed == "-" {
        "0".to_string()
    } else {
        trimmed.to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // A real excerpt of app/ui/brand/BrandMark.h, trimmed to the constants
    // these parsers read. Copied rather than read from the live header, so
    // this test does not drift out from under a later geometry edit.
    const BRAND_MARK_EXCERPT: &str = r##"
inline constexpr double kGrid = 32.0;
inline constexpr double kCenter = 16.0;

inline constexpr char kReferenceAccent[] = "#9BD9D2";

inline constexpr int kSmallProfileMaxPx = 20;
inline constexpr int kMediumProfileMaxPx = 48;

inline constexpr OpticalProfile kSmallProfile{
    .ring_stroke_scale = 1.25,
    .outer_opacity_scale = 1.30,
    .content_scale = 1.06,
};

inline constexpr OpticalProfile kMediumProfile{
    .ring_stroke_scale = 1.10,
    .outer_opacity_scale = 1.12,
    .content_scale = 1.03,
};

inline constexpr OpticalProfile kLargeProfile{
    .ring_stroke_scale = 1.0,
    .outer_opacity_scale = 1.0,
    .content_scale = 1.0,
};

inline constexpr double kStandaloneContentScale = 0.88;

inline constexpr double kGlyphDiscRadius = 7.0;
"##;

    const THEMES_EXCERPT: &str = r##"
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
        "#67676C",
        "#84CBA2",
        "#E6C57C",
        "#E0786C",
        "#1A0D0B",
        "#84CBA2",
        "#E6C57C",
        "#E0786C",
    },
    {
        "light",
        "Light",
        ThemeKind::Light,
        "Cool daylight \xE2\x80\x94 a grey ground with near-white surfaces on it.",
        "#DCE0E7",
        "#F4F5F8",
        "#FCFCFD",
        "#E8EAF0",
        "rgba(20, 26, 38, 0.12)",
        "rgba(20, 26, 38, 0.22)",
        "#171B24",
        "#3D4351",
        "#545A68",
        "#6F7787",
        "#1C915B",
        "#A7761A",
        "#C94631",
        "#FFFFFF",
        "#146842",
        "#795513",
        "#A13827",
    },
}};
"##;

    #[test]
    fn parse_mark_circles_reads_the_three_circles_in_paint_order() {
        let svg = r##"<svg><circle cx="16" cy="16" r="13.7" fill="none" stroke="#9BD9D2" stroke-width="1.75" opacity="0.64"/><circle cx="16" cy="16" r="7.4" fill="none" stroke="#9BD9D2" stroke-width="1.4"/><circle cx="16" cy="16" r="2.75" fill="#E0786C"/></svg>"##;
        let circles = parse_mark_circles(svg);
        assert_eq!(circles.len(), 3);
        assert_eq!(circles[0].r, 13.7);
        assert_eq!(circles[0].stroke.as_deref(), Some("#9BD9D2"));
        assert_eq!(circles[0].opacity, 0.64);
        assert_eq!(circles[1].r, 7.4);
        assert_eq!(circles[2].r, 2.75);
        assert_eq!(circles[2].fill, "#E0786C");
        assert!(circles[2].stroke.is_none());
    }

    #[test]
    #[should_panic(expected = "expected 3 circles")]
    fn parse_mark_circles_panics_on_the_wrong_shape() {
        let svg = r#"<svg><circle cx="16" cy="16" r="13.7"/></svg>"#;
        parse_mark_circles(svg);
    }

    #[test]
    fn parse_brand_mark_reads_scalars_and_profiles() {
        let mark = parse_brand_mark(BRAND_MARK_EXCERPT);
        assert_eq!(mark.value("kGrid"), 32.0);
        assert_eq!(mark.value("kCenter"), 16.0);
        assert_eq!(mark.value("kStandaloneContentScale"), 0.88);
        assert_eq!(mark.value("kGlyphDiscRadius"), 7.0);

        let small = mark.profile("kSmallProfile");
        assert_eq!(small.ring_stroke_scale, 1.25);
        assert_eq!(small.outer_opacity_scale, 1.30);
        assert_eq!(small.content_scale, 1.06);
    }

    #[test]
    fn parse_brand_mark_profile_for_resolves_by_size() {
        let mark = parse_brand_mark(BRAND_MARK_EXCERPT);
        assert_eq!(mark.profile_for(16).content_scale, 1.06);
        assert_eq!(mark.profile_for(20).content_scale, 1.06);
        assert_eq!(mark.profile_for(32).content_scale, 1.03);
        assert_eq!(mark.profile_for(48).content_scale, 1.03);
        assert_eq!(mark.profile_for(256).content_scale, 1.0);
    }

    #[test]
    #[should_panic(expected = "missing constant")]
    fn parse_brand_mark_value_panics_on_an_unknown_constant() {
        let mark = parse_brand_mark(BRAND_MARK_EXCERPT);
        mark.value("kNeverExisted");
    }

    #[test]
    fn parse_theme_colour_reads_by_position() {
        // Position 0 is the first colour column (bg); 10, 11 and 12 are
        // success, caution and error, matching the header's own count of
        // "bg surf surf2 raise line line2 ink text1 mut dim, then the three
        // semantic ones".
        assert_eq!(parse_theme_colour(THEMES_EXCERPT, "dark", 0), "#0E0E10");
        assert_eq!(parse_theme_colour(THEMES_EXCERPT, "dark", 10), "#84CBA2");
        assert_eq!(parse_theme_colour(THEMES_EXCERPT, "dark", 11), "#E6C57C");
        assert_eq!(parse_theme_colour(THEMES_EXCERPT, "dark", 12), "#E0786C");
        assert_eq!(parse_theme_colour(THEMES_EXCERPT, "light", 12), "#C94631");
    }

    #[test]
    #[should_panic(expected = "not found")]
    fn parse_theme_colour_panics_on_an_unknown_appearance() {
        parse_theme_colour(THEMES_EXCERPT, "midnight", 0);
    }

    #[test]
    fn num_trims_trailing_zeroes() {
        assert_eq!(num(13.7, 2), "13.7");
        assert_eq!(num(1.0, 2), "1");
        assert_eq!(num(0.640, 2), "0.64");
        assert_eq!(num(0.0, 2), "0");
    }
}
