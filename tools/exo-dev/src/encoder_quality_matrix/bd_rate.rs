//! Bjontegaard-delta rate (BD-rate) scoring between two 4-point rate/quality
//! curves, with a stdlib-only (no external linear-algebra crate) cubic fit.

use anyhow::{bail, ensure};

/// Backstop against a literal division-by-(near-)zero pivot inside
/// [`solve_linear_system`]. It is not the singularity/ill-conditioning
/// detector for [`polyfit3`]'s curve fits: see `FIT_RESIDUAL_TOL` and
/// `polyfit3`'s doc comment for why pivot magnitude, even after
/// equilibration, cannot reliably tell a genuinely degenerate fit from a
/// legitimate one. Ordinary, well-conditioned, evenly-spaced high-VMAF
/// sweeps (for example [97, 97.5, 98, 98.5] or [99.5, 99.6, 99.7, 99.8]) can
/// equilibrate down to pivots in the 6e-16 to 2e-15 range, overlapping the
/// genuinely degenerate clustered case's roughly 2e-16 to 5e-16 floor. No
/// fixed pivot-magnitude threshold sits between those two ranges. This
/// constant exists only so an exact (or floating-point-exact) zero pivot,
/// for example duplicate x-values collapsing a column to identically zero,
/// raises a clear error instead of dividing by zero. It should essentially
/// never fire on real data.
const ZERO_PIVOT_TOL: f64 = 1e-300;

/// Singularity/ill-conditioning tolerance for [`polyfit3`], applied to the
/// fitted cubic's residual at the original sample points. Legitimate
/// sweeps top out around 1.2e-8 max residual (observed worst case, metrics
/// [99, 99.2, 99.4, 99.6], up to about 1.5e-7 across a similar set), while a
/// genuinely clustered/near-duplicate case measures about 4.3e-4, four to
/// five orders of magnitude higher. 1e-5 sits well inside that gap: about
/// 800x above the worst legitimate residual seen and about 40x below the
/// degenerate case's residual.
const FIT_RESIDUAL_TOL: f64 = 1e-5;

/// BD-rate (Bjontegaard-delta rate) between curve A (baseline) and curve B
/// (candidate), in percent. Negative means B needs less bitrate than A for
/// the same quality (an improvement); positive means B is worse.
///
/// Each curve is exactly 4 (rate, metric) points, matching this tool's own
/// matrix sweep (4 CQ points, 4 VBR points per preset; see
/// [`super::matrix::default_matrix`]). Rates are log-transformed (the
/// standard BD-rate construction: rate-distortion curves are close to
/// linear in log(rate) vs. quality), fit with a cubic polynomial, and the
/// integral of the fitted log-rate over the metric range common to both
/// curves is compared. This mirrors the piecewise log-interpolation
/// approach used by reference BD-rate implementations, reimplemented here
/// with no external linear-algebra dependency.
///
/// Exactly 4 points is a real constraint, not an arbitrary minimum. With 4
/// points, [`polyfit3`]'s cubic fit is an exact interpolation (4 points, 4
/// degrees of freedom), which is what makes its fit-residual singularity
/// check meaningful. A curve of 5 or more points would be a genuine
/// least-squares approximation with an inherent nonzero residual even when
/// well-conditioned, indistinguishable by that check from actual
/// ill-conditioning, so 5 or more points is rejected rather than silently
/// mismeasured.
pub fn bd_rate(
    rates_a: &[f64],
    metrics_a: &[f64],
    rates_b: &[f64],
    metrics_b: &[f64],
) -> anyhow::Result<f64> {
    ensure!(
        rates_a.len() == 4 && rates_b.len() == 4,
        "bd_rate needs exactly 4 (rate, metric) points per curve"
    );
    ensure!(
        rates_a.len() == metrics_a.len() && rates_b.len() == metrics_b.len(),
        "rates and metrics must be the same length"
    );

    let log_rates_a: Vec<f64> = rates_a.iter().map(|r| r.log10()).collect();
    let log_rates_b: Vec<f64> = rates_b.iter().map(|r| r.log10()).collect();

    let coeffs_a = polyfit3(metrics_a, &log_rates_a)?;
    let coeffs_b = polyfit3(metrics_b, &log_rates_b)?;

    let lo = min_of(metrics_a).max(min_of(metrics_b));
    let hi = max_of(metrics_a).min(max_of(metrics_b));
    ensure!(hi > lo, "curves do not overlap in quality range");

    let integral_a = (polyint3(&coeffs_a, hi) - polyint3(&coeffs_a, lo)) / (hi - lo);
    let integral_b = (polyint3(&coeffs_b, hi) - polyint3(&coeffs_b, lo)) / (hi - lo);

    Ok((10f64.powf(integral_b - integral_a) - 1.0) * 100.0)
}

fn min_of(values: &[f64]) -> f64 {
    values.iter().copied().fold(f64::INFINITY, f64::min)
}

fn max_of(values: &[f64]) -> f64 {
    values.iter().copied().fold(f64::NEG_INFINITY, f64::max)
}

/// Least-squares cubic fit: returns `[c0, c1, c2, c3]` for
/// `y = c0 + c1*x + c2*x^2 + c3*x^3`, solving the 4x4 normal equations
/// directly with Gaussian elimination.
///
/// With exactly 4 (x, y) sample points, the only size this is ever called
/// with (see [`bd_rate`]), the normal-equations solve is not a lossy
/// least-squares approximation: it is an exact interpolation of all 4
/// points, since a cubic has exactly 4 degrees of freedom. That gives a
/// cheap, reliable way to detect whether the solve was numerically
/// trustworthy: after solving, re-evaluate the fitted cubic at the original
/// x values and compare against the y values. For a well-conditioned
/// system, that residual is near machine precision (about 1e-8 or smaller,
/// measured against real sweep data); for an ill-conditioned one (near-
/// duplicate x values), rounding error during elimination blows the
/// residual up to about 1e-4 or worse, even though the solve itself
/// completes without a literal division error. This residual is the
/// discriminator used here, not the pivot magnitude inside
/// [`solve_linear_system`] (see `FIT_RESIDUAL_TOL` and `ZERO_PIVOT_TOL`
/// above for why pivot magnitude alone cannot separate the two cases).
pub(super) fn polyfit3(x: &[f64], y: &[f64]) -> anyhow::Result<[f64; 4]> {
    let n = x.len();
    // x^0..x^6 per sample point, needed for the normal equations below.
    // `powi` (exponentiation by squaring), not a running-product chain: a
    // chain of plain multiplications rounds differently at each step and
    // measurably diverges from Python's `xi**p` (which uses the same
    // squaring approach) once x and the exponent are large enough to make
    // that rounding path matter.
    let powers: Vec<[f64; 7]> = x
        .iter()
        .map(|&xi| std::array::from_fn(|p| xi.powi(p as i32)))
        .collect();

    // Normal equations: A^T A c = A^T y, where A's columns are x^0..x^3.
    let mut ata = [[0.0f64; 4]; 4];
    for a in 0..4 {
        for b in 0..4 {
            ata[a][b] = (0..n).map(|i| powers[i][a + b]).sum();
        }
    }
    let aty: [f64; 4] = std::array::from_fn(|a| (0..n).map(|i| powers[i][a] * y[i]).sum());

    let coeffs = solve_linear_system(&ata, &aty)?;
    let [c0, c1, c2, c3] = coeffs;

    let max_residual = x
        .iter()
        .zip(y.iter())
        .map(|(&xi, &yi)| ((c0 + c1 * xi + c2 * xi * xi + c3 * xi * xi * xi) - yi).abs())
        .fold(0.0f64, f64::max);
    if max_residual > FIT_RESIDUAL_TOL {
        bail!(
            "curve fit is unreliable (max residual {max_residual:.3e} > {FIT_RESIDUAL_TOL:.0e} \
             at the sample points). The 4 (metric, rate) points do not interpolate cleanly. \
             Check the input data."
        );
    }

    Ok(coeffs)
}

/// Definite-integral-to-`x` of `c0 + c1*x + c2*x^2 + c3*x^3`.
fn polyint3(coeffs: &[f64; 4], x: f64) -> f64 {
    let [c0, c1, c2, c3] = *coeffs;
    c0 * x + c1 * x * x / 2.0 + c2 * x.powi(3) / 3.0 + c3 * x.powi(4) / 4.0
}

/// Solves `a * x = b` for a 4x4 matrix `a` via Gaussian elimination with
/// partial pivoting. Fixed at 4x4: this is only ever called from
/// [`polyfit3`]'s normal equations for a cubic fit through exactly 4 sample
/// points.
///
/// `a` is always `polyfit3`'s normal-equations matrix A^T A: symmetric
/// positive-semidefinite by construction, with `a[i][i]` equal to the sum
/// of x^(2i) over the quality-metric sample points. Its raw entries span
/// many orders of magnitude by design, not because of ill-conditioning:
/// row/column 0 holds x^0..x^3 sums (roughly 1 to 1e6 for VMAF-range x),
/// row/column 3 holds x^3..x^6 sums (roughly 1e6 to 1e12 or more). That
/// spread exists identically for perfectly well-separated sample points
/// (for example metrics [95, 96, 97, 98], an ordinary quality sweep) and
/// for genuinely near-duplicate ones alike.
///
/// This function symmetrically equilibrates `a` before elimination: it
/// solves `(D A D) x' = D b` for `x' = D^-1 x`, where `D = diag(1 /
/// sqrt(a[i][i]))`. This is the standard "correlation matrix"
/// transformation for a symmetric positive-definite matrix: it rescales
/// every row and column so the diagonal is exactly 1, and by
/// Cauchy-Schwarz every off-diagonal entry of the equilibrated matrix is
/// bounded to [-1, 1] regardless of the original matrix's scale. This is
/// kept purely for the numerical-stability benefit during elimination (it
/// measurably reduces rounding error in the fitted coefficients), not as a
/// singularity decision: using the equilibrated pivot magnitude itself as
/// the singularity signal, compared against a fixed threshold, does not
/// separate a genuinely degenerate fit from a legitimate one (see
/// [`polyfit3`]'s doc comment and `FIT_RESIDUAL_TOL` above). The
/// singularity decision lives entirely in `polyfit3`'s post-fit residual
/// check. The only check left here is `ZERO_PIVOT_TOL`, a loose backstop
/// against a literal (near-)zero pivot so elimination raises a clear error
/// instead of dividing by zero. It is not expected to fire on real data.
/// Row swaps during partial pivoting only reorder equations, never
/// unknowns, so `D` needs no bookkeeping through the swaps, only the final
/// `x = D x'` undo at the end.
// The elimination pass indexes both `m[r]` and `m[col]` per column, which an
// iterator adapter cannot express without extra indirection given the
// borrow split between the two rows.
#[allow(clippy::needless_range_loop)]
fn solve_linear_system(a: &[[f64; 4]; 4], b: &[f64; 4]) -> anyhow::Result<[f64; 4]> {
    const N: usize = 4;

    // d[i] = 1/sqrt(a[i][i]): the per-unknown equilibration scale. a[i][i]
    // is a sum of even powers of the sample points, so it is always
    // positive unless that power's column carries no information at all
    // (for example every sample point is exactly 0), a genuine
    // degenerate/singular case.
    let mut d = [0.0f64; N];
    for i in 0..N {
        let diag = a[i][i];
        ensure!(diag > 0.0, "singular matrix in bd_rate curve fit");
        d[i] = 1.0 / diag.sqrt();
    }

    // Build the equilibrated augmented matrix once, then run ordinary
    // Gaussian elimination with partial pivoting.
    let mut m = [[0.0f64; N + 1]; N];
    for i in 0..N {
        for j in 0..N {
            m[i][j] = a[i][j] * d[i] * d[j];
        }
        m[i][N] = b[i] * d[i];
    }

    for col in 0..N {
        // The first row of the largest magnitude, not the last: Python's
        // `max(..., key=...)` keeps the first maximal element on a tie, and
        // an ill-conditioned matrix can have two candidate pivots close
        // enough that a different tie-break picks a different row, which
        // then diverges through elimination.
        let mut pivot = col;
        for r in (col + 1)..N {
            if m[r][col].abs() > m[pivot][col].abs() {
                pivot = r;
            }
        }
        ensure!(
            m[pivot][col].abs() >= ZERO_PIVOT_TOL,
            "singular matrix in bd_rate curve fit (exact zero pivot)"
        );
        m.swap(col, pivot);
        for r in 0..N {
            if r == col {
                continue;
            }
            let factor = m[r][col] / m[col][col];
            for c in col..=N {
                m[r][c] -= factor * m[col][c];
            }
        }
    }

    let mut x = [0.0f64; N];
    for i in 0..N {
        x[i] = m[i][N] / m[i][i] * d[i];
    }
    Ok(x)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identical_curves_score_near_zero() {
        let rates = [1000.0, 2000.0, 4000.0, 8000.0];
        let metrics = [80.0, 85.0, 90.0, 93.0];
        let result = bd_rate(&rates, &metrics, &rates, &metrics).unwrap();
        assert!(result.abs() < 0.01, "got {result}");
    }

    #[test]
    fn half_bitrate_candidate_scores_near_negative_50_percent() {
        let rates = [1000.0, 2000.0, 4000.0, 8000.0];
        let metrics = [80.0, 85.0, 90.0, 93.0];
        let rates_b: Vec<f64> = rates.iter().map(|r| r / 2.0).collect();
        let result = bd_rate(&rates, &metrics, &rates_b, &metrics).unwrap();
        assert!((-55.0..-45.0).contains(&result), "got {result}");
    }

    #[test]
    fn double_bitrate_candidate_scores_near_positive_100_percent() {
        let rates = [1000.0, 2000.0, 4000.0, 8000.0];
        let metrics = [80.0, 85.0, 90.0, 93.0];
        let rates_b: Vec<f64> = rates.iter().map(|r| r * 2.0).collect();
        let result = bd_rate(&rates, &metrics, &rates_b, &metrics).unwrap();
        assert!((95.0..105.0).contains(&result), "got {result}");
    }

    #[test]
    fn exactly_three_points_is_rejected() {
        let rates = [1000.0, 2000.0, 4000.0, 8000.0];
        let metrics = [80.0, 85.0, 90.0, 93.0];
        assert!(bd_rate(&[1.0, 2.0, 3.0], &[1.0, 2.0, 3.0], &rates, &metrics).is_err());
    }

    #[test]
    fn exactly_five_points_is_rejected() {
        let rates = [1000.0, 2000.0, 4000.0, 8000.0];
        let metrics = [80.0, 85.0, 90.0, 93.0];
        assert!(
            bd_rate(
                &[1000.0, 2000.0, 4000.0, 6000.0, 8000.0],
                &[80.0, 85.0, 88.0, 90.0, 93.0],
                &rates,
                &metrics
            )
            .is_err()
        );
    }

    #[test]
    fn clustered_near_duplicate_metrics_trip_the_fit_residual_guard() {
        let metrics_clustered = [90.0000, 90.0001, 90.0002, 90.0003];
        let rates_clustered: [f64; 4] = [500.0, 501.0, 503.0, 506.0];
        let log_rates: Vec<f64> = rates_clustered.iter().map(|r| r.log10()).collect();
        assert!(polyfit3(&metrics_clustered, &log_rates).is_err());
    }

    #[test]
    fn polyfit3_reproduces_a_known_cubic_on_noiseless_points() {
        // y = 2 - 3x + 0.5x^2 + 0.1x^3, sampled at 4 well-separated points.
        let f = |x: f64| 2.0 - 3.0 * x + 0.5 * x * x + 0.1 * x * x * x;
        let x = [1.0, 2.0, 4.0, 7.0];
        let y: Vec<f64> = x.iter().map(|&xi| f(xi)).collect();
        let coeffs = polyfit3(&x, &y).unwrap();
        let expected = [2.0, -3.0, 0.5, 0.1];
        for (got, want) in coeffs.iter().zip(expected.iter()) {
            assert!(
                (got - want).abs() < 1e-8,
                "got {coeffs:?}, want {expected:?}"
            );
        }
    }

    #[test]
    fn high_vmaf_evenly_spaced_sweeps_do_not_trip_the_fit_residual_guard() {
        let fa = |x: f64| -2.0 + 0.05 * x;
        for sweep in [
            [97.0, 97.5, 98.0, 98.5],
            [99.0, 99.2, 99.4, 99.6],
            [99.5, 99.6, 99.7, 99.8],
        ] {
            let rates: Vec<f64> = sweep.iter().map(|&x| 10f64.powf(fa(x))).collect();
            let result = bd_rate(&rates, &sweep, &rates, &sweep)
                .unwrap_or_else(|e| panic!("sweep {sweep:?} incorrectly raised: {e}"));
            assert!(result.abs() < 0.01, "sweep {sweep:?} got {result}");
        }
    }

    #[test]
    fn differently_shaped_partial_overlap_curves_match_hand_derived_bd_rate() {
        let metrics_a = [80.0, 85.0, 90.0, 95.0];
        let metrics_b = [81.0, 84.0, 91.0, 94.0];
        let fa = |x: f64| -2.0 + 0.05 * x;
        let rates_a: Vec<f64> = metrics_a.iter().map(|&x| 10f64.powf(fa(x))).collect();
        let rates_b: Vec<f64> = metrics_b
            .iter()
            .map(|&x| 10f64.powf(fa(x) + 0.001 * (x - 90.0).powi(2)))
            .collect();
        let result = bd_rate(&rates_a, &metrics_a, &rates_b, &metrics_b).unwrap();
        let (lo, hi): (f64, f64) = (81.0, 94.0);
        let avg_quad = ((hi - 90.0).powi(3) - (lo - 90.0).powi(3)) / (3.0 * (hi - lo));
        let expected = (10f64.powf(0.001 * avg_quad) - 1.0) * 100.0;
        assert!(
            (result - expected).abs() < 1e-6,
            "got {result}, want {expected}"
        );
    }

    /// Values recorded from the ported Python script's own `bd_rate` on the
    /// same inputs, computed independently of this Rust implementation, to
    /// prove numeric parity beyond the ported self-test's own cases.
    #[test]
    #[allow(clippy::type_complexity)]
    fn matches_python_bd_rate_on_additional_synthetic_curve_pairs() {
        let cases: [(&[f64], &[f64], &[f64], &[f64], f64); 5] = [
            (
                &[5000.0, 8000.0, 14000.0, 22000.0],
                &[82.0, 88.0, 93.5, 96.0],
                &[4200.0, 7100.0, 12500.0, 20000.0],
                &[82.5, 88.2, 93.6, 96.1],
                -13.432065852518516,
            ),
            (
                &[1000.0, 3000.0, 9000.0, 20000.0],
                &[70.0, 82.0, 90.0, 95.0],
                &[1200.0, 3100.0, 8500.0, 18000.0],
                &[69.0, 82.5, 90.5, 95.5],
                -1.3939188380534318,
            ),
            (
                &[
                    1000.0,
                    1202.2644346174131,
                    1367.7288255958495,
                    1499.684835502374,
                ],
                &[97.0, 98.0, 98.7, 99.2],
                &[
                    907.8205301781852,
                    1081.4339512979375,
                    1230.2687708123824,
                    1348.9628825916548,
                ],
                &[97.1, 98.05, 98.75, 99.25],
                -10.874903712825446,
            ),
            (
                &[2000.0, 4000.0, 8000.0, 16000.0],
                &[75.0, 84.0, 91.0, 96.0],
                &[2500.0, 4600.0, 8200.0, 15000.0],
                &[78.0, 85.0, 90.0, 94.0],
                8.829582069580066,
            ),
            (
                &[3000.0, 5000.0, 9000.0, 15000.0],
                &[85.0, 89.0, 93.0, 96.5],
                &[2250.0, 3750.0, 6750.0, 11250.0],
                &[85.0, 89.0, 93.0, 96.5],
                -25.000000000131283,
            ),
        ];
        for (rates_a, metrics_a, rates_b, metrics_b, expected) in cases {
            let result = bd_rate(rates_a, metrics_a, rates_b, metrics_b).unwrap();
            assert!(
                (result - expected).abs() < 1e-9,
                "got {result}, want {expected} for {rates_a:?}/{metrics_a:?} vs {rates_b:?}/{metrics_b:?}"
            );
        }
    }
}
