//! Numerical checks available without a GPU or metric-tool dependency.

use super::bd_rate::bd_rate;

/// Every failed check's message, empty when all checks passed.
pub fn self_test_failures() -> Vec<String> {
    let mut failures = Vec::new();
    let rates = [1000.0, 2000.0, 4000.0, 8000.0];
    let quality = [80.0, 85.0, 90.0, 93.0];
    for ratio in [0.5, 1.0, 2.0] {
        let candidate = rates.map(|r| r * ratio);
        match bd_rate(&rates, &quality, &candidate, &quality) {
            Ok(result) if (result - (ratio - 1.0) * 100.0).abs() < 1e-9 => {}
            result => failures.push(format!("constant rate ratio {ratio}: {result:?}")),
        }
    }
    let rates_a = [66935.2176, 31579.712, 11990.6592, 4639.0234666666665];
    let quality_a = [99.948437, 99.658215, 93.662073, 74.339812];
    let rates_b = [58530.872, 26736.22666666667, 10107.1776, 3980.0128];
    let quality_b = [99.931507, 99.378973, 91.145589, 69.069792];
    match bd_rate(&rates_a, &quality_a, &rates_b, &quality_b) {
        Ok(result) if (result - 1.5007488885949112).abs() < 1e-10 => {}
        result => failures.push(format!("saturated quality PCHIP reference: {result:?}")),
    }
    if bd_rate(&rates, &[80.0, 85.0, 85.0, 93.0], &rates, &quality).is_ok() {
        failures.push("duplicate quality must be rejected".into());
    }
    failures
}

/// Runs the checks and prints a concise numerical validation result.
pub fn self_test() -> bool {
    let failures = self_test_failures();
    if failures.is_empty() {
        println!("SELF-TEST PASSED");
        true
    } else {
        println!("SELF-TEST FAILED: {failures:?}");
        false
    }
}
#[cfg(test)]
mod tests {
    #[test]
    fn all_self_checks_pass() {
        let failures = super::self_test_failures();
        assert!(failures.is_empty(), "{failures:?}");
    }
}
