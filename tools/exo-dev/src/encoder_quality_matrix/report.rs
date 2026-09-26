//! CSV and Markdown output for a completed matrix sweep.

use std::path::Path;

use anyhow::Context as _;

use super::MatrixRow;
use super::process_args;

const CSV_HEADER: &str = "preset,rc,value,bitrate_kbps,vmaf,vmaf_median,vmaf_p10,vmaf_p5,vmaf_p1,vmaf_worst1_mean,vmaf_min,ssim,psnr";

/// The CSV rendering of `rows`. Column order and set match the CSV writer
/// this replaces exactly: `frames` and `libvmaf_version` are tracked per
/// row but are not CSV columns.
pub fn build_csv(rows: &[MatrixRow]) -> String {
    let mut out = String::from(CSV_HEADER);
    out.push('\n');
    for row in rows {
        out.push_str(&format!(
            "{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
            row.preset,
            row.rc.as_str(),
            row.value,
            python_float(row.bitrate_kbps),
            python_float(row.vmaf),
            python_float(row.vmaf_median),
            python_float(row.vmaf_p10),
            python_float(row.vmaf_p5),
            python_float(row.vmaf_p1),
            python_float(row.vmaf_worst1_mean),
            python_float(row.vmaf_min),
            optional_field(row.ssim),
            optional_field(row.psnr),
        ));
    }
    out
}

fn optional_field(value: Option<f64>) -> String {
    match value {
        Some(v) => python_float(v),
        None => String::new(),
    }
}

/// Renders `v` the way Python's `str(float)` would: Rust's own `{}` for
/// `f64` prints a whole number like `80.0` as `80`, but Python's float
/// `str()` always keeps a decimal point. The CSV this replaces was written
/// by Python's `csv` module, which calls `str()` on every field, so a
/// consumer that parses the CSV as text (rather than as a float) sees the
/// same shape either way. This does not reproduce Python's switch to
/// scientific notation for very large or very small magnitudes, which does
/// not occur for this tool's actual bitrate/VMAF/SSIM/PSNR value ranges.
fn python_float(v: f64) -> String {
    if v.is_nan() {
        return "nan".to_string();
    }
    if v.is_infinite() {
        return if v > 0.0 {
            "inf".to_string()
        } else {
            "-inf".to_string()
        };
    }
    let s = format!("{v}");
    if s.contains('.') || s.contains('e') || s.contains('E') {
        s
    } else {
        format!("{s}.0")
    }
}

/// The Markdown report for `rows`: a small header of run metadata followed
/// by one table row per matrix cell.
pub fn build_markdown(
    vcodec: &str,
    clip_path: &str,
    date: &str,
    ffmpeg_version: &str,
    rows: &[MatrixRow],
) -> String {
    let mut out = String::new();
    out.push_str(&format!("# Encoder quality matrix - {vcodec}\n\n"));
    out.push_str(&format!("Clip: `{clip_path}`\n\n"));
    out.push_str(&format!("Date: {date}\n\n"));
    out.push_str(&format!("ffmpeg: `{ffmpeg_version}`\n\n"));
    if let Some(first) = rows.first() {
        let version = first.libvmaf_version.as_deref().unwrap_or("unknown");
        out.push_str(&format!(
            "libvmaf: `{version}`, model `vmaf_v0.6.1` (libvmaf default)\n\n"
        ));
        out.push_str(&format!("Scored frames per encode: {}\n\n", first.frames));
    }
    out.push_str(
        "Both inputs are normalised to an unspecified limited-range colour description before \
scoring, so a colour description present on only one side cannot be scored as distortion.\n\n",
    );
    out.push_str(
        "| Preset | RC | Value | Bitrate (kbps) | VMAF | VMAF p10 | VMAF p5 | VMAF p1 | \
VMAF worst-1% | VMAF min | SSIM | PSNR |\n",
    );
    out.push_str("|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for row in rows {
        out.push_str(&format!(
            "| {} | {} | {} | {:.0} | {:.2} | {:.2} | {:.2} | {:.2} | {:.2} | {:.2} | {} | {} |\n",
            row.preset,
            row.rc.as_str(),
            row.value,
            row.bitrate_kbps,
            row.vmaf,
            row.vmaf_p10,
            row.vmaf_p5,
            row.vmaf_p1,
            row.vmaf_worst1_mean,
            row.vmaf_min,
            row.ssim.map(|v| format!("{v:.4}")).unwrap_or_default(),
            row.psnr.map(|v| format!("{v:.2}")).unwrap_or_default(),
        ));
    }
    out
}

/// Writes `{output_base}.csv` and `{output_base}.md`, running `ffmpeg
/// -version` once for the Markdown report's header.
pub fn write_report(
    output_base: &str,
    vcodec: &str,
    clip_path: &Path,
    rows: &[MatrixRow],
    ffmpeg_path: &str,
) -> anyhow::Result<()> {
    std::fs::write(format!("{output_base}.csv"), build_csv(rows))
        .with_context(|| format!("could not write {output_base}.csv"))?;

    let version_argv = process_args::ffmpeg_version_argv(ffmpeg_path);
    let output = std::process::Command::new(&version_argv[0])
        .args(&version_argv[1..])
        .output()
        .with_context(|| "could not run ffmpeg -version")?;
    let ffmpeg_version = String::from_utf8_lossy(&output.stdout)
        .lines()
        .next()
        .unwrap_or_default()
        .to_string();

    let markdown = build_markdown(
        vcodec,
        &clip_path.to_string_lossy(),
        &today_iso8601(),
        &ffmpeg_version,
        rows,
    );
    std::fs::write(format!("{output_base}.md"), markdown)
        .with_context(|| format!("could not write {output_base}.md"))?;
    Ok(())
}

/// Today's UTC calendar date as `YYYY-MM-DD`, with no date/time dependency:
/// days since the Unix epoch converted with the standard civil-calendar
/// algorithm (Howard Hinnant's `civil_from_days`).
fn today_iso8601() -> String {
    let days = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| (d.as_secs() / 86400) as i64)
        .unwrap_or(0);
    let (year, month, day) = civil_from_days(days);
    format!("{year:04}-{month:02}-{day:02}")
}

fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = (z - era * 146097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let month = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if month <= 2 { y + 1 } else { y }, month, day)
}

#[cfg(test)]
mod tests {
    use super::super::matrix::RateControl;
    use super::*;

    fn sample_row() -> MatrixRow {
        MatrixRow {
            preset: "p4".to_string(),
            rc: RateControl::Cq,
            value: 24,
            bitrate_kbps: 4321.6,
            vmaf: 92.345,
            vmaf_median: 92.5,
            vmaf_p10: 88.1,
            vmaf_p5: 86.2,
            vmaf_p1: 84.3,
            vmaf_worst1_mean: 83.9,
            vmaf_min: 80.0,
            frames: 300,
            libvmaf_version: Some("2.3.1".to_string()),
            ssim: Some(0.9812),
            psnr: Some(41.23),
        }
    }

    #[test]
    fn csv_has_the_expected_header_and_one_line_per_row() {
        let csv = build_csv(&[sample_row()]);
        let mut lines = csv.lines();
        assert_eq!(lines.next().unwrap(), CSV_HEADER);
        assert_eq!(
            lines.next().unwrap(),
            "p4,cq,24,4321.6,92.345,92.5,88.1,86.2,84.3,83.9,80.0,0.9812,41.23"
        );
        assert!(lines.next().is_none());
    }

    #[test]
    fn csv_writes_an_empty_field_for_a_missing_optional_metric() {
        let mut row = sample_row();
        row.ssim = None;
        row.psnr = None;
        let csv = build_csv(&[row]);
        assert_eq!(
            csv.lines().nth(1).unwrap(),
            "p4,cq,24,4321.6,92.345,92.5,88.1,86.2,84.3,83.9,80.0,,"
        );
    }

    #[test]
    fn python_float_always_keeps_a_decimal_point() {
        assert_eq!(python_float(80.0), "80.0");
        assert_eq!(python_float(4321.6), "4321.6");
        assert_eq!(python_float(0.0), "0.0");
    }

    #[test]
    fn markdown_includes_run_metadata_and_a_table_row_per_cell() {
        let markdown = build_markdown(
            "h264",
            "/clips/desktop.y4m",
            "2026-09-26",
            "ffmpeg version 7.1.1",
            &[sample_row()],
        );
        assert!(markdown.starts_with("# Encoder quality matrix - h264\n\n"));
        assert!(markdown.contains("Clip: `/clips/desktop.y4m`"));
        assert!(markdown.contains("Date: 2026-09-26"));
        assert!(markdown.contains("libvmaf: `2.3.1`, model `vmaf_v0.6.1` (libvmaf default)"));
        assert!(markdown.contains("Scored frames per encode: 300"));
        assert!(markdown.contains("| p4 | cq | 24 | 4322 | 92.34 | 88.10 | 86.20 | 84.30 | 83.90 | 80.00 | 0.9812 | 41.23 |"));
    }

    #[test]
    fn markdown_omits_the_libvmaf_metadata_lines_when_there_are_no_rows() {
        let markdown = build_markdown(
            "av1",
            "/clips/desktop.y4m",
            "2026-09-26",
            "ffmpeg version 7.1.1",
            &[],
        );
        assert!(!markdown.contains("libvmaf:"));
        assert!(!markdown.contains("Scored frames"));
    }

    #[test]
    fn civil_from_days_matches_known_epoch_day_calendar_dates() {
        // 1970-01-01 is epoch day 0.
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        // 2000-01-01 is epoch day 10957 (a well-known reference point).
        assert_eq!(civil_from_days(10957), (2000, 1, 1));
        // 2024-01-01 is epoch day 19723 (computed independently from a
        // calendar, not from this function).
        assert_eq!(civil_from_days(19723), (2024, 1, 1));
        // 2024 is a leap year: day 19723 + 59 lands on 2024-02-29.
        assert_eq!(civil_from_days(19723 + 59), (2024, 2, 29));
    }
}
