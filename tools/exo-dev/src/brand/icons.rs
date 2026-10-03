//! Generates ExoSnap's build-time brand artefacts: the application `.ico`,
//! the thumbnail-toolbar glyph `.ico` files, and `exosnap-logo.svg`.
//!
//! The state marks (recording/paused/saved/warning/error, the two
//! animations) are NOT here: their outer ring is the user's accent and
//! their inner ring is the session's semantic colour, so as files they
//! would be one icon per (state x accent x appearance x heartbeat frame).
//! Both surfaces that show one get it painted at runtime by
//! `app/ui/brand/ShellIconRenderer`.
//!
//! The mark's geometry lives in `app/assets/brand/marks/brand.svg`
//! (written by [`super::marks`] from `parameters.json`). The optical
//! corrections and the thumbnail glyph shapes live in
//! `app/ui/brand/BrandMark.h`, because they are a property of the raster
//! rather than of the drawing. This module parses both; it restates
//! neither, so a renamed constant or a changed asset shape fails here
//! rather than silently producing an icon that no longer matches the
//! application.

use std::path::Path;

use anyhow::Context as _;

use super::{BrandMark, Circle, parse_brand_mark, parse_mark_circles, parse_theme_colour};

/// Windows shell scale factors are 100/125/150/200/250/300/400%, applied to
/// the 16, 32 and 48 px base metrics; 256 is the jumbo view. This set is the
/// union of what those produce, minus the sizes no context selects exactly:
/// anything not listed is downscaled by the shell from the next one up.
const APP_ICO_SIZES: [u32; 13] = [16, 20, 24, 32, 40, 48, 60, 64, 72, 80, 96, 128, 256];

/// Thumbnail glyphs are shell chrome at 16-48 px. The large frames an
/// application icon needs would only bloat the executable.
const SHELL_ICO_SIZES: [u32; 6] = [16, 20, 24, 32, 40, 48];

/// Above this, a frame is stored as PNG; at or below it, as an
/// uncompressed DIB. The shell surfaces that decode small PNG frames badly
/// are the small ones, and a DIB costs `width * height * 4` bytes, so
/// storing the whole large end of the icon as DIB would waste a quarter
/// of a megabyte in the executable for no benefit.
const PNG_FRAME_MIN_PX: u32 = 64;

/// Positions in `ExoAppearance`'s colour-valued fields, counting from the
/// appearance id: bg surf surf2 raise line line2 ink text1 mut dim, then
/// the three semantic ones.
const CAUTION_INDEX: usize = 11;
const ERROR_INDEX: usize = 12;
const INK_INDEX: usize = 6;

/// The installer's in-window brand lockup, reproducing the application Top
/// Bar's own relationship -- an 18 px mark, an 8 px gap and the wordmark at a
/// 16 px type size, all vertically centred -- at 2x so the header reads at
/// window scale. The displayed pixels are rasterized at 4x so a DPI-scaled
/// control downsamples cleanly. Never a second drawing: the mark comes from
/// `marks/brand.svg` plus `BrandMark.h`'s optical profile, the wordmark from
/// `marks/wordmark.svg`, and neither is restated here.
const INSTALLER_MARK_PX: f64 = 36.0;
const INSTALLER_GAP_PX: f64 = 16.0;
const INSTALLER_WORDMARK_TYPE_PX: f64 = 32.0;
const INSTALLER_RASTER_SCALE: u32 = 4;

struct Rgb(u8, u8, u8);

fn hex_to_rgb(value: &str) -> anyhow::Result<Rgb> {
    let value = value.trim_start_matches('#');
    anyhow::ensure!(value.len() == 6, "not a 6-digit hex colour: {value}");
    let byte = |i: usize| u8::from_str_radix(&value[i..i + 2], 16);
    Ok(Rgb(
        byte(0).context("parsing red channel")?,
        byte(2).context("parsing green channel")?,
        byte(4).context("parsing blue channel")?,
    ))
}

impl Rgb {
    fn svg(&self) -> String {
        format!("#{:02x}{:02x}{:02x}", self.0, self.1, self.2)
    }
}

/// One rasterized square RGBA frame at `size` pixels on a side.
struct Frame {
    size: u32,
    pixmap: tiny_skia::Pixmap,
}

/// Renders `svg_body` (already a complete `<svg>...</svg>` document string,
/// viewBox `0 0 grid grid`) to a `size x size` RGBA frame. Rendering is
/// analytic (vector to raster with real antialiasing), not the
/// draw-then-downscale supersampling the Pillow original needed for
/// smooth edges, since resvg does not need it.
fn render_frame(svg_body: &str, size: u32) -> anyhow::Result<Frame> {
    let tree = resvg::usvg::Tree::from_str(svg_body, &resvg::usvg::Options::default())
        .context("parsing generated icon frame SVG")?;
    let mut pixmap = tiny_skia::Pixmap::new(size, size).context("allocating icon frame pixmap")?;
    let tree_size = tree.size();
    let scale = size as f32 / tree_size.width().max(tree_size.height());
    let transform = tiny_skia::Transform::from_scale(scale, scale);
    resvg::render(&tree, transform, &mut pixmap.as_mut());
    Ok(Frame { size, pixmap })
}

/// Renders a non-square SVG document to a transparent RGBA PNG at the exact
/// pixel size asked for. Used for the installer lockup, which is wider than
/// it is tall.
fn render_png(svg: &str, width: u32, height: u32) -> anyhow::Result<Vec<u8>> {
    let tree = resvg::usvg::Tree::from_str(svg, &resvg::usvg::Options::default())
        .context("parsing generated brand SVG")?;
    let mut pixmap =
        tiny_skia::Pixmap::new(width, height).context("allocating brand lockup pixmap")?;
    let size = tree.size();
    let transform = tiny_skia::Transform::from_scale(
        width as f32 / size.width(),
        height as f32 / size.height(),
    );
    resvg::render(&tree, transform, &mut pixmap.as_mut());
    pixmap.encode_png().context("encoding brand lockup PNG")
}

/// The installer's mark+wordmark lockup as one transparent PNG, composed from
/// the same two canonical SVGs the running application uses.
fn installer_brand_png(
    repo_root: &Path,
    mark: &BrandMark,
    circles: &[Circle],
    accent_hex: &str,
    ink_hex: &str,
) -> anyhow::Result<Vec<u8>> {
    let wordmark_path = repo_root.join("app/assets/brand/marks/wordmark.svg");
    let wordmark_svg = std::fs::read_to_string(&wordmark_path)
        .with_context(|| format!("could not read {}", wordmark_path.display()))?;
    anyhow::ensure!(
        wordmark_svg.contains("#9BD9D2") && wordmark_svg.contains("#F1F1EF"),
        "{}: the wordmark no longer carries the reference accent and ink",
        wordmark_path.display()
    );
    let view_box = regex::Regex::new(r#"viewBox="([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)""#)
        .expect("valid regex");
    let caps = view_box
        .captures(&wordmark_svg)
        .with_context(|| format!("{} has no viewBox", wordmark_path.display()))?;
    let vb_x: f64 = caps[1].parse()?;
    let vb_y: f64 = caps[2].parse()?;
    let vb_w: f64 = caps[3].parse()?;
    let vb_h: f64 = caps[4].parse()?;

    let grid = mark.value("kGrid");
    let center = mark.value("kCenter");
    let em_units = mark.value("kWordmarkEmUnits");

    let scale = f64::from(INSTALLER_RASTER_SCALE);
    let mark_px = INSTALLER_MARK_PX * scale;
    let gap_px = INSTALLER_GAP_PX * scale;
    let type_px = INSTALLER_WORDMARK_TYPE_PX * scale;
    let wordmark_scale = type_px / em_units;
    let wordmark_w = vb_w * wordmark_scale;
    let wordmark_h = vb_h * wordmark_scale;
    let canvas_w = mark_px + gap_px + wordmark_w;
    let canvas_h = mark_px;

    // Inline next to the wordmark, so no standalone margin; the optical
    // profile is the one the DISPLAYED size resolves to, not the raster's own
    // larger size, because the correction answers how large the mark reads.
    let profile = mark.profile_for(INSTALLER_MARK_PX as i32);
    let mark_scale = mark_px / grid;
    let mut mark_body = String::new();
    for circle in circles {
        let alpha = if circle.opacity < 1.0 {
            (circle.opacity * profile.outer_opacity_scale).min(1.0)
        } else {
            1.0
        };
        if let Some(stroke) = &circle.stroke {
            let width = circle.stroke_width * profile.ring_stroke_scale;
            mark_body.push_str(&format!(
                r#"<circle cx="{center}" cy="{center}" r="{}" fill="none" stroke="{stroke}" stroke-width="{}" opacity="{}"/>"#,
                super::num(circle.r, 4),
                super::num(width, 4),
                super::num(alpha, 4),
            ));
        } else {
            mark_body.push_str(&format!(
                r#"<circle cx="{center}" cy="{center}" r="{}" fill="{}" opacity="{}"/>"#,
                super::num(circle.r, 4),
                circle.fill,
                super::num(alpha, 4),
            ));
        }
    }

    let wordmark_content = wordmark_svg
        .split_once('>')
        .and_then(|(_, rest)| rest.rsplit_once("</svg>").map(|(inner, _)| inner))
        .with_context(|| format!("{} has no SVG body", wordmark_path.display()))?
        .replace("#9BD9D2", accent_hex)
        .replace("#F1F1EF", ink_hex);

    // The wordmark's box is centred against the taller mark on the same
    // vertical centre line the Top Bar's RowLayout uses.
    let tx = mark_px + gap_px - vb_x * wordmark_scale;
    let ty = (canvas_h - wordmark_h) / 2.0 - vb_y * wordmark_scale;
    let svg = format!(
        r#"<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="0 0 {} {}"><g transform="translate(0,0) scale({})">{mark_body}</g><g transform="translate({},{}) scale({})">{wordmark_content}</g></svg>"#,
        super::num(canvas_w, 3),
        super::num(canvas_h, 3),
        super::num(canvas_w, 3),
        super::num(canvas_h, 3),
        super::num(mark_scale, 6),
        super::num(tx, 4),
        super::num(ty, 4),
        super::num(wordmark_scale, 6),
    );
    render_png(&svg, canvas_w.ceil() as u32, canvas_h.ceil() as u32)
}

fn svg_document(grid: f64, body: &str) -> String {
    let grid = grid.to_string();
    format!(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{grid}\" height=\"{grid}\" viewBox=\"0 0 {grid} {grid}\">{body}</svg>"
    )
}

/// The application icon frame at one size: the canonical mark, with the
/// optical profile that size resolves to. The colours are the asset's own;
/// this icon is the identity of the file and carries no session and no
/// user accent, so there is nothing here to resolve against a theme.
fn mark_frame_svg(mark: &BrandMark, circles: &[Circle], grid: f64, center: f64, px: u32) -> String {
    let profile = mark.profile_for(px as i32);
    let content = mark.value("kStandaloneContentScale") * profile.content_scale;

    let mut body = String::new();
    for circle in circles {
        let alpha = if circle.opacity < 1.0 {
            (circle.opacity * profile.outer_opacity_scale).min(1.0)
        } else {
            1.0
        };
        let r = circle.r * content;
        if let Some(stroke) = &circle.stroke {
            let width = circle.stroke_width * content * profile.ring_stroke_scale;
            body.push_str(&format!(
                r#"<circle cx="{center}" cy="{center}" r="{r}" fill="none" stroke="{stroke}" stroke-width="{width}" opacity="{alpha}"/>"#
            ));
        } else {
            body.push_str(&format!(
                r#"<circle cx="{center}" cy="{center}" r="{r}" fill="{}" opacity="{alpha}"/>"#,
                circle.fill
            ));
        }
    }
    svg_document(grid, &body)
}

/// A thumbnail glyph shape, drawn on the aperture's own grid, in one flat
/// colour. Every shape here mirrors the constants BrandMark.h dedicates to
/// it; there is no equivalent state-suite composition for these, they are
/// shell chrome.
enum GlyphShape {
    Disc,
    Square,
    Bars,
    Folder,
    Triangle,
}

fn thumb_frame_svg(
    mark: &BrandMark,
    grid: f64,
    center: f64,
    shape: &GlyphShape,
    colour: &str,
) -> String {
    let body = match shape {
        GlyphShape::Disc => {
            let r = mark.value("kGlyphDiscRadius");
            format!(r#"<circle cx="{center}" cy="{center}" r="{r}" fill="{colour}"/>"#)
        }
        GlyphShape::Square => {
            let half = mark.value("kGlyphSquareHalf");
            let corner = mark.value("kGlyphSquareCorner");
            let side = half * 2.0;
            format!(
                r#"<rect x="{}" y="{}" width="{side}" height="{side}" rx="{corner}" ry="{corner}" fill="{colour}"/>"#,
                center - half,
                center - half,
            )
        }
        GlyphShape::Bars => {
            let bar_w = mark.value("kGlyphBarWidth");
            let bar_h = mark.value("kGlyphBarHeight");
            let gap = mark.value("kGlyphBarGap");
            let corner = mark.value("kGlyphBarCorner");
            let mut out = String::new();
            for direction in [-1.0, 1.0] {
                let x = center + direction * (gap / 2.0 + bar_w / 2.0);
                out.push_str(&format!(
                    r#"<rect x="{}" y="{}" width="{bar_w}" height="{bar_h}" rx="{corner}" ry="{corner}" fill="{colour}"/>"#,
                    x - bar_w / 2.0,
                    center - bar_h / 2.0,
                ));
            }
            out
        }
        GlyphShape::Folder => {
            // Stroked rather than filled, matching the tray menu's own
            // folder: the row it labels is about a place, not a recording.
            let left = center - mark.value("kGlyphFolderHalfWidth");
            let right = center + mark.value("kGlyphFolderHalfWidth");
            let top = mark.value("kGlyphFolderTopY");
            let body_y = mark.value("kGlyphFolderBodyY");
            let bottom = mark.value("kGlyphFolderBottomY");
            let tab = mark.value("kGlyphFolderTabWidth");
            let stroke = mark.value("kGlyphStroke");
            let points = [
                (left, bottom),
                (left, top),
                (left + tab, top),
                (left + tab + 1.8, body_y),
                (right, body_y),
                (right, bottom),
                (left, bottom),
            ];
            let points_attr = points
                .iter()
                .map(|(x, y)| format!("{x},{y}"))
                .collect::<Vec<_>>()
                .join(" ");
            format!(
                r#"<polyline points="{points_attr}" fill="none" stroke="{colour}" stroke-width="{stroke}" stroke-linejoin="round" stroke-linecap="round"/>"#
            )
        }
        GlyphShape::Triangle => {
            let back = mark.value("kGlyphTriangleBackX");
            let tip = mark.value("kGlyphTriangleTipX");
            let half = mark.value("kGlyphTriangleHalfHeight");
            format!(
                r#"<polygon points="{back},{} {back},{} {tip},{center}" fill="{colour}"/>"#,
                center - half,
                center + half,
            )
        }
    };
    svg_document(grid, &body)
}

/// One `.ico` directory entry's payload, as a classic DIB. NOT PNG, and
/// that is the point: several shell surfaces still decode small PNG
/// frames incorrectly, so frames at or below [`PNG_FRAME_MIN_PX`] are
/// written the old way: a `BITMAPINFOHEADER` whose height is doubled for
/// the mask, 32-bit BGRA bottom-up pixels, and an AND mask that is all
/// zeroes because the alpha channel is what actually carries transparency.
fn bmp_frame(frame: &Frame) -> Vec<u8> {
    let (width, height) = (frame.size, frame.size);
    let data = frame.pixmap.data();
    let mut body = Vec::with_capacity((width * height * 4) as usize);
    for y in (0..height).rev() {
        for x in 0..width {
            let i = ((y * width + x) * 4) as usize;
            let (r, g, b, a) = (data[i], data[i + 1], data[i + 2], data[i + 3]);
            body.extend_from_slice(&[b, g, r, a]);
        }
    }
    let mask_stride = width.div_ceil(32) * 4;
    let mask = vec![0u8; (mask_stride * height) as usize];

    let mut header = Vec::with_capacity(40);
    header.extend_from_slice(&40u32.to_le_bytes());
    header.extend_from_slice(&(width as i32).to_le_bytes());
    header.extend_from_slice(&((height * 2) as i32).to_le_bytes());
    header.extend_from_slice(&1u16.to_le_bytes());
    header.extend_from_slice(&32u16.to_le_bytes());
    header.extend_from_slice(&0u32.to_le_bytes());
    header.extend_from_slice(&((body.len() + mask.len()) as u32).to_le_bytes());
    header.extend_from_slice(&0i32.to_le_bytes());
    header.extend_from_slice(&0i32.to_le_bytes());
    header.extend_from_slice(&0u32.to_le_bytes());
    header.extend_from_slice(&0u32.to_le_bytes());

    let mut out = header;
    out.extend_from_slice(&body);
    out.extend_from_slice(&mask);
    out
}

fn png_frame(frame: &Frame) -> anyhow::Result<Vec<u8>> {
    frame
        .pixmap
        .encode_png()
        .context("encoding icon frame as PNG")
}

/// The ICO container bytes: the container is written here rather than by an
/// image library, so the frame encoding is a decision this function makes,
/// not one a dependency makes for it.
fn ico_bytes(frames: &[Frame]) -> anyhow::Result<Vec<u8>> {
    let mut payloads = Vec::with_capacity(frames.len());
    for frame in frames {
        if frame.size > PNG_FRAME_MIN_PX {
            payloads.push(png_frame(frame)?);
        } else {
            payloads.push(bmp_frame(frame));
        }
    }

    let mut out = Vec::new();
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&(frames.len() as u16).to_le_bytes());

    let mut offset: u32 = 6 + 16 * frames.len() as u32;
    for (frame, payload) in frames.iter().zip(&payloads) {
        out.push((frame.size % 256) as u8);
        out.push((frame.size % 256) as u8);
        out.push(0);
        out.push(0);
        out.extend_from_slice(&1u16.to_le_bytes());
        out.extend_from_slice(&32u16.to_le_bytes());
        out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        out.extend_from_slice(&offset.to_le_bytes());
        offset += payload.len() as u32;
    }
    for payload in &payloads {
        out.extend_from_slice(payload);
    }

    Ok(out)
}

/// Generates `exosnap-app.ico`, the five thumbnail-toolbar glyph `.ico`
/// files (in the dark and light appearance's own state colours),
/// `exosnap-logo.svg` and the installer's brand lockup. With `check`, writes
/// nothing and reports drift instead.
pub fn generate(repo_root: &Path, check: bool) -> anyhow::Result<()> {
    let brand_mark_h_path = repo_root.join("app/ui/brand/BrandMark.h");
    let themes_h_path = repo_root.join("app/ui/theme/ExoSnapThemes.h");
    let brand_svg_path = repo_root.join("app/assets/brand/marks/brand.svg");
    let out_dir = repo_root.join("app/assets/brand");

    let brand_mark_h = std::fs::read_to_string(&brand_mark_h_path)
        .with_context(|| format!("could not read {}", brand_mark_h_path.display()))?;
    let themes_h = std::fs::read_to_string(&themes_h_path)
        .with_context(|| format!("could not read {}", themes_h_path.display()))?;
    let brand_svg = std::fs::read_to_string(&brand_svg_path)
        .with_context(|| format!("could not read {}", brand_svg_path.display()))?;

    let mark = parse_brand_mark(&brand_mark_h);
    let circles = parse_mark_circles(&brand_svg);
    let grid = mark.value("kGrid");
    let center = mark.value("kCenter");

    // The asset is authored in the shipped default accent, which is the
    // only one a build-time artefact can carry: this is the identity of
    // the executable, not of a session. If the two ever part company that
    // is a decision, so it is asserted rather than papered over.
    let accent_hex = find_aqua_hex(&themes_h, 0)
        .context("ExoSnapThemes.h: could not find the shipped default accent")?;
    anyhow::ensure!(
        brand_svg
            .to_uppercase()
            .contains(&accent_hex.to_uppercase()),
        "{}: authored accent is not the shipped default {accent_hex}",
        brand_svg_path.display()
    );
    let accent_light_hex = find_aqua_hex(&themes_h, 2)
        .context("ExoSnapThemes.h: could not find the light-appearance accent")?;

    std::fs::create_dir_all(&out_dir)
        .with_context(|| format!("could not create {}", out_dir.display()))?;

    let mut artifacts: Vec<(std::path::PathBuf, Vec<u8>)> = Vec::new();

    let mut app_frames = Vec::with_capacity(APP_ICO_SIZES.len());
    for &px in &APP_ICO_SIZES {
        app_frames.push(render_frame(
            &mark_frame_svg(&mark, &circles, grid, center, px),
            px,
        )?);
    }
    artifacts.push((out_dir.join("exosnap-app.ico"), ico_bytes(&app_frames)?));

    artifacts.push((
        out_dir.join("exosnap-logo.svg"),
        brand_svg.clone().into_bytes(),
    ));

    // Two sets, one per appearance: the taskbar's thumbnail strip is
    // Windows chrome, so its ground follows the system appearance and we
    // only supply the glyph. The dark appearance's state colours are
    // lightened for a dark ground, so on a light strip the amber pause
    // glyph would sit on light grey at a contrast the palette never
    // intended. Each set uses the state colours of the appearance it will
    // be drawn against.
    let dark_coral = hex_to_rgb(&parse_theme_colour(&themes_h, "dark", ERROR_INDEX))?;
    let dark_amber = hex_to_rgb(&parse_theme_colour(&themes_h, "dark", CAUTION_INDEX))?;
    let dark_accent = hex_to_rgb(&accent_hex)?;
    let light_coral = hex_to_rgb(&parse_theme_colour(&themes_h, "light", ERROR_INDEX))?;
    let light_amber = hex_to_rgb(&parse_theme_colour(&themes_h, "light", CAUTION_INDEX))?;
    let light_accent = hex_to_rgb(&accent_light_hex)?;

    let thumbs: [(&str, GlyphShape, &str); 5] = [
        ("exosnap-thumb-record", GlyphShape::Disc, "coral"),
        ("exosnap-thumb-pause", GlyphShape::Bars, "amber"),
        ("exosnap-thumb-resume", GlyphShape::Triangle, "accent"),
        ("exosnap-thumb-stop", GlyphShape::Square, "coral"),
        // Not transport: the thumbnail strip's fourth button opens the
        // recording destination, which is safe in every state and
        // therefore never greyed.
        ("exosnap-thumb-folder", GlyphShape::Folder, "accent"),
    ];

    for (suffix, coral, amber, accent) in [
        ("", &dark_coral, &dark_amber, &dark_accent),
        ("-light", &light_coral, &light_amber, &light_accent),
    ] {
        for (stem, shape, role) in &thumbs {
            let rgb = match *role {
                "coral" => coral,
                "amber" => amber,
                _ => accent,
            };
            let colour = rgb.svg();
            let mut frames = Vec::with_capacity(SHELL_ICO_SIZES.len());
            for &px in &SHELL_ICO_SIZES {
                frames.push(render_frame(
                    &thumb_frame_svg(&mark, grid, center, shape, &colour),
                    px,
                )?);
            }
            artifacts.push((
                out_dir.join(format!("{stem}{suffix}.ico")),
                ico_bytes(&frames)?,
            ));
        }
    }

    // The installer's fixed-dark lockup: the dark appearance's own ink and
    // the shipped default accent's dark value, so the raster is derived from
    // the same theme table the running application resolves.
    let dark_ink = parse_theme_colour(&themes_h, "dark", INK_INDEX);
    artifacts.push((
        repo_root.join("packaging/burn/exosnap-brand.png"),
        installer_brand_png(repo_root, &mark, &circles, &accent_hex, &dark_ink)?,
    ));

    if check {
        let drifted: Vec<_> = artifacts
            .iter()
            .filter(|(path, bytes)| std::fs::read(path).ok().as_deref() != Some(bytes.as_slice()))
            .map(|(path, _)| path.clone())
            .collect();
        if !drifted.is_empty() {
            for path in &drifted {
                eprintln!("drifted: {}", path.display());
            }
            anyhow::bail!(
                "{} generated brand artefact(s) differ from the generator; run `cargo exo-dev generate-app-icons`",
                drifted.len()
            );
        }
        println!("{} brand artefact(s) match the generator", artifacts.len());
        return Ok(());
    }

    for (path, bytes) in &artifacts {
        std::fs::write(path, bytes)
            .with_context(|| format!("could not write {}", path.display()))?;
    }
    Ok(())
}

/// The Nth `#RRGGBB` colour value that appears after the `"aqua",` id in
/// `ExoSnapThemes.h`. Position 0 is dark's shipped default accent; per the
/// original script's own indexing, position 2 (counting successive matches
/// after the id, 0-based) is light's accent column.
fn find_aqua_hex(source: &str, skip: usize) -> Option<String> {
    let start = source.find("\"aqua\",")?;
    let colour_re = regex::Regex::new(r"#[0-9A-Fa-f]{6}").expect("valid regex");
    colour_re
        .find_iter(&source[start..])
        .nth(skip)
        .map(|m| m.as_str().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bmp_frame_has_the_doubled_height_header_and_and_mask() {
        let pixmap = tiny_skia::Pixmap::new(4, 4).unwrap();
        let frame = Frame { size: 4, pixmap };
        let bytes = bmp_frame(&frame);
        // 40-byte header + 4*4*4 body + AND mask (padded to 4 bytes per row).
        let mask_stride = 4u32.div_ceil(32) * 4;
        assert_eq!(bytes.len(), 40 + 4 * 4 * 4 + (mask_stride * 4) as usize);
        assert_eq!(&bytes[0..4], &40u32.to_le_bytes());
        assert_eq!(&bytes[4..8], &4i32.to_le_bytes());
        assert_eq!(
            &bytes[8..12],
            &8i32.to_le_bytes(),
            "height must be doubled for the mask"
        );
    }

    #[test]
    fn ico_bytes_roundtrips_a_readable_directory() {
        let pixmap = tiny_skia::Pixmap::new(16, 16).unwrap();
        let frame = Frame { size: 16, pixmap };
        let bytes = ico_bytes(std::slice::from_ref(&frame)).unwrap();
        assert_eq!(&bytes[0..2], &0u16.to_le_bytes());
        assert_eq!(&bytes[2..4], &1u16.to_le_bytes(), "type must be 1 (icon)");
        assert_eq!(&bytes[4..6], &1u16.to_le_bytes(), "one frame");
        assert_eq!(bytes[6], 16, "width byte");
        assert_eq!(bytes[7], 16, "height byte");
    }

    #[test]
    fn png_frame_is_used_above_the_dib_threshold() {
        let pixmap = tiny_skia::Pixmap::new(128, 128).unwrap();
        let frame = Frame { size: 128, pixmap };
        let bytes = png_frame(&frame).unwrap();
        assert_eq!(
            &bytes[0..8],
            &[0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A],
            "PNG signature"
        );
    }

    #[test]
    fn find_aqua_hex_reads_successive_colours() {
        let source = r##""aqua", "Aqua", ThemeKind::Dark, "#111111", "#222222""##;
        assert_eq!(find_aqua_hex(source, 0), Some("#111111".to_string()));
        assert_eq!(find_aqua_hex(source, 1), Some("#222222".to_string()));
        assert_eq!(find_aqua_hex(source, 5), None);
    }

    #[test]
    fn hex_to_rgb_parses_channels() {
        let rgb = hex_to_rgb("#9BD9D2").unwrap();
        assert_eq!((rgb.0, rgb.1, rgb.2), (0x9B, 0xD9, 0xD2));
        assert_eq!(rgb.svg(), "#9bd9d2");
    }
}
