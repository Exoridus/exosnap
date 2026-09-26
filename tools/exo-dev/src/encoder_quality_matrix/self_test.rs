//! This tool's own numerical self-check: the same synthetic rate/metric
//! pairs and hand-derived expectations the ported script used to prove
//! `bd_rate`/`polyfit3`/`polyint3` before any GPU or ffmpeg dependency is
//! available. `exo-dev encoder-quality-matrix --self-test` runs this.

use super::bd_rate::{bd_rate, polyfit3};

/// Every failed check's message, empty when everything passed.
pub fn self_test_failures() -> Vec<String> {
    let mut failures = Vec::new();

    // Identical curves -> approximately 0% BD-rate.
    let rates = [1000.0, 2000.0, 4000.0, 8000.0];
    let metrics = [80.0, 85.0, 90.0, 93.0];
    let same = bd_rate(&rates, &metrics, &rates, &metrics).unwrap();
    if same.abs() >= 0.01 {
        failures.push(format!("identical curves near 0% (got {same:.4})"));
    }

    // Candidate needs exactly half the bitrate at every point for the same
    // quality -> BD-rate close to -50%.
    let rates_half: Vec<f64> = rates.iter().map(|r| r / 2.0).collect();
    let half = bd_rate(&rates, &metrics, &rates_half, &metrics).unwrap();
    if !(-55.0..-45.0).contains(&half) {
        failures.push(format!("half-bitrate curve near -50% (got {half:.2})"));
    }

    // Candidate needs double the bitrate for the same quality: positive,
    // exactly symmetric in log-space to the halving case above. Because
    // `polyfit3` is OLS with an intercept and `rates_double` is a uniform
    // constant-factor (2x) scale of `rates` at the same metrics, the fit
    // only shifts curve B's constant term (c0): c1/c2/c3 come out identical
    // to curve A's and cancel exactly in the BD-rate subtraction. That
    // makes the true BD-rate exactly (2 - 1) * 100 = +100.00%, the mirror
    // image of half-bitrate's exact -50.00% above.
    let rates_double: Vec<f64> = rates.iter().map(|r| r * 2.0).collect();
    let double = bd_rate(&rates, &metrics, &rates_double, &metrics).unwrap();
    if !(95.0..105.0).contains(&double) {
        failures.push(format!("double-bitrate curve near +100% (got {double:.2})"));
    }

    // Two curves with genuinely different shapes (not a uniform rate
    // scale) and a partial (non-identical) quality-range overlap, to
    // exercise the fit's curvature terms and the integration terms in
    // `polyint3`. The three cases above only ever scale curve B's rates by
    // a constant factor at identical metrics, which only ever moves c0, so
    // a bug in the higher-order fit or integration terms would go
    // completely undetected by those cases alone.
    //
    // Curve A's log10(rate) is a known, exactly linear function of the
    // quality metric x: fa(x) = -2 + 0.05*x. Curve B's is fa(x) plus a
    // known quadratic term: fb(x) = fa(x) + 0.001*(x-90)^2. Because each
    // curve has exactly 4 points, `polyfit3`'s solve is an exact
    // interpolation, so the fitted coefficients exactly reproduce fa and
    // fb algebraically, and the BD-rate integral is derivable in closed
    // form without going through the code under test:
    //   avg[(x-90)^2 over [lo,hi]] = ((hi-90)^3 - (lo-90)^3) / (3*(hi-lo))
    //   d = 0.001 * avg[(x-90)^2]
    //   bd_rate = (10^d - 1) * 100
    let metrics_shape_a = [80.0, 85.0, 90.0, 95.0];
    let metrics_shape_b = [81.0, 84.0, 91.0, 94.0]; // narrower, partially-overlapping range
    let fa = |x: f64| -2.0 + 0.05 * x;
    let rates_shape_a: Vec<f64> = metrics_shape_a.iter().map(|&x| 10f64.powf(fa(x))).collect();
    let rates_shape_b: Vec<f64> = metrics_shape_b
        .iter()
        .map(|&x| 10f64.powf(fa(x) + 0.001 * (x - 90.0).powi(2)))
        .collect();
    let shape = bd_rate(
        &rates_shape_a,
        &metrics_shape_a,
        &rates_shape_b,
        &metrics_shape_b,
    )
    .unwrap();
    let (shape_lo, shape_hi): (f64, f64) = (81.0, 94.0);
    let avg_quad =
        ((shape_hi - 90.0).powi(3) - (shape_lo - 90.0).powi(3)) / (3.0 * (shape_hi - shape_lo));
    let expected_shape = (10f64.powf(0.001 * avg_quad) - 1.0) * 100.0;
    if (shape - expected_shape).abs() >= 1e-6 {
        failures.push(format!(
            "differently-shaped partial-overlap curves match hand-derived BD-rate (got {shape:.6}, expected {expected_shape:.6})"
        ));
    }

    // Regression check: an entirely ordinary, evenly-spaced quality sweep
    // with no clustering at all (metrics 95..98, step of exactly 1 VMAF
    // point) used to raise under a singularity guard that compared pivots
    // to the matrix's single largest entry. Same hand-derivation as the
    // "differently shaped" case above.
    let metrics_narrow = [95.0, 96.0, 97.0, 98.0];
    let rates_narrow_a: Vec<f64> = metrics_narrow.iter().map(|&x| 10f64.powf(fa(x))).collect();
    let rates_narrow_b: Vec<f64> = metrics_narrow
        .iter()
        .map(|&x| 10f64.powf(fa(x) + 0.0005 * (x - 96.5).powi(2)))
        .collect();
    let narrow = bd_rate(
        &rates_narrow_a,
        &metrics_narrow,
        &rates_narrow_b,
        &metrics_narrow,
    )
    .unwrap();
    let (narrow_lo, narrow_hi): (f64, f64) = (95.0, 98.0);
    let avg_quad_narrow =
        ((narrow_hi - 96.5).powi(3) - (narrow_lo - 96.5).powi(3)) / (3.0 * (narrow_hi - narrow_lo));
    let expected_narrow = (10f64.powf(0.0005 * avg_quad_narrow) - 1.0) * 100.0;
    if (narrow - expected_narrow).abs() >= 1e-6 {
        failures.push(format!(
            "realistic narrow-range evenly-spaced metrics (95-98) match hand-derived BD-rate (got {narrow:.6}, expected {expected_narrow:.6})"
        ));
    }

    // Three more high-VMAF, evenly-spaced, entirely ordinary sweeps that an
    // earlier pivot-magnitude guard (even after equilibration) falsely
    // flagged as singular: their equilibrated pivot floor genuinely
    // overlaps the real degenerate/clustered case's floor, so the actual
    // discriminator is the fit residual, not the pivot. Identical curves
    // are used since the point here is purely "does the fit succeed", not
    // curve shape.
    for (name, sweep_metrics) in [
        ("[97, 97.5, 98, 98.5]", [97.0, 97.5, 98.0, 98.5]),
        ("[99, 99.2, 99.4, 99.6]", [99.0, 99.2, 99.4, 99.6]),
        ("[99.5, 99.6, 99.7, 99.8]", [99.5, 99.6, 99.7, 99.8]),
    ] {
        let sweep_rates: Vec<f64> = sweep_metrics.iter().map(|&x| 10f64.powf(fa(x))).collect();
        match bd_rate(&sweep_rates, &sweep_metrics, &sweep_rates, &sweep_metrics) {
            Ok(result) if result.abs() < 0.01 => {}
            Ok(result) => failures.push(format!(
                "legitimate high-VMAF sweep {name} does not falsely trip the fit-residual guard (identical curves, got {result:.4}, want ~0)"
            )),
            Err(error) => failures.push(format!("legitimate high-VMAF sweep {name} incorrectly raised: {error}")),
        }
    }

    // Closely-clustered/tightly-spaced metric values: a preset/RC sweep
    // that has plateaued in quality can produce samples this close
    // together. The fit residual here is about 4.3e-4, four to five orders
    // of magnitude above the legitimate sweeps checked so far, so
    // `polyfit3` must reject it.
    let metrics_clustered = [90.0000, 90.0001, 90.0002, 90.0003];
    let rates_clustered: [f64; 4] = [500.0, 501.0, 503.0, 506.0];
    let log_rates_clustered: Vec<f64> = rates_clustered.iter().map(|r| r.log10()).collect();
    if polyfit3(&metrics_clustered, &log_rates_clustered).is_ok() {
        failures.push("expected an error for near-singular clustered metrics".to_string());
    }

    // Anything other than exactly 4 points must raise, not silently
    // misbehave.
    if bd_rate(&[1.0, 2.0, 3.0], &[1.0, 2.0, 3.0], &rates, &metrics).is_ok() {
        failures.push("expected an error for fewer than 4 points".to_string());
    }
    if bd_rate(
        &[1000.0, 2000.0, 4000.0, 6000.0, 8000.0],
        &[80.0, 85.0, 88.0, 90.0, 93.0],
        &rates,
        &metrics,
    )
    .is_ok()
    {
        failures.push("expected an error for more than 4 points".to_string());
    }

    failures
}

/// Runs the self-test and prints the same "SELF-TEST PASSED" /
/// "SELF-TEST FAILED:" summary as the ported script. Returns whether it
/// passed.
pub fn self_test() -> bool {
    let failures = self_test_failures();
    if failures.is_empty() {
        println!("SELF-TEST PASSED");
        true
    } else {
        println!("SELF-TEST FAILED:");
        for failure in &failures {
            println!("  - {failure}");
        }
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn self_test_has_no_failures() {
        assert_eq!(self_test_failures(), Vec::<String>::new());
    }
}
