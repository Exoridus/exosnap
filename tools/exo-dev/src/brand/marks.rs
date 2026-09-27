//! Generates the ExoSnap mark suite (`app/assets/brand/marks/*.svg`) from
//! `parameters.json`.
//!
//! Every mark draws the same aperture: an outer ring, an inner ring and
//! whatever the state puts inside it. The five numbers that define that
//! aperture appear in fourteen files, so a hand-kept suite makes a
//! one-number design change a fourteen-file edit that drifts on the first
//! miss. `parameters.json` is the one place those numbers live; this module
//! writes the suite from them, and `check` mode reports drift without
//! writing, so a checked-in file that stopped matching the parameters fails
//! the build instead of silently going stale.
//!
//! The colours written into the files are the designer's reference palette,
//! not what ships: `app/ui/brand/BrandMarkSvg.h` substitutes the running
//! theme's accent and semantic colours for them at load. Keeping the
//! reference values in the files is what makes the assets readable on
//! their own.

use std::path::{Path, PathBuf};

use anyhow::Context as _;
use serde::Deserialize;

use super::num;

/// Frames per animated state. The shell renders them as whole-icon swaps.
const PROCESSING_FRAME_COUNT: usize = 4;

#[derive(Deserialize)]
struct Geometry {
    outer_r: f64,
    outer_w: f64,
    inner_r: f64,
    inner_w: f64,
    center_r: f64,
}

#[derive(Deserialize)]
struct OuterOpacity {
    dark: f64,
}

#[derive(Deserialize)]
struct ReferenceColours {
    accent: String,
    recording: String,
    caution: String,
    success: String,
}

#[derive(Deserialize)]
struct Parameters {
    grid: f64,
    geometry: Geometry,
    outer_opacity: OuterOpacity,
    reference_colors: ReferenceColours,
}

pub struct Marks {
    grid: f64,
    center: f64,
    outer_r: f64,
    outer_w: f64,
    inner_r: f64,
    inner_w: f64,
    center_r: f64,
    outer_opacity: f64,
    accent: String,
    recording: String,
    caution: String,
    success: String,
}

/// The recording animation's per-frame (dot, ring) brightness levels.
///
/// ALPHA ONLY, deliberately: modulating radii as well makes adjacent frames
/// differ by well under a device pixel at 16 px, which reads as a flicker
/// rather than a heartbeat. The first and last frames are identical on
/// purpose: the loop rests at the bottom for two ticks, which is what makes
/// it read as a heartbeat rather than a metronome.
const DIM: f64 = 0.42;
const MID: f64 = 0.70;
const BRIGHT: f64 = 1.0;
const RECORDING_LEVELS: [(f64, f64); 6] = [
    (DIM, DIM),
    (MID, DIM),
    (BRIGHT, MID),
    (MID, BRIGHT),
    (DIM, MID),
    (DIM, DIM),
];

/// The processing animation's dash geometry: six dashes so the arc reads as
/// motion rather than a broken ring, one quarter segment of travel per
/// frame so the loop is seamless.
const PROCESSING_DASH_COUNT: f64 = 6.0;
const PROCESSING_DASH_FRACTION: f64 = 0.42;
const PROCESSING_BAR_HEIGHTS: [[f64; 3]; 4] = [
    [0.878, 0.703, 0.514],
    [0.703, 0.878, 0.703],
    [0.514, 0.703, 0.878],
    [0.703, 0.514, 0.703],
];

impl Marks {
    pub fn from_parameters_json(source: &str) -> anyhow::Result<Self> {
        let params: Parameters = serde_json::from_str(source).context("parsing parameters.json")?;
        Ok(Marks {
            grid: params.grid,
            center: params.grid / 2.0,
            outer_r: params.geometry.outer_r,
            outer_w: params.geometry.outer_w,
            inner_r: params.geometry.inner_r,
            inner_w: params.geometry.inner_w,
            center_r: params.geometry.center_r,
            outer_opacity: params.outer_opacity.dark,
            accent: params.reference_colors.accent,
            recording: params.reference_colors.recording,
            caution: params.reference_colors.caution,
            success: params.reference_colors.success,
        })
    }

    fn circle(
        &self,
        r: f64,
        stroke: Option<&str>,
        width: Option<f64>,
        fill: &str,
        opacity: Option<f64>,
        extra: &str,
    ) -> String {
        let mut out = format!(
            r#"<circle cx="{}" cy="{}" r="{}""#,
            num(self.center, 2),
            num(self.center, 2),
            num(r, 2)
        );
        out.push_str(&format!(r#" fill="{fill}""#));
        if let Some(stroke) = stroke {
            out.push_str(&format!(
                r#" stroke="{stroke}" stroke-width="{}""#,
                num(width.unwrap_or(0.0), 2)
            ));
        }
        if !extra.is_empty() {
            out.push(' ');
            out.push_str(extra);
        }
        if let Some(opacity) = opacity {
            out.push_str(&format!(r#" opacity="{}""#, num(opacity, 2)));
        }
        out.push_str("/>");
        out
    }

    fn disc(&self, r: f64, colour: &str, opacity: Option<f64>) -> String {
        self.circle(r, None, None, colour, opacity, "")
    }

    /// An upright bar centred on `(cx, cy)`. `radius` defaults to fully
    /// rounded ends; the transport bars use a softer corner instead, since
    /// a pause glyph drawn as two capsules reads as an equals sign at 16 px.
    fn bar(&self, cx: f64, cy: f64, w: f64, h: f64, colour: &str, radius: Option<f64>) -> String {
        let r = radius.unwrap_or(w / 2.0);
        format!(
            r#"<rect x="{}" y="{}" width="{}" height="{}" rx="{}" ry="{}" fill="{colour}"/>"#,
            num(cx - w / 2.0, 2),
            num(cy - h / 2.0, 2),
            num(w, 2),
            num(h, 2),
            num(r, 2),
            num(r, 2),
        )
    }

    fn outer_ring(&self) -> String {
        self.circle(
            self.outer_r,
            Some(&self.accent),
            Some(self.outer_w),
            "none",
            Some(self.outer_opacity),
            "",
        )
    }

    fn document(&self, body: &[String]) -> String {
        let grid = num(self.grid, 2);
        format!(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{grid}\" height=\"{grid}\" viewBox=\"0 0 {grid} {grid}\">{}</svg>\n",
            body.concat()
        )
    }

    /// The identity, and what Idle shows. The dot is the recording colour:
    /// the aperture is trained on something and the mark says so at rest.
    fn brand(&self) -> String {
        self.document(&[
            self.outer_ring(),
            self.circle(
                self.inner_r,
                Some(&self.accent),
                Some(self.inner_w),
                "none",
                None,
                "",
            ),
            self.disc(self.center_r, &self.recording, None),
        ])
    }

    fn paused(&self) -> String {
        let bar_w = self.inner_w * 1.107;
        let bar_h = self.inner_r * 1.284;
        let offset = self.inner_r * 0.294;
        let corner = self.inner_w * 0.321;
        self.document(&[
            self.outer_ring(),
            self.circle(
                self.inner_r,
                Some(&self.caution),
                Some(self.inner_w),
                "none",
                None,
                "",
            ),
            self.bar(
                self.center - offset,
                self.center,
                bar_w,
                bar_h,
                &self.caution,
                Some(corner),
            ),
            self.bar(
                self.center + offset,
                self.center,
                bar_w,
                bar_h,
                &self.caution,
                Some(corner),
            ),
        ])
    }

    /// A check, not a tick mark: the short arm is deliberately short, since
    /// at 16 px two arms of similar length read as a V. Every coefficient
    /// is a fraction of the inner radius, and the arms are given as
    /// explicit deltas from the elbow rather than as one length times two
    /// ratios, so the stroke clears the aperture at the smallest rendered
    /// profile instead of touching it.
    fn saved(&self) -> String {
        let elbow_x = self.center - self.inner_r * 0.194595;
        let elbow_y = self.center + self.inner_r * 0.462162;
        let short = self.inner_r * 0.389189;
        let long_dx = self.inner_r * 0.778378;
        let long_dy = self.inner_r * 0.839189;
        let path = format!(
            "M{} {}L{} {}L{} {}",
            num(elbow_x - short, 2),
            num(elbow_y - short, 2),
            num(elbow_x, 2),
            num(elbow_y, 2),
            num(elbow_x + long_dx, 2),
            num(elbow_y - long_dy, 2),
        );
        self.document(&[
            self.outer_ring(),
            self.circle(self.inner_r, Some(&self.success), Some(self.inner_w), "none", None, ""),
            format!(
                r#"<path d="{path}" fill="none" stroke="{}" stroke-width="{}" stroke-linecap="round" stroke-linejoin="round"/>"#,
                self.success,
                num(self.inner_w * 1.214286, 2)
            ),
        ])
    }

    fn warning(&self) -> String {
        let half = self.inner_r * 0.586486;
        let top = self.center - self.inner_r * 0.586486;
        let bottom = self.center + self.inner_r * 0.429730;
        self.document(&[
            self.outer_ring(),
            self.circle(self.inner_r, Some(&self.caution), Some(self.inner_w), "none", None, ""),
            format!(
                r#"<path d="M{} {}L{} {}H{}Z" fill="none" stroke="{}" stroke-width="{}" stroke-linejoin="round"/>"#,
                num(self.center, 2),
                num(top, 2),
                num(self.center + half, 2),
                num(bottom, 2),
                num(self.center - half, 2),
                self.caution,
                num(self.inner_w * 0.893, 2),
            ),
            format!(
                r#"<path d="M{} {}V{}" fill="none" stroke="{}" stroke-width="{}" stroke-linecap="round" stroke-linejoin="round"/>"#,
                num(self.center, 2),
                num(self.center - self.inner_r * 0.266216, 2),
                num(self.center + self.inner_r * 0.121622, 2),
                self.caution,
                num(self.inner_w * 0.964, 2),
            ),
            format!(
                r#"<circle cx="{}" cy="{}" r="{}" fill="{}"/>"#,
                num(self.center, 2),
                num(self.center + self.inner_r * 0.314865, 2),
                num(self.center_r * 0.221818, 2),
                self.caution,
            ),
        ])
    }

    fn error(&self) -> String {
        let arm = self.inner_r * 0.493;
        let (lo, hi) = (self.center - arm, self.center + arm);
        let path = format!(
            "M{} {}L{} {} M{} {}L{} {}",
            num(lo, 2),
            num(lo, 2),
            num(hi, 2),
            num(hi, 2),
            num(hi, 2),
            num(lo, 2),
            num(lo, 2),
            num(hi, 2),
        );
        self.document(&[
            self.outer_ring(),
            self.circle(self.inner_r, Some(&self.recording), Some(self.inner_w), "none", None, ""),
            format!(
                r#"<path d="{path}" fill="none" stroke="{}" stroke-width="{}" stroke-linecap="round" stroke-linejoin="round"/>"#,
                self.recording,
                num(self.inner_w * 1.036, 2)
            ),
        ])
    }

    fn recording_frame(&self, index: usize) -> String {
        let (dot, ring) = RECORDING_LEVELS[index];
        self.document(&[
            self.outer_ring(),
            self.circle(
                self.inner_r,
                Some(&self.recording),
                Some(self.inner_w),
                "none",
                Some(ring),
                "",
            ),
            self.disc(self.center_r, &self.recording, Some(dot)),
        ])
    }

    fn processing_frame(&self, index: usize) -> String {
        let segment = 2.0 * std::f64::consts::PI * self.inner_r / PROCESSING_DASH_COUNT;
        let dash = segment * PROCESSING_DASH_FRACTION;
        let gap = segment - dash;
        // Negate the integer index before converting to f64: negating a
        // float 0.0 yields IEEE negative zero, which `num()` then prints as
        // "-0" for the index == 0 frame instead of "0".
        let offset = -(index as isize) as f64 * segment / PROCESSING_FRAME_COUNT as f64;
        let extra = format!(
            r#"stroke-linecap="round" stroke-dasharray="{} {}" stroke-dashoffset="{}" transform="rotate(-90 {} {})""#,
            num(dash, 5),
            num(gap, 5),
            num(offset, 5),
            num(self.center, 2),
            num(self.center, 2),
        );
        let arc = self.circle(
            self.inner_r,
            Some(&self.accent),
            Some(self.inner_w),
            "none",
            None,
            &extra,
        );
        let bar_w = self.inner_w * 0.986;
        let pitch = self.inner_r * 0.466;
        let mut body = vec![self.outer_ring(), arc];
        for (column, height) in PROCESSING_BAR_HEIGHTS[index].iter().enumerate() {
            let cx = self.center + (column as f64 - 1.0) * pitch;
            body.push(self.bar(
                cx,
                self.center,
                bar_w,
                self.inner_r * height,
                &self.accent,
                None,
            ));
        }
        self.document(&body)
    }

    /// Every file the suite writes, keyed by its filename under
    /// `app/assets/brand/marks/`.
    pub fn suite(&self) -> Vec<(String, String)> {
        let mut files = vec![
            ("brand.svg".to_string(), self.brand()),
            // Idle and the brand mark are deliberately the same drawing.
            // Two names because they are two ideas: one is the product's
            // identity and the other is a session state that happens to
            // look like it.
            ("idle.svg".to_string(), self.brand()),
            ("paused.svg".to_string(), self.paused()),
            ("saved.svg".to_string(), self.saved()),
            ("warning.svg".to_string(), self.warning()),
            ("error.svg".to_string(), self.error()),
        ];
        for index in 0..RECORDING_LEVELS.len() {
            files.push((
                format!("recording-f{index}.svg"),
                self.recording_frame(index),
            ));
        }
        for index in 0..PROCESSING_FRAME_COUNT {
            files.push((
                format!("processing-f{index}.svg"),
                self.processing_frame(index),
            ));
        }
        files
    }
}

pub struct MarksReport {
    pub stale: Vec<String>,
    pub total: usize,
}

impl MarksReport {
    pub fn ok(&self) -> bool {
        self.stale.is_empty()
    }
}

/// Generates the mark suite into `<repo_root>/app/assets/brand/marks/`.
/// `check` reports drift without writing, matching `--check`'s contract:
/// every file that disagrees with the generated content is reported stale,
/// nothing is written, and the caller treats a nonempty report as failure.
pub fn generate(repo_root: &Path, check: bool) -> anyhow::Result<MarksReport> {
    let marks_dir = marks_dir(repo_root);
    let parameters_path = marks_dir.join("parameters.json");
    let parameters = std::fs::read_to_string(&parameters_path)
        .with_context(|| format!("could not read {}", parameters_path.display()))?;
    let marks = Marks::from_parameters_json(&parameters)?;
    let suite = marks.suite();

    let mut stale = Vec::new();
    for (name, content) in &suite {
        let path = marks_dir.join(name);
        let current = std::fs::read_to_string(&path).unwrap_or_default();
        if &current == content {
            continue;
        }
        if check {
            stale.push(name.clone());
            continue;
        }
        std::fs::write(&path, content)
            .with_context(|| format!("could not write {}", path.display()))?;
    }

    Ok(MarksReport {
        stale,
        total: suite.len(),
    })
}

fn marks_dir(repo_root: &Path) -> PathBuf {
    repo_root.join("app/assets/brand/marks")
}

#[cfg(test)]
mod tests {
    use super::*;

    const PARAMETERS: &str = r##"{
        "grid": 32,
        "geometry": { "outer_r": 13.7, "outer_w": 1.75, "inner_r": 7.4, "inner_w": 1.4, "center_r": 2.75 },
        "outer_opacity": { "dark": 0.64, "light": 0.82 },
        "reference_colors": { "accent": "#9BD9D2", "recording": "#E0786C", "caution": "#E7C875", "success": "#8FD0AF", "ink": "#F1F1EF" }
    }"##;

    #[test]
    fn suite_has_sixteen_files() {
        let marks = Marks::from_parameters_json(PARAMETERS).unwrap();
        assert_eq!(marks.suite().len(), 16);
    }

    #[test]
    fn brand_and_idle_are_identical() {
        let marks = Marks::from_parameters_json(PARAMETERS).unwrap();
        let suite: std::collections::HashMap<_, _> = marks.suite().into_iter().collect();
        assert_eq!(suite["brand.svg"], suite["idle.svg"]);
    }

    #[test]
    fn brand_svg_has_three_circles_and_the_accent_ring() {
        let marks = Marks::from_parameters_json(PARAMETERS).unwrap();
        let brand = marks.brand();
        assert_eq!(brand.matches("<circle").count(), 3);
        assert!(brand.contains("#9BD9D2"));
        assert!(brand.contains("#E0786C"));
    }

    #[test]
    fn check_reports_drift_without_writing() {
        let dir = tempfile::tempdir().unwrap();
        let marks_dir = dir.path().join("app/assets/brand/marks");
        std::fs::create_dir_all(&marks_dir).unwrap();
        std::fs::write(marks_dir.join("parameters.json"), PARAMETERS).unwrap();
        std::fs::write(marks_dir.join("brand.svg"), "stale content").unwrap();

        let report = generate(dir.path(), true).unwrap();
        assert!(!report.ok());
        assert!(report.stale.contains(&"brand.svg".to_string()));
        // Nothing was written: the file on disk is still the stale content.
        assert_eq!(
            std::fs::read_to_string(marks_dir.join("brand.svg")).unwrap(),
            "stale content"
        );
    }

    #[test]
    fn without_check_it_writes_and_then_reports_clean() {
        let dir = tempfile::tempdir().unwrap();
        let marks_dir = dir.path().join("app/assets/brand/marks");
        std::fs::create_dir_all(&marks_dir).unwrap();
        std::fs::write(marks_dir.join("parameters.json"), PARAMETERS).unwrap();

        let first = generate(dir.path(), false).unwrap();
        assert!(first.ok());
        assert_eq!(first.total, 16);

        let second = generate(dir.path(), true).unwrap();
        assert!(second.ok(), "{:?}", second.stale);
    }

    #[test]
    fn brand_uses_num_for_the_grid_dimension() {
        let marks = Marks::from_parameters_json(PARAMETERS).unwrap();
        assert!(marks.document(&[]).contains("width=\"32\""));
    }
}
