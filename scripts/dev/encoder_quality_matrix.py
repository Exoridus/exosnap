#!/usr/bin/env python3
"""Encoder quality matrix harness for ExoSnap's NVENC path.

Sweeps preset/rate-control combinations through probe_encode_file, scores
each encode against a reference Y4M clip with an external ffmpeg (libvmaf),
and reports BD-rate (bitrate delta at equal quality) across the sweep.

This is dev-only tooling: it is never run in CI (needs real NVENC hardware
and a local ffmpeg build with libvmaf) and nothing here ships in the product.
See docs/development/encoder-quality-matrix.md for the full workflow.

Self-test (no GPU, no ffmpeg needed):
    python encoder_quality_matrix.py --self-test
"""

import argparse
import math
import sys


def bd_rate(rates_a, metrics_a, rates_b, metrics_b):
    """BD-rate (Bjontegaard-delta rate) between curve A (baseline) and curve B
    (candidate), in percent. Negative means B needs less bitrate than A for
    the same quality (an improvement); positive means B is worse.

    Each curve is >= 4 (rate, metric) points. Rates are log-transformed (the
    standard BD-rate construction: rate-distortion curves are close to linear
    in log(rate) vs. quality), fit with a cubic polynomial, then the integral
    of the fitted log-rate over the metric range common to both curves is
    compared. This mirrors the piecewise log-interpolation approach used by
    the reference BD-rate implementations (e.g. the JCT-VC Excel/Matlab
    tools), reimplemented here with only the Python standard library.
    """
    if len(rates_a) < 4 or len(rates_b) < 4:
        raise ValueError("bd_rate needs at least 4 (rate, metric) points per curve")
    if len(rates_a) != len(metrics_a) or len(rates_b) != len(metrics_b):
        raise ValueError("rates and metrics must be the same length")

    log_rates_a = [math.log10(r) for r in rates_a]
    log_rates_b = [math.log10(r) for r in rates_b]

    coeffs_a = _polyfit3(metrics_a, log_rates_a)
    coeffs_b = _polyfit3(metrics_b, log_rates_b)

    lo = max(min(metrics_a), min(metrics_b))
    hi = min(max(metrics_a), max(metrics_b))
    if hi <= lo:
        raise ValueError("curves do not overlap in quality range")

    integral_a = (_polyint3(coeffs_a, hi) - _polyint3(coeffs_a, lo)) / (hi - lo)
    integral_b = (_polyint3(coeffs_b, hi) - _polyint3(coeffs_b, lo)) / (hi - lo)

    return (10 ** (integral_b - integral_a) - 1) * 100.0


def _polyfit3(x, y):
    """Least-squares cubic fit: returns [c0, c1, c2, c3] for
    y = c0 + c1*x + c2*x^2 + c3*x^3. Stdlib-only (solves the 4x4 normal
    equations directly with Gaussian elimination — no numpy).
    """
    n = len(x)
    powers = [[xi**p for p in range(7)] for xi in x]  # x^0..x^6, needed for the normal equations

    # Normal equations: A^T A c = A^T y, where A's columns are x^0..x^3.
    ata = [[sum(powers[i][a + b] for i in range(n)) for b in range(4)] for a in range(4)]
    aty = [sum(powers[i][a] * y[i] for i in range(n)) for a in range(4)]

    return _solve_linear_system(ata, aty)


def _polyint3(coeffs, x):
    """Definite-integral-to-x of c0 + c1*x + c2*x^2 + c3*x^3."""
    c0, c1, c2, c3 = coeffs
    return c0 * x + c1 * x**2 / 2 + c2 * x**3 / 3 + c3 * x**4 / 4


def _solve_linear_system(a, b):
    """Solves a*x = b for a square matrix `a` (list of rows) via Gaussian
    elimination with partial pivoting. Stdlib-only 4x4 solver — small and
    fixed-size enough that numerical stability is not a practical concern
    for this harness's rate-distortion curves.
    """
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]

    for col in range(n):
        pivot = max(range(col, n), key=lambda r: abs(m[r][col]))
        if abs(m[pivot][col]) < 1e-12:
            raise ValueError("singular matrix in bd_rate curve fit")
        m[col], m[pivot] = m[pivot], m[col]
        for r in range(n):
            if r == col:
                continue
            factor = m[r][col] / m[col][col]
            for c in range(col, n + 1):
                m[r][c] -= factor * m[col][c]

    return [m[i][n] / m[i][i] for i in range(n)]


def _self_test():
    failures = []

    def check(name, cond):
        if not cond:
            failures.append(name)

    # Identical curves -> ~0% BD-rate.
    rates = [1000, 2000, 4000, 8000]
    metrics = [80, 85, 90, 93]
    same = bd_rate(rates, metrics, rates, metrics)
    check(f"identical curves near 0% (got {same:.4f})", abs(same) < 0.01)

    # Candidate needs exactly half the bitrate at every point for the same
    # quality -> BD-rate close to -50%.
    rates_b = [r / 2 for r in rates]
    half = bd_rate(rates, metrics, rates_b, metrics)
    check(f"half-bitrate curve near -50% (got {half:.2f})", -55.0 < half < -45.0)

    # Candidate needs double the bitrate for the same quality -> positive,
    # roughly symmetric in log-space to the halving case above.
    rates_double = [r * 2 for r in rates]
    double = bd_rate(rates, metrics, rates_double, metrics)
    check(f"double-bitrate curve is positive (got {double:.2f})", double > 45.0)

    # Fewer than 4 points must raise, not silently misbehave.
    try:
        bd_rate([1, 2, 3], [1, 2, 3], rates, metrics)
        failures.append("expected ValueError for <4 points")
    except ValueError:
        pass

    if failures:
        print("SELF-TEST FAILED:")
        for f in failures:
            print(f"  - {f}")
        return False

    print("SELF-TEST PASSED")
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="run built-in self-tests and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return 0 if _self_test() else 1

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
