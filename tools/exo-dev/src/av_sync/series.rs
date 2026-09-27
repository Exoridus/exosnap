//! Signal extraction: shells to system ffmpeg to sample luma and audio RMS
//! level over time, then locates rising-edge events (a flash or a beep) in
//! the resulting series.

use std::path::Path;
use std::process::Stdio;
use std::sync::LazyLock;

use anyhow::Context as _;
use regex::Regex;

use crate::process;

/// A rising edge above threshold: where a flash or beep first crossed it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Event {
    pub time_s: f64,
    /// The sampling gap the edge was interpolated across (or its floor or
    /// cap), not the series' nominal interval. A marker that lands right
    /// after a dropped sample is located less precisely than one in a clean
    /// stretch, and the verdict has to carry that difference.
    pub sample_interval_s: f64,
}

impl Event {
    /// Half-width of the interval the true edge lies in.
    pub fn uncertainty_s(self) -> f64 {
        self.sample_interval_s / 2.0
    }
}

/// A time-value sample series, e.g. luma or audio RMS level over the clip.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Series {
    pub times: Vec<f64>,
    pub values: Vec<f64>,
}

static FRAME_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"pts_time:([-+0-9.eEnaif]+)").unwrap());
static YAVG_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"lavfi\.signalstats\.YAVG=([-+0-9.eEnaif]+)").unwrap());
static RMS_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"lavfi\.astats\.Overall\.RMS_level=([-+0-9.eEnaif]+)").unwrap());

fn to_float(token: &str, default: f64) -> f64 {
    let token = token.trim().to_lowercase();
    if matches!(token.as_str(), "-inf" | "inf" | "nan" | "-nan") {
        return default;
    }
    token.parse::<f64>().unwrap_or(default)
}

/// Runs ffmpeg and returns its stdout, where the metadata `file=-` sink
/// writes. ffmpeg's own logs go to stderr and are discarded.
fn run_metadata(args: &[&str]) -> anyhow::Result<String> {
    let mut command = process::command("ffmpeg");
    command
        .args(args)
        .stdin(Stdio::null())
        .stderr(Stdio::null());
    let output = command.output().context("could not run ffmpeg")?;
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

/// `metadata=print` emits a `frame:.. pts_time:X` line then a `key=value`
/// line.
fn pair_series(text: &str, value_re: &Regex, silence_default: f64) -> Series {
    let mut series = Series::default();
    let mut pending_time: Option<f64> = None;
    for line in text.lines() {
        if let Some(caps) = FRAME_RE.captures(line) {
            pending_time = Some(to_float(&caps[1], -1.0));
            continue;
        }
        if let Some(caps) = value_re.captures(line)
            && let Some(t) = pending_time
            && t >= 0.0
        {
            series.times.push(t);
            series.values.push(to_float(&caps[1], silence_default));
            pending_time = None;
        }
    }
    series
}

pub fn extract_luma_series(path: &Path) -> anyhow::Result<Series> {
    let path = path.to_string_lossy();
    let out = run_metadata(&[
        "-hide_banner",
        "-nostats",
        "-i",
        path.as_ref(),
        "-vf",
        "signalstats,metadata=mode=print:file=-",
        "-an",
        "-f",
        "null",
        "-",
    ])?;
    Ok(pair_series(&out, &YAVG_RE, 0.0))
}

pub fn extract_rms_series(path: &Path) -> anyhow::Result<Series> {
    let path = path.to_string_lossy();
    let out = run_metadata(&[
        "-hide_banner",
        "-nostats",
        "-i",
        path.as_ref(),
        "-af",
        "astats=metadata=1:reset=1,ametadata=mode=print:file=-",
        "-vn",
        "-f",
        "null",
        "-",
    ])?;
    // Silence reads as -inf dB; floor it well below any real beep.
    Ok(pair_series(&out, &RMS_RE, -120.0))
}

/// Standard deviation of the samples below threshold.
///
/// This is what sets how precisely an edge can be located: a clean flash
/// against a still desktop is locatable to a fraction of a frame, the same
/// flash over moving content is not, and the verdict has to carry the
/// difference rather than assume the clean case.
pub fn baseline_noise(series: &Series, threshold: f64) -> f64 {
    let quiet: Vec<f64> = series
        .values
        .iter()
        .copied()
        .filter(|&v| v < threshold)
        .collect();
    if quiet.len() < 2 {
        return 0.0;
    }
    let mean = quiet.iter().sum::<f64>() / quiet.len() as f64;
    let variance =
        quiet.iter().map(|v| (v - mean).powi(2)).sum::<f64>() / (quiet.len() as f64 - 1.0);
    variance.sqrt()
}

/// The series' own sampling period, as the median gap between samples.
pub fn median_sample_interval(series: &Series) -> f64 {
    if series.times.len() < 2 {
        return 0.0;
    }
    let mut gaps: Vec<f64> = series
        .times
        .windows(2)
        .map(|w| w[1] - w[0])
        .filter(|&gap| gap > 0.0)
        .collect();
    if gaps.is_empty() {
        return 0.0;
    }
    gaps.sort_by(|a, b| a.partial_cmp(b).unwrap());
    gaps[gaps.len() / 2]
}

/// Locates a threshold crossing between two samples, and says how well.
///
/// Taking the first sample above threshold puts the edge anywhere in the
/// preceding sampling gap, so a 60 fps capture locates a flash to +/-8 ms,
/// and with three markers that is +/-13 ms of drift over the whole span,
/// which cannot carry a 20 ms verdict. It is also biased: every edge lands
/// systematically half a gap late. Interpolating removes the bias and, when
/// the signal really does rise across the gap, most of the spread.
///
/// The uncertainty returned is the standard edge-jitter estimate, the
/// baseline noise divided by the slope at the crossing, floored by the
/// quantisation of the sampling grid and capped at half the gap, which is
/// what an uninterpolated crossing is worth. A step that goes from dark to
/// bright inside one sample carries no sub-sample information and lands at
/// that cap.
fn interpolate_crossing(
    previous: Option<(f64, f64)>,
    time: f64,
    value: f64,
    threshold: f64,
    gap: f64,
    noise: f64,
) -> (f64, f64) {
    let Some((previous_time, previous_value)) = previous else {
        return (time, if gap > 0.0 { gap / 2.0 } else { 0.0 });
    };
    if gap <= 0.0 {
        return (time, 0.0);
    }

    let rise = value - previous_value;
    if rise <= 0.0 {
        return (time, gap / 2.0);
    }

    let fraction = (threshold - previous_value) / rise;
    if !(0.0..=1.0).contains(&fraction) {
        return (time, gap / 2.0);
    }

    let crossing = previous_time + fraction * gap;
    // Jitter = noise / slope. The slope here is rise per gap, so the time
    // jitter is (noise / rise) * gap.
    let jitter = (noise / rise) * gap;
    // The floor is the grid's own quantisation: an interpolated crossing
    // cannot be known better than the uniform-distribution standard
    // deviation of one gap.
    let floor = gap / 12f64.sqrt();
    (crossing, jitter.max(floor).min(gap / 2.0))
}

/// Rising edges where the value crosses a fraction of its own dynamic range.
pub fn detect_events(series: &Series, threshold_frac: f64) -> Vec<Event> {
    if series.values.len() < 2 {
        return Vec::new();
    }
    let lo = series.values.iter().copied().fold(f64::INFINITY, f64::min);
    let hi = series
        .values
        .iter()
        .copied()
        .fold(f64::NEG_INFINITY, f64::max);
    if hi - lo < 1e-6 {
        return Vec::new();
    }
    let threshold = lo + threshold_frac * (hi - lo);

    let mut events = Vec::new();
    let mut above = false;
    let mut previous: Option<(f64, f64)> = None;
    let nominal = median_sample_interval(series);
    let noise = baseline_noise(series, threshold);

    for (&t, &v) in series.times.iter().zip(series.values.iter()) {
        if v >= threshold && !above {
            // The gap to the last sample below threshold, not the series
            // median: a marker that lands right after a dropped frame is
            // located less precisely than one in a clean stretch, and the
            // verdict has to know that.
            let gap = match previous {
                None => nominal,
                Some((previous_time, _)) => (t - previous_time).max(0.0),
            };
            let (crossing, residual_gap) =
                interpolate_crossing(previous, t, v, threshold, gap, noise);
            events.push(Event {
                time_s: crossing,
                sample_interval_s: residual_gap,
            });
            above = true;
        } else if v < threshold {
            above = false;
        }
        previous = Some((t, v));
    }
    events
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_crossing_is_interpolated_not_rounded_up_to_a_sample() {
        let series = Series {
            times: vec![0.0, 0.04, 0.08, 0.12],
            values: vec![0.0, 0.0, 1.0, 1.0],
        };
        let found = detect_events(&series, 0.5);
        assert_eq!(found.len(), 1);
        assert!((found[0].time_s - 0.06).abs() < 1e-9);
    }

    #[test]
    fn a_noiseless_step_is_worth_the_grid_quantisation() {
        let series = Series {
            times: vec![0.0, 0.04, 0.08, 0.12],
            values: vec![0.0, 0.0, 1.0, 1.0],
        };
        let found = detect_events(&series, 0.5);
        let expected = 0.04 / 12f64.sqrt() / 2.0;
        assert!((found[0].uncertainty_s() - expected).abs() < 1e-9);
    }

    #[test]
    fn an_edge_after_a_gap_is_located_less_precisely() {
        let clean = detect_events(
            &Series {
                times: vec![0.0, 0.04, 0.08, 0.12],
                values: vec![0.0, 0.0, 1.0, 1.0],
            },
            0.5,
        );
        let after_gap = detect_events(
            &Series {
                times: vec![0.0, 0.04, 0.28, 0.32],
                values: vec![0.0, 0.0, 1.0, 1.0],
            },
            0.5,
        );
        assert!(after_gap[0].uncertainty_s() > clean[0].uncertainty_s());
    }

    #[test]
    fn a_noisy_baseline_widens_the_edge() {
        let (_, quiet) = interpolate_crossing(Some((0.0, 0.0)), 0.04, 1.0, 0.5, 0.04, 0.0);
        let (_, noisy) = interpolate_crossing(Some((0.0, 0.0)), 0.04, 1.0, 0.5, 0.04, 0.4);
        assert!(noisy > quiet);
    }

    #[test]
    fn the_edge_uncertainty_is_capped_at_an_uninterpolated_crossing() {
        let (_, wild) = interpolate_crossing(Some((0.0, 0.0)), 0.04, 1.0, 0.5, 0.04, 100.0);
        assert!((wild - 0.02).abs() < 1e-9);
    }

    #[test]
    fn baseline_noise_measures_the_quiet_samples() {
        let series = Series {
            times: vec![0.0, 0.04, 0.08, 0.12],
            values: vec![0.0, 0.2, 0.0, 1.0],
        };
        assert!((baseline_noise(&series, 0.5) - 0.115_470_05).abs() < 1e-6);
    }
}
