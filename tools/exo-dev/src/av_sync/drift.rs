//! Marker pairing and selection, and the weighted least-squares drift fit
//! and reference qualification gate that decide whether a fitted drift can
//! carry a verdict at all.

use crate::av_sync::series::Event;

/// One recognized marker: the paired flash and beep edges, and the
/// localisation uncertainty each edge carries.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MarkerPair {
    pub flash_pts: f64,
    pub beep_pts: f64,
    pub flash_uncertainty_s: f64,
    pub beep_uncertainty_s: f64,
}

impl MarkerPair {
    pub fn new(
        flash_pts: f64,
        beep_pts: f64,
        flash_uncertainty_s: f64,
        beep_uncertainty_s: f64,
    ) -> Self {
        Self {
            flash_pts,
            beep_pts,
            flash_uncertainty_s,
            beep_uncertainty_s,
        }
    }

    pub fn event_pts(self) -> f64 {
        (self.flash_pts + self.beep_pts) / 2.0
    }

    pub fn offset_s(self) -> f64 {
        self.flash_pts - self.beep_pts
    }

    /// Uncertainty of this marker's offset, from two independent edge
    /// locations.
    pub fn offset_uncertainty_s(self) -> f64 {
        (self.flash_uncertainty_s.powi(2) + self.beep_uncertainty_s.powi(2)).sqrt()
    }
}

/// Greedily pairs the globally closest cross-stream edges within the skew
/// limit.
pub fn pair_marker_events(
    flashes: &[Event],
    beeps: &[Event],
    max_pair_skew_s: f64,
) -> Vec<MarkerPair> {
    let mut candidates: Vec<(f64, usize, usize)> = Vec::new();
    for (flash_index, flash) in flashes.iter().enumerate() {
        for (beep_index, beep) in beeps.iter().enumerate() {
            let skew = (flash.time_s - beep.time_s).abs();
            if skew <= max_pair_skew_s {
                candidates.push((skew, flash_index, beep_index));
            }
        }
    }
    // Stable sort: ties keep the (flash_index, beep_index) generation order,
    // matching a plain Python `sorted(..., key=lambda c: c[0])`.
    candidates.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap());

    let mut used_flashes = std::collections::HashSet::new();
    let mut used_beeps = std::collections::HashSet::new();
    let mut pairs = Vec::new();
    for (_, flash_index, beep_index) in candidates {
        if used_flashes.contains(&flash_index) || used_beeps.contains(&beep_index) {
            continue;
        }
        used_flashes.insert(flash_index);
        used_beeps.insert(beep_index);
        pairs.push(MarkerPair::new(
            flashes[flash_index].time_s,
            beeps[beep_index].time_s,
            flashes[flash_index].uncertainty_s(),
            beeps[beep_index].uncertainty_s(),
        ));
    }
    pairs.sort_by(|a, b| a.event_pts().partial_cmp(&b.event_pts()).unwrap());
    pairs
}

/// How unevenly a candidate marker set is spaced, as max interval minus min.
fn interval_spread(candidate: &[MarkerPair]) -> f64 {
    let intervals: Vec<f64> = candidate
        .windows(2)
        .map(|w| w[1].event_pts() - w[0].event_pts())
        .collect();
    if intervals.is_empty() {
        return 0.0;
    }
    let max = intervals.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    let min = intervals.iter().copied().fold(f64::INFINITY, f64::min);
    max - min
}

/// Every `k`-combination of `items`, in the same lexicographic-by-index
/// order as Python's `itertools.combinations`.
fn combinations<T: Copy>(items: &[T], k: usize) -> Vec<Vec<T>> {
    let n = items.len();
    let mut result = Vec::new();
    if k == 0 {
        result.push(Vec::new());
        return result;
    }
    if k > n {
        return result;
    }
    let mut idx: Vec<usize> = (0..k).collect();
    loop {
        result.push(idx.iter().map(|&i| items[i]).collect());
        let mut pos = k;
        while pos > 0 {
            pos -= 1;
            if idx[pos] < pos + n - k {
                break;
            }
        }
        if idx[pos] >= pos + n - k {
            break;
        }
        idx[pos] += 1;
        for j in (pos + 1)..k {
            idx[j] = idx[j - 1] + 1;
        }
    }
    result
}

/// Selects a complete ordered marker set without silently ignoring
/// disturbances. `Err` carries the human-readable reason no set qualifies.
pub fn select_marker_pairs(
    pairs: &[MarkerPair],
    expected_markers: Option<usize>,
    expected_marker_times_s: Option<&[f64]>,
    schedule_tolerance_s: f64,
) -> Result<Vec<MarkerPair>, String> {
    let Some(expected_markers) = expected_markers else {
        // Auto mode stays at two or three. It has no schedule to check
        // against, so the only thing separating "five markers" from "three
        // markers and two disturbances" is the count it was told to expect,
        // and a run with more markers always knows its own schedule,
        // because the clapper prints it.
        if pairs.len() != 2 && pairs.len() != 3 {
            return Err(format!(
                "need exactly 2 or 3 paired markers in auto mode, found {}; pass --expected-markers for a longer schedule",
                pairs.len()
            ));
        }
        return Ok(pairs.to_vec());
    };

    if pairs.len() < expected_markers {
        return Err(format!(
            "need {expected_markers} paired markers, found {}",
            pairs.len()
        ));
    }
    if let Some(times) = expected_marker_times_s
        && times.len() != expected_markers
    {
        return Err("marker schedule length does not match --expected-markers".to_string());
    }

    let combos = combinations(pairs, expected_markers);

    let Some(times) = expected_marker_times_s else {
        // Python's min()/max() return the FIRST element on a tie; Rust's
        // Iterator::max_by returns the LAST, so both selections below use an
        // explicit fold to keep first-encountered semantics.
        let selected = if expected_markers == 2 {
            let mut best: Option<(&Vec<MarkerPair>, f64)> = None;
            for candidate in &combos {
                let span = candidate[candidate.len() - 1].event_pts() - candidate[0].event_pts();
                if best.is_none_or(|(_, best_span)| span > best_span) {
                    best = Some((candidate, span));
                }
            }
            best.unwrap().0.clone()
        } else {
            // The most evenly spaced candidate, then the widest. A clapper
            // emits on a regular schedule, so the set whose intervals vary
            // least is the one that is the schedule rather than the one
            // that happens to include a disturbance.
            let mut best: Option<(&Vec<MarkerPair>, f64, f64)> = None;
            for candidate in &combos {
                let spread = interval_spread(candidate);
                let neg_span =
                    -(candidate[candidate.len() - 1].event_pts() - candidate[0].event_pts());
                let better = match best {
                    None => true,
                    Some((_, best_spread, best_neg_span)) => {
                        spread < best_spread || (spread == best_spread && neg_span < best_neg_span)
                    }
                };
                if better {
                    best = Some((candidate, spread, neg_span));
                }
            }
            best.unwrap().0.clone()
        };
        return Ok(selected);
    };

    let expected_intervals: Vec<f64> = (1..expected_markers)
        .map(|index| times[index] - times[index - 1])
        .collect();
    if expected_intervals.iter().any(|&interval| interval <= 0.0) {
        return Err("expected marker times must be strictly increasing".to_string());
    }

    let schedule_error = |candidate: &[MarkerPair]| -> f64 {
        (1..expected_markers)
            .map(|index| {
                let observed = candidate[index].event_pts() - candidate[index - 1].event_pts();
                (observed - expected_intervals[index - 1]).abs()
            })
            .fold(f64::NEG_INFINITY, f64::max)
    };

    let mut selected: Option<(&Vec<MarkerPair>, f64)> = None;
    for candidate in &combos {
        let error = schedule_error(candidate);
        if selected.is_none_or(|(_, best_error)| error < best_error) {
            selected = Some((candidate, error));
        }
    }
    let (selected, selected_error) = selected.unwrap();
    if selected_error > schedule_tolerance_s {
        return Err(format!(
            "paired markers do not match the expected schedule (max interval error {selected_error:.3}s > {schedule_tolerance_s:.3}s)"
        ));
    }
    Ok(selected.clone())
}

/// A weighted least-squares fit of marker offset against time.
///
/// Weighted rather than ordinary: the markers do not carry equal
/// uncertainty. A flash that lands after a dropped frame is located less
/// precisely than one in a clean stretch, and an ordinary fit would give
/// both the same say.
#[derive(Clone, Debug, PartialEq)]
pub struct DriftFit {
    pub slope_s_per_s: f64,
    pub intercept_s: f64,
    pub slope_standard_error_s_per_s: f64,
    pub residuals_s: Vec<f64>,
    pub max_abs_residual_s: f64,
    pub marker_uncertainties_s: Vec<f64>,
}

/// Fits `markers` by weighted least squares. `None` when fewer than two
/// markers are given, or every marker sits at the same time (zero time
/// variance, so no slope is determined).
pub fn fit_drift(markers: &[MarkerPair]) -> Option<DriftFit> {
    if markers.len() < 2 {
        return None;
    }

    let weights: Vec<f64> = markers
        .iter()
        .map(|marker| {
            let sigma = marker.offset_uncertainty_s();
            // A marker with no stated uncertainty is not infinitely
            // precise, it is unmeasured. Weighting it as exact would
            // silence every marker that did state one.
            if sigma > 0.0 {
                1.0 / (sigma * sigma)
            } else {
                1.0
            }
        })
        .collect();
    let times: Vec<f64> = markers.iter().map(|marker| marker.event_pts()).collect();
    let offsets: Vec<f64> = markers.iter().map(|marker| marker.offset_s()).collect();

    let weight_sum: f64 = weights.iter().sum();
    let mean_time: f64 = weights.iter().zip(&times).map(|(w, t)| w * t).sum::<f64>() / weight_sum;
    let mean_offset: f64 = weights
        .iter()
        .zip(&offsets)
        .map(|(w, o)| w * o)
        .sum::<f64>()
        / weight_sum;

    let covariance: f64 = weights
        .iter()
        .zip(&times)
        .zip(&offsets)
        .map(|((w, t), o)| w * (t - mean_time) * (o - mean_offset))
        .sum();
    let variance: f64 = weights
        .iter()
        .zip(&times)
        .map(|(w, t)| w * (t - mean_time).powi(2))
        .sum();
    if variance <= 0.0 {
        return None;
    }

    let slope = covariance / variance;
    let intercept = mean_offset - slope * mean_time;
    let residuals: Vec<f64> = times
        .iter()
        .zip(&offsets)
        .map(|(t, o)| o - (intercept + slope * t))
        .collect();
    // Standard error from the stated uncertainties, not from the residual
    // spread: with three markers the residual-based estimate has one degree
    // of freedom and is worthless, while the edge uncertainties are known
    // from the sampling rates.
    let slope_standard_error = (1.0 / variance).sqrt();
    let max_abs_residual = residuals
        .iter()
        .copied()
        .fold(0.0_f64, |acc, r| acc.max(r.abs()));
    let marker_uncertainties: Vec<f64> = markers
        .iter()
        .map(|marker| marker.offset_uncertainty_s())
        .collect();

    Some(DriftFit {
        slope_s_per_s: slope,
        intercept_s: intercept,
        slope_standard_error_s_per_s: slope_standard_error,
        residuals_s: residuals,
        max_abs_residual_s: max_abs_residual,
        marker_uncertainties_s: marker_uncertainties,
    })
}

/// How many evenly spaced markers a budget needs at a given edge precision.
///
/// The uncertainty of the TOTAL drift does not shrink with a longer run: the
/// slope is determined better over a longer span, and the span it is
/// multiplied by grows by the same factor. Only more markers (or sharper
/// edges) help, which is worth saying in the verdict rather than leaving a
/// campaign to lengthen a run that cannot get more precise that way.
///
/// Returns `None` when no marker count within `limit` would do, which means
/// the edges themselves have to get sharper, and also when either input is
/// non-positive.
pub fn required_marker_count(
    marker_sigma_s: f64,
    allowed_uncertainty_ms: f64,
    limit: usize,
) -> Option<usize> {
    if marker_sigma_s <= 0.0 || allowed_uncertainty_ms <= 0.0 {
        return None;
    }
    for count in 3..=limit {
        let count = count as f64;
        // For `count` markers spread evenly over a span, the slope's
        // standard error times that span reduces to
        // sigma * sqrt(12(n-1) / (n(n+1))), the span cancels.
        let factor = (12.0 * (count - 1.0) / (count * (count + 1.0))).sqrt();
        if marker_sigma_s * factor * 1000.0 <= allowed_uncertainty_ms {
            return Some(count as usize);
        }
    }
    None
}

/// Whether the clapper signal can carry a drift verdict at all.
#[derive(Clone, Debug, PartialEq)]
pub enum Qualification {
    Qualified,
    NotQualified { reasons: Vec<String> },
}

impl Qualification {
    pub fn qualified(&self) -> bool {
        matches!(self, Qualification::Qualified)
    }

    pub fn reasons(&self) -> &[String] {
        match self {
            Qualification::Qualified => &[],
            Qualification::NotQualified { reasons } => reasons,
        }
    }
}

const REQUIRED_MARKER_COUNT_LIMIT: usize = 200;

/// Decides whether the clapper signal can carry a drift verdict at all.
///
/// Three conditions, each of which a real run has failed, ALL evaluated and
/// ALL failing reasons collected:
///
/// * At least three markers. Two define a line exactly, so their residuals
///   are zero by construction and nonlinearity is invisible.
/// * The drift the fit could be wrong by, over the measured span, is well
///   under the budget. Otherwise "within budget" and "too noisy to tell"
///   produce the same answer.
/// * Every marker lies within its own edge uncertainty of the fitted line. A
///   marker further out means the offset did not move linearly, and then no
///   single rate, fitted or differenced, describes the run.
pub fn qualify_reference(
    markers: &[MarkerPair],
    fit: Option<&DriftFit>,
    span_s: f64,
    budget_ms: f64,
) -> Qualification {
    const UNCERTAINTY_FRACTION: f64 = 1.0 / 3.0;
    const RESIDUAL_SIGMAS: f64 = 3.0;

    let Some(fit) = fit else {
        let reason = if markers.len() < 2 {
            "a fit needs at least two markers"
        } else {
            "every marker sits at the same time"
        };
        return Qualification::NotQualified {
            reasons: vec![reason.to_string()],
        };
    };

    let mut reasons = Vec::new();

    if markers.len() < 3 {
        reasons.push(format!(
            "{} markers cannot show nonlinearity; two always lie exactly on a line",
            markers.len()
        ));
    }

    let drift_uncertainty_ms = fit.slope_standard_error_s_per_s * span_s * 1000.0;
    let allowed_uncertainty_ms = budget_ms * UNCERTAINTY_FRACTION;
    if drift_uncertainty_ms > allowed_uncertainty_ms {
        let mut stated: Vec<f64> = fit
            .marker_uncertainties_s
            .iter()
            .copied()
            .filter(|&u| u > 0.0)
            .collect();
        stated.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let typical_sigma = if stated.is_empty() {
            0.0
        } else {
            stated[stated.len() / 2]
        };
        let needed = required_marker_count(
            typical_sigma,
            allowed_uncertainty_ms,
            REQUIRED_MARKER_COUNT_LIMIT,
        );
        let remedy = match needed {
            Some(count) => {
                format!("; {count} evenly spaced markers would reach it at this edge precision")
            }
            None => "; sharpen the marker edges or lengthen the run".to_string(),
        };
        reasons.push(format!(
            "the fitted drift is uncertain to +/-{drift_uncertainty_ms:.2} ms over the span, more than the {allowed_uncertainty_ms:.2} ms a {budget_ms:.2} ms budget can be judged against{remedy}"
        ));
    }

    let uncertainties = &fit.marker_uncertainties_s;
    if uncertainties.iter().any(|&sigma| sigma > 0.0) {
        // Each marker against ITS OWN edges, not against the median of all
        // of them: the markers do not carry equal uncertainty, and a median
        // allowance both accuses the well-measured markers and excuses the
        // poorly measured ones.
        let mut worst_index: Option<usize> = None;
        let mut worst_excess = 0.0;
        for (index, (&residual, &sigma)) in
            fit.residuals_s.iter().zip(uncertainties.iter()).enumerate()
        {
            if sigma <= 0.0 {
                continue;
            }
            let excess = residual.abs() - RESIDUAL_SIGMAS * sigma;
            if excess > worst_excess {
                worst_excess = excess;
                worst_index = Some(index);
            }
        }
        if let Some(index) = worst_index {
            let allowed = RESIDUAL_SIGMAS * uncertainties[index];
            reasons.push(format!(
                "marker {} sits {:.2} ms off the fitted line, beyond the {:.2} ms its own edges allow; the drift is not linear",
                index + 1,
                fit.residuals_s[index].abs() * 1000.0,
                allowed * 1000.0
            ));
        }
    } else {
        reasons.push(
            "no marker carries an edge uncertainty, so the fit cannot be told from a coincidence"
                .to_string(),
        );
    }

    if reasons.is_empty() {
        Qualification::Qualified
    } else {
        Qualification::NotQualified { reasons }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn markers(pairs: &[(f64, f64)], flash_sigma: f64, beep_sigma: f64) -> Vec<MarkerPair> {
        pairs
            .iter()
            .map(|&(flash, beep)| MarkerPair::new(flash, beep, flash_sigma, beep_sigma))
            .collect()
    }

    #[test]
    fn required_marker_count_matches_the_formula_and_can_return_none() {
        let sigma = (0.008_f64.powi(2) + 0.005_f64.powi(2)).sqrt();
        let allowed_ms = 20.0 / 3.0;
        let needed = required_marker_count(sigma, allowed_ms, 200).unwrap();
        let factor =
            (12.0 * (needed as f64 - 1.0) / (needed as f64 * (needed as f64 + 1.0))).sqrt();
        assert!(sigma * factor * 1000.0 <= allowed_ms);
        let factor_prev =
            (12.0 * (needed as f64 - 2.0) / ((needed as f64 - 1.0) * needed as f64)).sqrt();
        assert!(sigma * factor_prev * 1000.0 > allowed_ms);

        // A precision no marker count within the limit can reach returns None.
        assert_eq!(required_marker_count(1.0, 0.0001, 10), None);
        // Non-positive inputs return None immediately.
        assert_eq!(required_marker_count(0.0, 20.0, 200), None);
        assert_eq!(required_marker_count(0.01, 0.0, 200), None);
        assert_eq!(required_marker_count(-1.0, 20.0, 200), None);
    }

    #[test]
    fn qualify_reference_refuses_a_verdict_with_fewer_than_three_markers() {
        let selected = markers(&[(10.000, 10.000), (110.020, 110.000)], 0.008, 0.005);
        let fit = fit_drift(&selected);
        let verdict = qualify_reference(&selected, fit.as_ref(), 100.0, 20.0);
        assert!(!verdict.qualified());
        assert!(verdict.reasons().iter().any(|r| r.contains("nonlinearity")));
    }

    #[test]
    fn qualify_reference_passes_three_well_spaced_markers_with_zero_true_drift_and_tight_edges() {
        let selected = markers(
            &[(10.000, 10.000), (60.000, 60.000), (110.000, 110.000)],
            0.002,
            0.001,
        );
        let fit = fit_drift(&selected);
        let verdict = qualify_reference(&selected, fit.as_ref(), 100.0, 20.0);
        assert!(verdict.qualified(), "{:?}", verdict.reasons());
    }

    #[test]
    fn qualify_reference_collects_every_failing_reason_not_just_the_first() {
        // Two markers (fails the count floor) with a budget too tight for
        // the fit's own uncertainty (fails the slope-uncertainty check).
        // Both reasons must appear, not just the first one found.
        let selected = markers(&[(10.000, 10.000), (110.020, 110.000)], 0.008, 0.005);
        let fit = fit_drift(&selected);
        let verdict = qualify_reference(&selected, fit.as_ref(), 100.0, 0.001);
        assert!(!verdict.qualified());
        let reasons = verdict.reasons();
        assert!(reasons.iter().any(|r| r.contains("nonlinearity")));
        assert!(reasons.iter().any(|r| r.contains("uncertain")));
    }

    #[test]
    fn qualify_reference_three_frame_accurate_markers_cannot_carry_a_20_ms_verdict() {
        let selected = markers(
            &[(10.000, 10.000), (60.010, 60.000), (110.020, 110.000)],
            0.008,
            0.005,
        );
        let fit = fit_drift(&selected);
        let verdict = qualify_reference(&selected, fit.as_ref(), 100.0, 20.0);
        assert!(!verdict.qualified());
        assert!(verdict.reasons().iter().any(|r| r.contains("uncertain")));
        assert!(
            verdict
                .reasons()
                .iter()
                .any(|r| r.contains("markers would reach it"))
        );
    }

    #[test]
    fn qualify_reference_enough_markers_at_the_same_edge_precision_do_qualify() {
        let sigma = (0.008_f64.powi(2) + 0.005_f64.powi(2)).sqrt();
        let needed = required_marker_count(sigma, 20.0 / 3.0, 200).unwrap();
        let selected: Vec<MarkerPair> = (0..needed)
            .map(|index| {
                MarkerPair::new(
                    10.0 + index as f64 * 10.0,
                    10.0 + index as f64 * 10.0,
                    0.008,
                    0.005,
                )
            })
            .collect();
        let span = selected.last().unwrap().event_pts() - selected[0].event_pts();
        let fit = fit_drift(&selected);
        let verdict = qualify_reference(&selected, fit.as_ref(), span, 20.0);
        assert!(verdict.qualified(), "{:?}", verdict.reasons());
    }

    #[test]
    fn qualify_reference_two_markers_do_not_qualify() {
        let selected = markers(&[(10.000, 10.000), (110.020, 110.000)], 0.008, 0.005);
        let fit = fit_drift(&selected);
        let verdict = qualify_reference(&selected, fit.as_ref(), 100.0, 20.0);
        assert!(!verdict.qualified());
        assert!(verdict.reasons().iter().any(|r| r.contains("nonlinearity")));
    }

    #[test]
    fn qualify_reference_a_budget_below_the_measurement_precision_does_not_qualify() {
        let selected = markers(
            &[(10.000, 10.000), (60.010, 60.000), (110.020, 110.000)],
            0.002,
            0.001,
        );
        let fit = fit_drift(&selected);
        assert!(qualify_reference(&selected, fit.as_ref(), 100.0, 20.0).qualified());
        let tight = qualify_reference(&selected, fit.as_ref(), 100.0, 1.0);
        assert!(!tight.qualified());
        assert!(tight.reasons().iter().any(|r| r.contains("uncertain")));
    }

    #[test]
    fn qualify_reference_a_residual_is_judged_against_that_markers_own_edges() {
        let selected = vec![
            MarkerPair::new(10.000, 10.000, 0.0005, 0.0005),
            MarkerPair::new(60.012, 60.000, 0.0300, 0.0300),
            MarkerPair::new(110.000, 110.000, 0.0005, 0.0005),
        ];
        let fit = fit_drift(&selected);
        let verdict = qualify_reference(&selected, fit.as_ref(), 100.0, 60.0);
        assert!(verdict.qualified(), "{:?}", verdict.reasons());
    }

    #[test]
    fn qualify_reference_a_nonlinear_run_does_not_qualify() {
        let selected = markers(
            &[(10.000, 10.000), (60.060, 60.000), (110.000, 110.000)],
            0.008,
            0.005,
        );
        let fit = fit_drift(&selected);
        let verdict = qualify_reference(&selected, fit.as_ref(), 100.0, 20.0);
        assert!(!verdict.qualified());
        assert!(verdict.reasons().iter().any(|r| r.contains("not linear")));
    }

    #[test]
    fn qualify_reference_markers_without_stated_uncertainty_do_not_qualify() {
        let selected = markers(&[(10.0, 10.0), (60.01, 60.0), (110.02, 110.0)], 0.0, 0.0);
        let fit = fit_drift(&selected);
        let verdict = qualify_reference(&selected, fit.as_ref(), 100.0, 20.0);
        assert!(!verdict.qualified());
        assert!(verdict.reasons().iter().any(|r| r.contains("coincidence")));
    }

    #[test]
    fn fit_drift_a_linear_drift_is_recovered() {
        let selected = markers(
            &[(10.000, 10.000), (60.010, 60.000), (110.020, 110.000)],
            0.008,
            0.005,
        );
        let fit = fit_drift(&selected).unwrap();
        assert!((fit.slope_s_per_s - 0.0002).abs() < 1e-6);
        assert!(fit.max_abs_residual_s < 1e-9);
    }

    #[test]
    fn fit_drift_a_less_certain_marker_pulls_the_fit_less() {
        let clean = fit_drift(&[
            MarkerPair::new(10.000, 10.000, 0.001, 0.001),
            MarkerPair::new(60.030, 60.000, 0.001, 0.001),
            MarkerPair::new(110.020, 110.000, 0.001, 0.001),
        ])
        .unwrap();
        let downweighted = fit_drift(&[
            MarkerPair::new(10.000, 10.000, 0.001, 0.001),
            MarkerPair::new(60.030, 60.000, 0.100, 0.100),
            MarkerPair::new(110.020, 110.000, 0.001, 0.001),
        ])
        .unwrap();
        assert!(downweighted.residuals_s[1].abs() > clean.residuals_s[1].abs());
        assert!((clean.slope_s_per_s - downweighted.slope_s_per_s).abs() > 1e-9);
    }

    #[test]
    fn fit_drift_two_markers_are_fitted_but_leave_no_residual() {
        let selected = markers(&[(10.000, 10.000), (110.020, 110.000)], 0.008, 0.005);
        let fit = fit_drift(&selected).unwrap();
        assert!(fit.max_abs_residual_s < 1e-12);
    }

    #[test]
    fn fit_drift_the_total_drift_uncertainty_does_not_shrink_with_a_longer_run() {
        let sigma = (0.008_f64.powi(2) + 0.005_f64.powi(2)).sqrt();
        let short = vec![
            MarkerPair::new(0.0, 0.0, 0.008, 0.005),
            MarkerPair::new(50.0, 50.0, 0.008, 0.005),
            MarkerPair::new(100.0, 100.0, 0.008, 0.005),
        ];
        let long_run = vec![
            MarkerPair::new(0.0, 0.0, 0.008, 0.005),
            MarkerPair::new(500.0, 500.0, 0.008, 0.005),
            MarkerPair::new(1000.0, 1000.0, 0.008, 0.005),
        ];
        let short_total = fit_drift(&short).unwrap().slope_standard_error_s_per_s * 100.0;
        let long_total = fit_drift(&long_run).unwrap().slope_standard_error_s_per_s * 1000.0;
        assert!((short_total - long_total).abs() < 1e-9);
        assert!((short_total - sigma * 2f64.sqrt()).abs() < 1e-9);
    }

    #[test]
    fn select_marker_pairs_a_longer_schedule_is_selected_when_it_is_declared() {
        let pairs: Vec<MarkerPair> = [
            (10.02, 10.00),
            (35.02, 35.00),
            (60.02, 60.00),
            (85.02, 85.00),
            (110.02, 110.00),
        ]
        .iter()
        .map(|&(flash, beep)| MarkerPair::new(flash, beep, 0.002, 0.002))
        .collect();
        let selected = select_marker_pairs(&pairs, Some(5), None, 2.0).unwrap();
        assert_eq!(selected.len(), 5);
    }

    #[test]
    fn select_marker_pairs_the_evenly_spaced_candidate_is_preferred_over_a_disturbance() {
        let pairs: Vec<MarkerPair> = [
            (10.02, 10.00),
            (22.02, 22.00),
            (35.02, 35.00),
            (60.02, 60.00),
            (85.02, 85.00),
            (110.02, 110.00),
        ]
        .iter()
        .map(|&(flash, beep)| MarkerPair::new(flash, beep, 0.002, 0.002))
        .collect();
        let selected = select_marker_pairs(&pairs, Some(5), None, 2.0).unwrap();
        let beeps: Vec<f64> = selected.iter().map(|p| p.beep_pts).collect();
        assert_eq!(beeps, vec![10.0, 35.0, 60.0, 85.0, 110.0]);
    }

    #[test]
    fn select_marker_pairs_expected_schedule_selects_markers_around_extra_disturbances() {
        let pairs: Vec<MarkerPair> = [
            (10.030, 10.000),
            (30.020, 30.000),
            (60.050, 60.000),
            (110.040, 110.000),
        ]
        .iter()
        .map(|&(flash, beep)| MarkerPair::new(flash, beep, 0.0, 0.0))
        .collect();
        let selected =
            select_marker_pairs(&pairs, Some(3), Some(&[10.0, 60.0, 110.0]), 2.0).unwrap();
        let beeps: Vec<f64> = selected.iter().map(|p| p.beep_pts).collect();
        assert_eq!(beeps, vec![10.0, 60.0, 110.0]);
    }

    #[test]
    fn select_marker_pairs_wrong_schedule_order_fails_closed() {
        let pairs: Vec<MarkerPair> = [(10.030, 10.000), (45.050, 45.000), (110.040, 110.000)]
            .iter()
            .map(|&(flash, beep)| MarkerPair::new(flash, beep, 0.0, 0.0))
            .collect();
        let error =
            select_marker_pairs(&pairs, Some(3), Some(&[10.0, 60.0, 110.0]), 2.0).unwrap_err();
        assert!(error.contains("expected schedule"));
    }

    #[test]
    fn select_marker_pairs_extra_disturbances_are_rejected_in_auto_mode() {
        let pairs: Vec<MarkerPair> = [
            (10.030, 10.000),
            (30.020, 30.000),
            (60.050, 60.000),
            (110.040, 110.000),
        ]
        .iter()
        .map(|&(flash, beep)| MarkerPair::new(flash, beep, 0.0, 0.0))
        .collect();
        let error = select_marker_pairs(&pairs, None, None, 2.0).unwrap_err();
        assert!(error.contains("auto mode"));
        assert!(error.contains("--expected-markers"));
    }
}
