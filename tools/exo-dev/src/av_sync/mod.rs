//! CI regression gate for ExoSnap's clapper A/V-sync analysis.
//!
//! Measures clock drift from a recorded clapper file's flash and beep
//! markers, fits a weighted least-squares drift line across them, and only
//! issues a verdict when the reference signal is good enough to carry one.
//!
//! WHAT THIS MEASURES, AND WHAT IT DOES NOT. The flash and the beep leave
//! ExoSnap's observation through different, uncontrolled emission paths (GPU
//! present -> display capture vs. WASAPI render -> loopback capture), so the
//! start offset carries a device-dependent emission skew that is not an
//! ExoSnap A/V error. That constant skew cancels in the drift (the fitted
//! slope times the span), so the drift is pass/fail while the absolute
//! offset is advisory only, never gated.
//!
//! THE DRIFT IS FITTED, NOT DIFFERENCED. An endpoint difference is the drift
//! only if the drift is linear, and it cannot itself show whether it is: two
//! points always lie exactly on a line. Every marker also carries a real
//! localisation uncertainty, so all markers are fitted by weighted least
//! squares, and the fit's residuals are what says whether a straight line
//! describes the run at all.
//!
//! A VERDICT NEEDS A QUALIFIED REFERENCE. Before any budget is applied, the
//! clapper signal has to be shown good enough to carry the judgement. An
//! unqualified reference produces "could not measure", never a pass.

pub mod drift;
pub mod fixture;
pub mod series;

use std::path::Path;

pub use drift::{
    DriftFit, MarkerPair, Qualification, fit_drift, pair_marker_events, qualify_reference,
    required_marker_count, select_marker_pairs,
};
pub use series::{
    Event, Series, baseline_noise, detect_events, extract_luma_series, extract_rms_series,
    median_sample_interval,
};

/// One recognized marker in the report: the paired flash and beep PTS.
#[derive(Clone, Debug, PartialEq)]
pub struct MarkerReport {
    pub label: &'static str,
    pub flash_s: f64,
    pub beep_s: f64,
    pub offset_ms: f64,
}

/// The full measurement of one recording.
///
/// Distinct from `Result`: "could not measure" is a normal, expected outcome
/// carrying diagnostic fields (`flash_event_count`, `beep_event_count`), not
/// a Rust error. `error` carries the reason when `measurable` is false, or
/// when signal extraction itself failed (ffmpeg missing or unreadable
/// file); both are reported identically to the caller as "could not
/// measure".
#[derive(Clone, Debug, Default)]
pub struct MeasureResult {
    pub file: String,
    pub flash_events: Vec<f64>,
    pub beep_events: Vec<f64>,
    pub flash_event_count: usize,
    pub beep_event_count: usize,
    pub paired_event_count: usize,
    pub measurable: bool,
    pub error: Option<String>,
    pub marker_count: usize,
    pub markers: Vec<MarkerReport>,
    pub recognized_flash_pts: Vec<f64>,
    pub recognized_beep_pts: Vec<f64>,
    pub flash_start_s: f64,
    pub flash_end_s: f64,
    pub beep_start_s: f64,
    pub beep_end_s: f64,
    pub span_s: f64,
    pub offset_start_ms: f64,
    pub offset_middle_ms: Option<f64>,
    pub offset_end_ms: f64,
    pub drift_ms: f64,
    pub drift_start_end_ms: f64,
    pub drift_ms_per_hour: f64,
    pub drift_start_middle_ms: Option<f64>,
    pub drift_middle_end_ms: Option<f64>,
    pub fit: Option<DriftFit>,
    /// The budget qualification was judged against, kept for diagnostics
    /// (e.g. reporting `allowed_uncertainty_ms = budget_ms / 3`).
    pub budget_ms: f64,
    pub reference: Option<Qualification>,
    pub fitted_drift_ms: Option<f64>,
    pub fitted_drift_ms_per_hour: Option<f64>,
    pub fitted_emission_skew_ms: Option<f64>,
    pub fitted_drift_uncertainty_ms: Option<f64>,
    pub max_residual_ms: Option<f64>,
    /// Set only for a 3-marker run: opposing segment drifts that each
    /// exceed the flat `--max-drift-ms` budget but cancel at the endpoint.
    pub segment_reliability_finding: Option<bool>,
}

fn not_measurable(error: impl Into<String>, flashes: &[Event], beeps: &[Event]) -> MeasureResult {
    MeasureResult {
        flash_events: flashes.iter().map(|e| e.time_s).collect(),
        beep_events: beeps.iter().map(|e| e.time_s).collect(),
        flash_event_count: flashes.len(),
        beep_event_count: beeps.len(),
        measurable: false,
        error: Some(error.into()),
        ..Default::default()
    }
}

fn analyze(
    flashes: &[Event],
    beeps: &[Event],
    expected_markers: Option<usize>,
    expected_marker_times_s: Option<&[f64]>,
    max_pair_skew_s: f64,
    schedule_tolerance_s: f64,
    budget_ms: f64,
) -> MeasureResult {
    let pairs = pair_marker_events(flashes, beeps, max_pair_skew_s);
    let paired_event_count = pairs.len();

    let selected = match select_marker_pairs(
        &pairs,
        expected_markers,
        expected_marker_times_s,
        schedule_tolerance_s,
    ) {
        Ok(selected) => selected,
        Err(error) => {
            let mut result = not_measurable(error, flashes, beeps);
            result.paired_event_count = paired_event_count;
            return result;
        }
    };

    let labels: &[&str] = if selected.len() == 2 {
        &["start", "end"]
    } else {
        &["start", "middle", "end"]
    };
    let markers: Vec<MarkerReport> = labels
        .iter()
        .zip(selected.iter())
        .map(|(&label, pair)| MarkerReport {
            label,
            flash_s: pair.flash_pts,
            beep_s: pair.beep_pts,
            offset_ms: pair.offset_s() * 1000.0,
        })
        .collect();

    let offset_start = selected[0].offset_s();
    let offset_end = selected[selected.len() - 1].offset_s();
    let span = selected[selected.len() - 1].beep_pts - selected[0].beep_pts;
    if span <= 1e-6 {
        let mut result = not_measurable("marker PTS are not strictly ordered", flashes, beeps);
        result.paired_event_count = paired_event_count;
        return result;
    }
    let drift = offset_end - offset_start;
    let drift_per_hour = drift / span * 3600.0;

    let mut result = MeasureResult {
        flash_events: flashes.iter().map(|e| e.time_s).collect(),
        beep_events: beeps.iter().map(|e| e.time_s).collect(),
        flash_event_count: flashes.len(),
        beep_event_count: beeps.len(),
        paired_event_count,
        measurable: true,
        marker_count: selected.len(),
        markers,
        recognized_flash_pts: selected.iter().map(|p| p.flash_pts).collect(),
        recognized_beep_pts: selected.iter().map(|p| p.beep_pts).collect(),
        flash_start_s: selected[0].flash_pts,
        flash_end_s: selected[selected.len() - 1].flash_pts,
        beep_start_s: selected[0].beep_pts,
        beep_end_s: selected[selected.len() - 1].beep_pts,
        span_s: span,
        offset_start_ms: offset_start * 1000.0,
        offset_end_ms: offset_end * 1000.0,
        drift_ms: drift * 1000.0,
        drift_start_end_ms: drift * 1000.0,
        drift_ms_per_hour: drift_per_hour * 1000.0,
        ..Default::default()
    };

    if selected.len() == 3 {
        let offset_middle = selected[1].offset_s();
        result.offset_middle_ms = Some(offset_middle * 1000.0);
        result.drift_start_middle_ms = Some((offset_middle - offset_start) * 1000.0);
        result.drift_middle_end_ms = Some((offset_end - offset_middle) * 1000.0);
    }

    // The fitted drift is the verdict's quantity; the endpoint difference
    // above stays as a diagnostic, because a large disagreement between the
    // two is itself the finding that the run was not linear.
    let fit = fit_drift(&selected);
    let reference = qualify_reference(&selected, fit.as_ref(), span, budget_ms);
    if let Some(fit) = &fit {
        let fitted_drift = fit.slope_s_per_s * span;
        let drift_uncertainty_ms = fit.slope_standard_error_s_per_s * span * 1000.0;
        result.fitted_drift_ms = Some(fitted_drift * 1000.0);
        result.fitted_drift_ms_per_hour = Some(fit.slope_s_per_s * 3600.0 * 1000.0);
        result.fitted_emission_skew_ms =
            Some((fit.intercept_s + fit.slope_s_per_s * selected[0].event_pts()) * 1000.0);
        result.fitted_drift_uncertainty_ms = Some(drift_uncertainty_ms);
        result.max_residual_ms = Some(fit.max_abs_residual_s * 1000.0);
    }
    result.fit = fit;
    result.budget_ms = budget_ms;
    result.reference = Some(reference);
    result
}

/// Measures A/V drift of a recorded clapper file: extracts the luma and RMS
/// series with system ffmpeg, detects flash and beep events, and analyzes
/// the recognized markers.
#[allow(clippy::too_many_arguments)]
pub fn measure(
    path: &Path,
    luma_threshold_frac: f64,
    rms_threshold_frac: f64,
    budget_ms: f64,
    expected_markers: Option<usize>,
    expected_marker_times_s: Option<&[f64]>,
    max_pair_skew_s: f64,
    schedule_tolerance_s: f64,
) -> MeasureResult {
    let luma = match extract_luma_series(path) {
        Ok(series) => series,
        Err(error) => {
            let mut result = not_measurable(
                format!("could not extract the luma series: {error:#}"),
                &[],
                &[],
            );
            result.file = path.display().to_string();
            return result;
        }
    };
    let rms = match extract_rms_series(path) {
        Ok(series) => series,
        Err(error) => {
            let mut result = not_measurable(
                format!("could not extract the RMS series: {error:#}"),
                &[],
                &[],
            );
            result.file = path.display().to_string();
            return result;
        }
    };
    let flashes = detect_events(&luma, luma_threshold_frac);
    let beeps = detect_events(&rms, rms_threshold_frac);
    let mut result = analyze(
        &flashes,
        &beeps,
        expected_markers,
        expected_marker_times_s,
        max_pair_skew_s,
        schedule_tolerance_s,
        budget_ms,
    );
    result.file = path.display().to_string();
    result
}

/// Whether a 3-marker run's segments drift in opposite directions, each
/// beyond the flat total-drift budget, and so cancel at the endpoint.
///
/// Uses `--max-drift-ms`, the un-substituted budget, never a
/// `--max-drift-ms-per-hour` rate: the finding is about a shape (opposing
/// segments), not about which budget mode is active.
pub fn segment_reliability_finding(result: &MeasureResult, max_drift_ms: f64) -> Option<bool> {
    if result.marker_count != 3 {
        return None;
    }
    let start_middle = result.drift_start_middle_ms?;
    let middle_end = result.drift_middle_end_ms?;
    Some(start_middle.abs().max(middle_end.abs()) > max_drift_ms && start_middle * middle_end < 0.0)
}

/// The CLI/executor decision inputs: which budget mode is active, and
/// whether an unqualified reference is still allowed to report a verdict.
pub struct VerdictOptions {
    pub unqualified_reference: bool,
    pub max_drift_ms: f64,
    pub max_drift_ms_per_hour: Option<f64>,
}

/// The `main()` decision, factored out: not measurable, an unqualified
/// reference without the override, over budget, or a pass.
#[derive(Clone, Debug, PartialEq)]
pub enum Verdict {
    Pass {
        measured: String,
    },
    OverBudget {
        measured: String,
        budget: String,
    },
    /// Exit 3: either the reference could never be measured at all
    /// (`reasons` names the missing-marker cause), or it was measured but
    /// did not qualify to carry a verdict (`reasons` are the qualification
    /// failures) and `--unqualified-reference` was not passed.
    CouldNotMeasure {
        reasons: Vec<String>,
    },
}

impl Verdict {
    pub fn exit_code(&self) -> i32 {
        match self {
            Verdict::Pass { .. } => 0,
            Verdict::OverBudget { .. } => 2,
            Verdict::CouldNotMeasure { .. } => 3,
        }
    }
}

/// Mirrors the legacy tool's decision exactly: not measurable -> exit 3;
/// reference not qualified and `--unqualified-reference` was NOT passed ->
/// exit 3; fitted drift (or, with a per-hour budget, the fitted rate,
/// falling back to the endpoint rate when unfitted) over budget -> exit 2;
/// else -> exit 0.
pub fn verdict(result: &MeasureResult, opts: &VerdictOptions) -> Verdict {
    if !result.measurable {
        let reason = result
            .error
            .clone()
            .unwrap_or_else(|| "could not measure".to_string());
        return Verdict::CouldNotMeasure {
            reasons: vec![reason],
        };
    }

    let qualified = result
        .reference
        .as_ref()
        .is_some_and(Qualification::qualified);
    if !qualified && !opts.unqualified_reference {
        let mut reasons: Vec<String> = result
            .reference
            .as_ref()
            .map(|q| q.reasons().to_vec())
            .unwrap_or_default();
        if reasons.is_empty() {
            reasons.push("no reason was recorded".to_string());
        }
        return Verdict::CouldNotMeasure { reasons };
    }

    // The fitted slope, not the endpoint difference: an endpoint difference
    // is the drift only if the drift is linear, which the qualification
    // above is what establishes.
    let (measured_value, over, budget) = if let Some(rate_budget) = opts.max_drift_ms_per_hour {
        let rate = result
            .fitted_drift_ms_per_hour
            .unwrap_or(result.drift_ms_per_hour)
            .abs();
        (rate, rate > rate_budget, format!("{rate_budget} ms/hour"))
    } else {
        let total = result.fitted_drift_ms.unwrap_or(result.drift_ms).abs();
        (
            total,
            total > opts.max_drift_ms,
            format!("{} ms", opts.max_drift_ms),
        )
    };

    let mut measured = if opts.max_drift_ms_per_hour.is_some() {
        format!("{measured_value:.2} ms/hour")
    } else {
        format!("{measured_value:.2} ms")
    };
    if let Some(uncertainty) = result.fitted_drift_uncertainty_ms {
        measured.push_str(&format!(" +/-{uncertainty:.2} ms"));
    }

    if over {
        Verdict::OverBudget { measured, budget }
    } else {
        Verdict::Pass { measured }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn events(times: &[f64]) -> Vec<Event> {
        times
            .iter()
            .map(|&t| Event {
                time_s: t,
                sample_interval_s: 0.0,
            })
            .collect()
    }

    fn events_with_interval(times: &[f64], interval: f64) -> Vec<Event> {
        times
            .iter()
            .map(|&t| Event {
                time_s: t,
                sample_interval_s: interval,
            })
            .collect()
    }

    #[test]
    fn analyze_two_markers() {
        let result = analyze(
            &events(&[10.030, 110.042]),
            &events(&[10.000, 110.000]),
            Some(2),
            None,
            0.250,
            2.0,
            20.0,
        );
        assert!(result.measurable);
        assert_eq!(result.marker_count, 2);
        assert!((result.drift_ms - 12.0).abs() < 1e-6);
    }

    #[test]
    fn analyze_three_markers_reports_segment_and_total_drift() {
        let result = analyze(
            &events(&[10.030, 60.050, 110.040]),
            &events(&[10.000, 60.000, 110.000]),
            Some(3),
            None,
            0.250,
            2.0,
            20.0,
        );
        assert!(result.measurable);
        assert!((result.offset_middle_ms.unwrap() - 50.0).abs() < 1e-6);
        assert!((result.drift_start_middle_ms.unwrap() - 20.0).abs() < 1e-6);
        assert!((result.drift_middle_end_ms.unwrap() - -10.0).abs() < 1e-6);
        assert!((result.drift_start_end_ms - 10.0).abs() < 1e-6);
    }

    #[test]
    fn analyze_missing_marker_fails_closed() {
        let result = analyze(
            &events(&[10.030, 110.040]),
            &events(&[10.000, 110.000]),
            Some(3),
            None,
            0.250,
            2.0,
            20.0,
        );
        assert!(!result.measurable);
        assert!(result.error.unwrap().contains("need 3 paired markers"));
    }

    #[test]
    fn measure_reports_could_not_measure_not_a_false_pass_when_events_are_undetectable() {
        // Zero flash events at all: pairing can never produce a marker, and
        // the auto-mode "need exactly 2 or 3" check reports it as not
        // measurable rather than a false pass.
        let result = analyze(&[], &events(&[10.0]), None, None, 0.250, 2.0, 20.0);
        assert!(!result.measurable);
        assert_eq!(result.flash_event_count, 0);
        assert_eq!(result.beep_event_count, 1);
    }

    #[test]
    fn measure_reports_not_qualified_not_a_false_pass_for_an_under_instrumented_but_measurable_clip()
     {
        // Two markers ARE detected and paired (measurable), but
        // qualify_reference refuses a verdict (fewer than three markers):
        // a distinct code path from zero detected events above.
        let result = analyze(
            &events_with_interval(&[10.030, 110.042], 0.016),
            &events_with_interval(&[10.000, 110.000], 0.010),
            None,
            None,
            0.250,
            2.0,
            20.0,
        );
        assert!(result.measurable);
        assert_eq!(result.marker_count, 2);
        assert!(!result.reference.as_ref().unwrap().qualified());

        let opts = VerdictOptions {
            unqualified_reference: false,
            max_drift_ms: 20.0,
            max_drift_ms_per_hour: None,
        };
        assert_eq!(verdict(&result, &opts).exit_code(), 3);
    }

    #[test]
    fn the_fit_and_the_endpoint_difference_disagree_on_a_nonlinear_run() {
        let result = analyze(
            &events_with_interval(&[10.000, 60.060, 110.000], 0.016),
            &events_with_interval(&[10.000, 60.000, 110.000], 0.010),
            Some(3),
            None,
            0.250,
            2.0,
            20.0,
        );
        assert!(result.measurable);
        assert!((result.drift_start_end_ms - 0.0).abs() < 1e-6);
        assert!(!result.reference.unwrap().qualified());
    }

    #[test]
    fn a_clean_run_carries_the_fitted_drift() {
        let result = analyze(
            &events_with_interval(&[10.000, 60.010, 110.020], 0.002),
            &events_with_interval(&[10.000, 60.000, 110.000], 0.002),
            Some(3),
            None,
            0.250,
            2.0,
            20.0,
        );
        assert!(result.reference.as_ref().unwrap().qualified());
        assert!((result.fitted_drift_ms.unwrap() - 20.0).abs() < 0.01);
        assert!(result.fitted_drift_uncertainty_ms.unwrap() > 0.0);
    }

    fn verdict_gate_fixture(qualified: bool, fitted_drift_ms: f64) -> MeasureResult {
        MeasureResult {
            measurable: true,
            marker_count: 3,
            span_s: 100.0,
            offset_start_ms: 30.0,
            offset_middle_ms: Some(35.0),
            offset_end_ms: 40.0,
            drift_start_middle_ms: Some(5.0),
            drift_middle_end_ms: Some(5.0),
            drift_start_end_ms: 10.0,
            drift_ms: 10.0,
            drift_ms_per_hour: 360.0,
            flash_event_count: 3,
            beep_event_count: 3,
            paired_event_count: 3,
            recognized_flash_pts: vec![10.0, 60.0, 110.0],
            recognized_beep_pts: vec![10.0, 60.0, 110.0],
            fitted_drift_ms: Some(fitted_drift_ms),
            fitted_drift_ms_per_hour: Some(fitted_drift_ms * 36.0),
            fitted_drift_uncertainty_ms: Some(2.0),
            max_residual_ms: Some(1.0),
            reference: Some(if qualified {
                Qualification::Qualified
            } else {
                Qualification::NotQualified {
                    reasons: vec!["the fitted drift is uncertain to +/-13.34 ms".to_string()],
                }
            }),
            ..Default::default()
        }
    }

    #[test]
    fn an_unqualified_reference_is_not_a_pass() {
        let result = verdict_gate_fixture(false, 4.0);
        let opts = VerdictOptions {
            unqualified_reference: false,
            max_drift_ms: 20.0,
            max_drift_ms_per_hour: None,
        };
        assert_eq!(verdict(&result, &opts).exit_code(), 3);
    }

    #[test]
    fn an_unqualified_reference_is_not_a_failure_either() {
        // Exit 3 is "could not measure". Reporting a drift the reference
        // cannot carry as a product failure (exit 2) is the same error in
        // the other direction.
        let result = verdict_gate_fixture(false, 400.0);
        let opts = VerdictOptions {
            unqualified_reference: false,
            max_drift_ms: 20.0,
            max_drift_ms_per_hour: None,
        };
        assert_eq!(verdict(&result, &opts).exit_code(), 3);
    }

    #[test]
    fn a_qualified_reference_within_budget_passes() {
        let result = verdict_gate_fixture(true, 4.0);
        let opts = VerdictOptions {
            unqualified_reference: false,
            max_drift_ms: 20.0,
            max_drift_ms_per_hour: None,
        };
        assert!(matches!(verdict(&result, &opts), Verdict::Pass { .. }));
    }

    #[test]
    fn a_qualified_reference_over_budget_fails() {
        let result = verdict_gate_fixture(true, 40.0);
        let opts = VerdictOptions {
            unqualified_reference: false,
            max_drift_ms: 20.0,
            max_drift_ms_per_hour: None,
        };
        assert_eq!(verdict(&result, &opts).exit_code(), 2);
    }

    #[test]
    fn the_verdict_reads_the_fitted_drift_not_the_endpoint_difference() {
        // The fixture's endpoint difference is 10 ms, inside the budget;
        // its fitted drift is 40 ms, outside it. A gate still reading the
        // endpoint would pass.
        let result = verdict_gate_fixture(true, 40.0);
        let opts = VerdictOptions {
            unqualified_reference: false,
            max_drift_ms: 20.0,
            max_drift_ms_per_hour: None,
        };
        assert_eq!(verdict(&result, &opts).exit_code(), 2);
    }

    #[test]
    fn the_override_reports_a_verdict_without_a_qualified_reference() {
        let result = verdict_gate_fixture(false, 4.0);
        let opts = VerdictOptions {
            unqualified_reference: true,
            max_drift_ms: 20.0,
            max_drift_ms_per_hour: None,
        };
        assert!(matches!(verdict(&result, &opts), Verdict::Pass { .. }));
    }

    #[test]
    fn segment_reliability_finding_fires_on_opposing_segment_drifts_over_budget() {
        let mut result = verdict_gate_fixture(true, 10.0);
        result.drift_start_middle_ms = Some(25.0);
        result.drift_middle_end_ms = Some(-25.0);
        assert_eq!(segment_reliability_finding(&result, 20.0), Some(true));
    }

    #[test]
    fn segment_reliability_finding_is_none_off_a_3_marker_run() {
        let mut result = verdict_gate_fixture(true, 10.0);
        result.marker_count = 2;
        assert_eq!(segment_reliability_finding(&result, 20.0), None);
    }

    #[test]
    fn segment_reliability_finding_uses_the_flat_budget_not_a_per_hour_one() {
        let mut result = verdict_gate_fixture(true, 10.0);
        result.drift_start_middle_ms = Some(25.0);
        result.drift_middle_end_ms = Some(-25.0);
        // Even though a per-hour budget would be active on the CLI, this
        // finding always reads --max-drift-ms.
        assert_eq!(segment_reliability_finding(&result, 20.0), Some(true));
        assert_eq!(segment_reliability_finding(&result, 30.0), Some(false));
    }

    /// Whether a real system ffmpeg is on PATH. `EXO_DEV_REQUIRE_FFMPEG=1`
    /// turns a missing ffmpeg from a skip into a failure, for a CI lane that
    /// is supposed to have one.
    fn require_ffmpeg_or_skip(test_name: &str) -> bool {
        let available = std::process::Command::new("ffmpeg")
            .arg("-version")
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .is_ok();
        if !available {
            if std::env::var("EXO_DEV_REQUIRE_FFMPEG").as_deref() == Ok("1") {
                panic!("{test_name}: EXO_DEV_REQUIRE_FFMPEG=1 but ffmpeg is not on PATH");
            }
            eprintln!("{test_name}: skipped, ffmpeg is not on PATH");
        }
        available
    }

    #[test]
    fn measure_reads_the_committed_golden_clip_as_a_qualified_five_marker_run() {
        if !require_ffmpeg_or_skip("measure_reads_the_committed_golden_clip") {
            return;
        }
        let path = Path::new(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../tests/fixtures/av-sync/clapper-golden.mp4"
        ));
        let result = measure(path, 0.7, 0.5, 25.0, Some(5), None, 0.250, 2.0);
        assert!(result.measurable, "{:?}", result.error);
        assert_eq!(result.marker_count, 5);
        assert!(result.reference.as_ref().unwrap().qualified());
        assert!(result.fitted_drift_ms.unwrap().abs() < 25.0);
    }

    #[test]
    fn measure_reads_a_freshly_generated_two_marker_clip_as_measurable_but_not_qualified() {
        if !require_ffmpeg_or_skip("measure_reads_a_freshly_generated_two_marker_clip") {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let out = dir.path().join("two-marker.mp4");
        let spec = fixture::FixtureSpec {
            duration: 2.0,
            markers: 2,
            out: out.clone(),
        };
        fixture::generate(&spec).expect("ffmpeg is on PATH, generation must succeed");

        let result = measure(&out, 0.7, 0.5, 20.0, None, None, 0.250, 2.0);
        assert!(result.measurable, "{:?}", result.error);
        assert_eq!(result.marker_count, 2);
        assert!(!result.reference.as_ref().unwrap().qualified());

        let opts = VerdictOptions {
            unqualified_reference: false,
            max_drift_ms: 20.0,
            max_drift_ms_per_hour: None,
        };
        assert_eq!(verdict(&result, &opts).exit_code(), 3);
    }

    #[test]
    fn measure_reads_a_freshly_generated_four_marker_clip_as_not_measurable_in_auto_mode() {
        if !require_ffmpeg_or_skip("measure_reads_a_freshly_generated_four_marker_clip") {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let out = dir.path().join("four-marker.mp4");
        let spec = fixture::FixtureSpec {
            duration: 3.0,
            markers: 4,
            out: out.clone(),
        };
        fixture::generate(&spec).expect("ffmpeg is on PATH, generation must succeed");

        let result = measure(&out, 0.7, 0.5, 20.0, None, None, 0.250, 2.0);
        assert!(!result.measurable);
        assert_eq!(result.flash_event_count, 4);
        assert_eq!(result.beep_event_count, 4);

        let opts = VerdictOptions {
            unqualified_reference: false,
            max_drift_ms: 20.0,
            max_drift_ms_per_hour: None,
        };
        assert_eq!(verdict(&result, &opts).exit_code(), 3);
    }
}
