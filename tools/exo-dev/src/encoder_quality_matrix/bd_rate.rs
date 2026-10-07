//! Shape-preserving BD-rate over the common quality interval.

use anyhow::ensure;
use serde::Serialize;

pub(super) const METHOD: &str = "pchip-log-rate-common-overlap-v1";

#[derive(Serialize)]
pub(super) struct Comparison {
    pub percent: f64,
    pub overlap: [f64; 2],
    pub overlap_fraction: f64,
    pub baseline_monotone: bool,
    pub candidate_monotone: bool,
    pub interpolation_sanity: bool,
    /// Quality, baseline kbps, candidate kbps, relative rate difference percent.
    pub relative_curve: Vec<[f64; 4]>,
}

struct Curve {
    x: [f64; 4],
    y: [f64; 4],
    slopes: [f64; 4],
}

impl Curve {
    fn new(rates: &[f64], quality: &[f64]) -> anyhow::Result<Self> {
        ensure!(
            rates.len() == 4 && quality.len() == 4,
            "BD-rate requires exactly four measured points per curve"
        );
        ensure!(
            rates.iter().all(|v| v.is_finite() && *v > 0.0),
            "bitrates must be finite and positive"
        );
        ensure!(
            quality.iter().all(|v| v.is_finite()),
            "quality must be finite"
        );
        let mut points: Vec<_> = quality
            .iter()
            .copied()
            .zip(rates.iter().map(|v| v.ln()))
            .collect();
        points.sort_by(|a, b| a.0.total_cmp(&b.0));
        let x = std::array::from_fn(|i| points[i].0);
        let y = std::array::from_fn(|i| points[i].1);
        let h: [f64; 3] = std::array::from_fn(|i| x[i + 1] - x[i]);
        ensure!(
            h.iter().all(|v| v.is_finite() && *v > 0.0),
            "duplicate quality or invalid quality spacing; points are not merged"
        );
        let d: [f64; 3] = std::array::from_fn(|i| (y[i + 1] - y[i]) / h[i]);
        let mut slopes = [0.0; 4];
        slopes[0] = endpoint(h[0], h[1], d[0], d[1]);
        slopes[3] = endpoint(h[2], h[1], d[2], d[1]);
        for i in 1..3 {
            if d[i - 1] * d[i] > 0.0 {
                let w1 = 2.0 * h[i] + h[i - 1];
                let w2 = h[i] + 2.0 * h[i - 1];
                slopes[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
            }
        }
        ensure!(
            slopes.iter().all(|v| v.is_finite()),
            "non-finite PCHIP slopes"
        );
        Ok(Self { x, y, slopes })
    }

    fn coefficients(&self, i: usize) -> [f64; 4] {
        let h = self.x[i + 1] - self.x[i];
        let delta = self.y[i + 1] - self.y[i];
        // Normalized local coordinates avoid cancellation at saturated quality values.
        [
            self.y[i],
            h * self.slopes[i],
            3.0 * delta - h * (2.0 * self.slopes[i] + self.slopes[i + 1]),
            -2.0 * delta + h * (self.slopes[i] + self.slopes[i + 1]),
        ]
    }

    fn at(&self, quality: f64) -> anyhow::Result<f64> {
        ensure!(
            quality.is_finite() && quality >= self.x[0] && quality <= self.x[3],
            "PCHIP extrapolation is forbidden"
        );
        let i = (0..3).find(|&i| quality <= self.x[i + 1]).unwrap();
        let t = (quality - self.x[i]) / (self.x[i + 1] - self.x[i]);
        let [a, b, c, d] = self.coefficients(i);
        let value = ((d * t + c) * t + b) * t + a;
        ensure!(
            value.is_finite()
                && value >= self.y[i].min(self.y[i + 1]) - 1e-10
                && value <= self.y[i].max(self.y[i + 1]) + 1e-10,
            "PCHIP shape-preservation check failed"
        );
        Ok(value)
    }

    fn integral(&self, lo: f64, hi: f64) -> f64 {
        (0..3)
            .map(|i| {
                let left = lo.max(self.x[i]);
                let right = hi.min(self.x[i + 1]);
                if right <= left {
                    return 0.0;
                }
                let h = self.x[i + 1] - self.x[i];
                let [a, b, c, d] = self.coefficients(i);
                let primitive = |t: f64| t * (a + t * (b / 2.0 + t * (c / 3.0 + t * d / 4.0)));
                h * (primitive((right - self.x[i]) / h) - primitive((left - self.x[i]) / h))
            })
            .sum()
    }

    fn monotone(&self) -> bool {
        self.y.windows(2).all(|p| p[1] >= p[0])
    }
}

fn endpoint(h0: f64, h1: f64, d0: f64, d1: f64) -> f64 {
    let slope = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
    if slope.signum() != d0.signum() {
        0.0
    } else if d0.signum() != d1.signum() && slope.abs() > 3.0 * d0.abs() {
        3.0 * d0
    } else {
        slope
    }
}

pub(super) fn compare(
    rates_a: &[f64],
    quality_a: &[f64],
    rates_b: &[f64],
    quality_b: &[f64],
) -> anyhow::Result<Comparison> {
    let a = Curve::new(rates_a, quality_a)?;
    let b = Curve::new(rates_b, quality_b)?;
    let lo = a.x[0].max(b.x[0]);
    let hi = a.x[3].min(b.x[3]);
    let resolution = 1e-9 * lo.abs().max(hi.abs()).max(1.0);
    ensure!(
        hi - lo > resolution,
        "insufficient common quality overlap (must exceed numerical resolution {resolution})"
    );
    let percent = ((b.integral(lo, hi) - a.integral(lo, hi)) / (hi - lo)).exp_m1() * 100.0;
    ensure!(
        percent.is_finite() && percent > -100.0,
        "non-finite or unrepresentable BD-rate"
    );
    let mut samples: Vec<_> = (0..=128)
        .map(|i| lo + (hi - lo) * (i as f64 / 128.0))
        .collect();
    samples.extend(a.x.into_iter().chain(b.x).filter(|v| *v >= lo && *v <= hi));
    samples.sort_by(f64::total_cmp);
    samples.dedup();
    let relative_curve = samples
        .into_iter()
        .map(|q| {
            let ar = a.at(q)?;
            let br = b.at(q)?;
            let row = [q, ar.exp(), br.exp(), (br - ar).exp_m1() * 100.0];
            ensure!(
                row.iter().all(|v| v.is_finite()),
                "non-finite relative curve sample"
            );
            Ok(row)
        })
        .collect::<anyhow::Result<Vec<_>>>()?;
    Ok(Comparison {
        percent,
        overlap: [lo, hi],
        overlap_fraction: (hi - lo) / (a.x[3].max(b.x[3]) - a.x[0].min(b.x[0])),
        baseline_monotone: a.monotone(),
        candidate_monotone: b.monotone(),
        interpolation_sanity: true,
        relative_curve,
    })
}

/// Candidate-vs-baseline bitrate delta at equal quality, in percent.
/// Uses actual measured rates and piecewise shape-preserving interpolation in
/// natural-log bitrate. Only the common quality interval is integrated.
/// Exactly four finite points are required. Duplicate quality, non-positive
/// rates and numerically insufficient overlap are rejected without merging or
/// extrapolation. Mild non-monotonic measurement noise is preserved locally.
pub fn bd_rate(
    rates_a: &[f64],
    quality_a: &[f64],
    rates_b: &[f64],
    quality_b: &[f64],
) -> anyhow::Result<f64> {
    Ok(compare(rates_a, quality_a, rates_b, quality_b)?.percent)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn saturated_hevc_cq_does_not_claim_a_pathological_rate_saving() {
        let rates_a = [66935.2176, 31579.712, 11990.6592, 4639.0234666666665];
        let quality_a = [99.948437, 99.658215, 93.662073, 74.339812];
        let rates_b = [58530.872, 26736.22666666667, 10107.1776, 3980.0128];
        let quality_b = [99.931507, 99.378973, 91.145589, 69.069792];
        let result = compare(&rates_a, &quality_a, &rates_b, &quality_b).unwrap();
        // SciPy 1.17.1 PchipInterpolator(log(rate), extrapolate=False), analytic integrate.
        assert!((result.percent - 1.5007488885949112).abs() < 1e-10);
        let curve = Curve::new(&rates_a, &quality_a).unwrap();
        assert!((curve.at(87.1356595).unwrap().exp() - 7347.309349599525).abs() < 1e-7);
        for i in 0..3 {
            for step in 0..=100 {
                let q = curve.x[i] + (curve.x[i + 1] - curve.x[i]) * (step as f64 / 100.0);
                let rate = curve.at(q).unwrap().exp();
                assert!(rate >= rates_a.iter().copied().fold(f64::INFINITY, f64::min) - 1e-7);
                assert!(rate <= 66935.2176 + 1e-7);
            }
        }
    }

    #[test]
    fn scipy_piecewise_values_and_partial_integral_match() {
        let rates = [1.0, 1f64.exp(), 1.5f64.exp(), 3f64.exp()];
        let curve = Curve::new(&rates, &[0.0, 1.0, 2.0, 3.0]).unwrap();
        for (q, expected) in [
            (0.5, 0.5729166666666667),
            (1.5, 1.2395833333333333),
            (2.5, 2.09375),
        ] {
            assert!((curve.at(q).unwrap() - expected).abs() < 1e-14);
        }
        assert!((curve.integral(0.25, 2.75) - 3.2103407118055554).abs() < 1e-14);
    }

    #[test]
    fn constant_rate_ratios_sorting_and_log_units_are_invariant() {
        let rates = [8000.0, 1000.0, 4000.0, 2000.0];
        let q = [99.9, 74.0, 99.0, 92.0];
        for ratio in [0.5, 1.0, 2.0] {
            let candidate = rates.map(|r| r * ratio);
            let value = bd_rate(&rates, &q, &candidate, &q).unwrap();
            assert!((value - (ratio - 1.0) * 100.0).abs() < 1e-10);
            let shifted = q.map(|v| v + 1000.0);
            let scaled = rates.map(|r| r * 1000.0);
            let scaled_candidate = candidate.map(|r| r * 1000.0);
            assert!(
                (bd_rate(&scaled, &shifted, &scaled_candidate, &shifted).unwrap() - value).abs()
                    < 1e-9
            );
        }
    }

    #[test]
    fn partial_overlap_is_integrated_without_extrapolation() {
        let a = [0.0, 1.0, 2.0, 3.0];
        let b: [f64; 4] = [1.0, 2.0, 3.0, 4.0];
        let rates_a = a.map(f64::exp);
        let rates_b = b.map(|v| (v - 0.2).exp());
        let result = compare(&rates_a, &a, &rates_b, &b).unwrap();
        assert_eq!(result.overlap, [1.0, 3.0]);
        assert!((result.percent - ((-0.2f64).exp_m1() * 100.0)).abs() < 1e-12);
        let curve = Curve::new(&rates_a, &a).unwrap();
        assert!(curve.at(-0.01).is_err());
        assert!(curve.at(3.01).is_err());
    }

    #[test]
    fn invalid_inputs_and_insufficient_overlap_are_rejected() {
        let r = [1.0, 2.0, 3.0, 4.0];
        let q = [0.0, 1.0, 2.0, 3.0];
        for bad in [0.0, -1.0, f64::NAN, f64::INFINITY] {
            let mut invalid = r;
            invalid[0] = bad;
            assert!(bd_rate(&invalid, &q, &r, &q).is_err());
        }
        for invalid in [
            [0.0, 1.0, 1.0, 3.0],
            [0.0, 1.0, f64::NAN, 3.0],
            [0.0, 1.0, 2.0, f64::INFINITY],
        ] {
            assert!(bd_rate(&r, &invalid, &r, &q).is_err());
        }
        for b in [
            [4.0, 5.0, 6.0, 7.0],
            [3.0, 4.0, 5.0, 6.0],
            [3.0 - 1e-12, 4.0, 5.0, 6.0],
        ] {
            assert!(bd_rate(&r, &q, &r, &b).is_err());
        }
        assert!(bd_rate(&r[..3], &q[..3], &r, &q).is_err());
    }

    #[test]
    fn mildly_noisy_curve_is_preserved_and_flagged() {
        let rates = [1000.0, 2000.0, 1995.0, 4000.0];
        let quality = [80.0, 90.0, 90.1, 99.0];
        let result = compare(&rates, &quality, &rates, &quality).unwrap();
        assert!(!result.baseline_monotone && !result.candidate_monotone);
        assert!(result.interpolation_sanity && result.percent.abs() < 1e-12);
    }

    #[test]
    fn closely_spaced_distinct_quality_is_not_a_global_fit_singularity() {
        let rates = [500.0, 501.0, 503.0, 506.0];
        let quality = [90.0, 90.0001, 90.0002, 90.0003];
        let result = compare(&rates, &quality, &rates, &quality).unwrap();
        assert!(result.percent.abs() < 1e-12 && result.interpolation_sanity);
    }
}
