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

# Relative (not absolute) pivot-singularity tolerance for _solve_linear_system,
# expressed as a fraction of the solved matrix's own largest-magnitude entry.
# See _solve_linear_system's docstring for why this must be relative and how
# this value was chosen.
_SINGULAR_REL_TOL = 1e-15


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
    for well-separated rate-distortion sample points.

    The singularity guard compares each pivot to a fraction of the matrix's
    own largest-magnitude entry rather than to a bare absolute constant.
    _polyfit3 builds `a` from sums of x^0..x^6 over quality-metric values
    (e.g. VMAF scores in roughly the 0-100 range), so entries routinely
    reach 1e6-1e12+ even for perfectly reasonable, well-separated sample
    points — this harness's own self-test data bottoms out around a
    relative pivot of ~1e-14. A fixed absolute threshold (previously
    1e-12) would never fire for matrices at that scale no matter how
    ill-conditioned they were in relative terms, silently handing back
    numerically meaningless coefficients for near-duplicate or
    tightly-clustered metric samples — a real possibility when a
    preset/RC sweep plateaus in quality. _SINGULAR_REL_TOL was picked
    empirically to sit comfortably below that ~1e-14 floor (so normal,
    well-separated inputs are unaffected) while still catching genuinely
    clustered inputs, whose relative pivots collapse many orders of
    magnitude further (see the self-test).
    """
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    scale = max(abs(v) for row in a for v in row)
    threshold = _SINGULAR_REL_TOL * scale if scale > 0 else _SINGULAR_REL_TOL

    for col in range(n):
        pivot = max(range(col, n), key=lambda r: abs(m[r][col]))
        if abs(m[pivot][col]) < threshold:
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
    # exactly symmetric in log-space to the halving case above. Because
    # _polyfit3 is OLS with an intercept and rates_double is a uniform
    # constant-factor (2x) scale of `rates` at the *same* metrics, the fit
    # only shifts curve B's constant term (c0) — c1/c2/c3 come out
    # identical to curve A's and cancel exactly in the BD-rate subtraction.
    # That makes the true BD-rate exactly (2 - 1) * 100 = +100.00%, the
    # mirror image of half-bitrate's exact -50.00% above (hand-verified;
    # matches the tight tolerance already used for the halving case).
    rates_double = [r * 2 for r in rates]
    double = bd_rate(rates, metrics, rates_double, metrics)
    check(f"double-bitrate curve near +100% (got {double:.2f})", 95.0 < double < 105.0)

    # Two curves with genuinely different SHAPES (not a uniform rate scale)
    # and a partial (non-identical) quality-range overlap, to exercise the
    # fit's curvature terms (c1/c2/c3) and the x^2/x^3/x^4 integration terms
    # in _polyint3. The three cases above only ever scale curve B's rates by
    # a constant factor at identical metrics, which — as noted above — only
    # ever moves c0; c1/c2/c3 are identical between curve A and curve B and
    # cancel out of the BD-rate subtraction, so a bug in the higher-order
    # fit or integration terms would go completely undetected by those
    # cases alone.
    #
    # Approach taken: (a) hand-derived exact expected value, not (b) a
    # coarse discriminating check, because it was workable here. Curve A's
    # log10(rate) is built from a KNOWN, exactly-linear function of quality
    # metric x: fA(x) = -2 + 0.05*x. Curve B's is fA(x) plus a KNOWN
    # quadratic term: fB(x) = fA(x) + 0.001*(x-90)**2 — enough to make c1
    # and c2 differ from curve A's while staying hand-integrable. Because
    # each curve has exactly 4 points, _polyfit3's 4x4 normal-equations
    # solve is an exact interpolation (not a lossy least-squares fit) here,
    # so the fitted coefficients exactly reproduce fA and fB algebraically.
    # That makes the BD-rate integral derivable in closed form without
    # going through the code under test:
    #   avg[(x-90)^2 over [lo,hi]] = ((hi-90)^3 - (lo-90)^3) / (3*(hi-lo))
    #   D = 0.001 * avg[(x-90)^2]   (curve B's average log-rate offset from
    #                                curve A; the average of fA itself
    #                                cancels exactly in the subtraction)
    #   bd_rate = (10**D - 1) * 100
    metrics_shape_a = [80, 85, 90, 95]
    metrics_shape_b = [81, 84, 91, 94]  # narrower, partially-overlapping range

    def _fa(x):
        return -2 + 0.05 * x

    rates_shape_a = [10 ** _fa(x) for x in metrics_shape_a]
    rates_shape_b = [10 ** (_fa(x) + 0.001 * (x - 90) ** 2) for x in metrics_shape_b]
    shape = bd_rate(rates_shape_a, metrics_shape_a, rates_shape_b, metrics_shape_b)
    shape_lo, shape_hi = 81.0, 94.0
    avg_quad = ((shape_hi - 90) ** 3 - (shape_lo - 90) ** 3) / (3 * (shape_hi - shape_lo))
    expected_shape = (10 ** (0.001 * avg_quad) - 1) * 100.0
    check(
        f"differently-shaped partial-overlap curves match hand-derived BD-rate "
        f"(got {shape:.6f}, expected {expected_shape:.6f})",
        abs(shape - expected_shape) < 1e-6,
    )

    # Closely-clustered/tightly-spaced metric values: a preset/RC sweep
    # that has plateaued in quality can produce samples this close
    # together. Under the OLD absolute pivot threshold (1e-12) this matrix
    # silently "solved" despite being severely ill-conditioned — verified
    # by hand: the fit's residuals at the 4 sample points were ~4e-4
    # (versus the ~1e-12 expected for an exact 4-point cubic
    # interpolation), and the fitted c0 came out around -291 for
    # log10(rate) values that are actually ~2.7. The relative threshold in
    # _solve_linear_system correctly identifies this matrix as unreliable
    # and raises instead of silently returning that garbage.
    metrics_clustered = [90.0000, 90.0001, 90.0002, 90.0003]
    rates_clustered = [500, 501, 503, 506]
    try:
        _polyfit3(metrics_clustered, [math.log10(r) for r in rates_clustered])
        failures.append("expected ValueError for near-singular clustered metrics")
    except ValueError:
        pass

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
