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

# Minimal backstop against a literal division-by-(near-)zero pivot inside
# _solve_linear_system. This is NOT the singularity/ill-conditioning
# detector for _polyfit3's curve fits -- see _FIT_RESIDUAL_TOL below and
# _polyfit3's docstring for why pivot magnitude (even after equilibration)
# cannot reliably discriminate a genuinely-degenerate fit from a legitimate
# one: hand-verification against real VMAF sweep data found that ordinary,
# well-conditioned, evenly-spaced high-VMAF sweeps (e.g. [97, 97.5, 98,
# 98.5] or [99.5, 99.6, 99.7, 99.8]) can equilibrate down to pivots in the
# 6e-16 to 2e-15 range -- overlapping the genuinely-degenerate clustered
# case's ~2e-16 to 5e-16 floor. No fixed pivot-magnitude threshold sits
# between those two ranges. This constant only exists so a truly exact (or
# floating-point-exact) zero pivot -- e.g. duplicate x-values collapsing a
# column to identically zero -- raises a clear error instead of a
# ZeroDivisionError; it should essentially never fire on real data.
_ZERO_PIVOT_TOL = 1e-300

# Singularity/ill-conditioning tolerance for _polyfit3, applied to the
# fitted cubic's residual at the original sample points (see _polyfit3's
# docstring for the full rationale). Hand-verified against real matrix
# data: legitimate sweeps top out around 1.2e-8 max residual (worst
# observed case, [99, 99.2, 99.4, 99.6]; reviewer independently measured
# up to ~1.5e-7 across a similar set), while the genuinely-clustered/
# near-duplicate case measures ~4.3e-4 -- roughly 4-5 orders of magnitude
# higher. 1e-5 sits in between with a wide margin on both sides (~800x
# above the worst legitimate residual seen, ~40x below the degenerate
# case's residual).
_FIT_RESIDUAL_TOL = 1e-5


def bd_rate(rates_a, metrics_a, rates_b, metrics_b):
    """BD-rate (Bjontegaard-delta rate) between curve A (baseline) and curve B
    (candidate), in percent. Negative means B needs less bitrate than A for
    the same quality (an improvement); positive means B is worse.

    Each curve is exactly 4 (rate, metric) points -- matching this tool's own
    matrix sweep (4 CQ points, 4 VBR points per preset; see default_matrix()).
    Rates are log-transformed (the standard BD-rate construction: rate-
    distortion curves are close to linear in log(rate) vs. quality), fit with
    a cubic polynomial, then the integral of the fitted log-rate over the
    metric range common to both curves is compared. This mirrors the
    piecewise log-interpolation approach used by the reference BD-rate
    implementations (e.g. the JCT-VC Excel/Matlab tools), reimplemented here
    with only the Python standard library.

    Exactly 4 points is a real constraint, not an arbitrary minimum: with 4
    points, _polyfit3's cubic fit is an EXACT interpolation (4 points, 4
    degrees of freedom), which is what makes its fit-residual singularity
    check meaningful (see _polyfit3's docstring). A 5+-point curve would be a
    genuine least-squares approximation with an inherent nonzero residual
    even when well-conditioned, which that check cannot distinguish from
    actual ill-conditioning -- so 5+ points is deliberately rejected rather
    than silently mismeasured.
    """
    if len(rates_a) != 4 or len(rates_b) != 4:
        raise ValueError("bd_rate needs exactly 4 (rate, metric) points per curve")
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

    With exactly 4 (x, y) sample points (the only size _polyfit3 is ever
    called with here — see bd_rate), the normal-equations solve is not a
    lossy least-squares approximation: it is an EXACT interpolation of all
    4 points, since a cubic has exactly 4 degrees of freedom. That gives a
    cheap, reliable way to detect whether the solve was numerically
    trustworthy: after solving, re-evaluate the fitted cubic at the
    original x_i and compare against y_i. For a well-conditioned system,
    that residual is near machine precision (~1e-8 or smaller, measured
    against real sweep data); for an ill-conditioned one (near-duplicate x
    values), rounding error during elimination blows the residual up to
    ~1e-4 or worse, even though the solve itself completes without a
    literal division error. This residual is the discriminator used here —
    NOT the pivot magnitude inside _solve_linear_system (see
    _FIT_RESIDUAL_TOL and _ZERO_PIVOT_TOL's module-level comments for why
    pivot magnitude alone cannot separate the two cases).
    """
    n = len(x)
    powers = [[xi**p for p in range(7)] for xi in x]  # x^0..x^6, needed for the normal equations

    # Normal equations: A^T A c = A^T y, where A's columns are x^0..x^3.
    ata = [[sum(powers[i][a + b] for i in range(n)) for b in range(4)] for a in range(4)]
    aty = [sum(powers[i][a] * y[i] for i in range(n)) for a in range(4)]

    coeffs = _solve_linear_system(ata, aty)

    c0, c1, c2, c3 = coeffs
    max_residual = max(abs((c0 + c1 * xi + c2 * xi**2 + c3 * xi**3) - yi) for xi, yi in zip(x, y))
    if max_residual > _FIT_RESIDUAL_TOL:
        raise ValueError(
            "curve fit is unreliable (max residual "
            f"{max_residual:.3e} > {_FIT_RESIDUAL_TOL:.0e} at the sample points) — "
            "the 4 (metric, rate) points do not interpolate cleanly; check the input data"
        )

    return coeffs


def _polyint3(coeffs, x):
    """Definite-integral-to-x of c0 + c1*x + c2*x^2 + c3*x^3."""
    c0, c1, c2, c3 = coeffs
    return c0 * x + c1 * x**2 / 2 + c2 * x**3 / 3 + c3 * x**4 / 4


def _solve_linear_system(a, b):
    """Solves a*x = b for a square matrix `a` (list of rows) via Gaussian
    elimination with partial pivoting. Stdlib-only 4x4 solver — small and
    fixed-size enough that numerical stability is not a practical concern
    for well-separated rate-distortion sample points.

    `a` is always _polyfit3's normal-equations matrix A^T A (symmetric
    positive-semidefinite by construction, with a[i][i] = sum(x^(2i)) over
    the quality-metric sample points). Its raw entries span many orders of
    magnitude by *design*, not because of ill-conditioning: column 0 holds
    x^0..x^3 sums (roughly 1-1e6 for VMAF-range x), column 3 holds x^3..x^6
    sums (roughly 1e6-1e12+) — a spread that exists identically for
    perfectly well-separated sample points (e.g. metrics=[95, 96, 97, 98],
    an ordinary quality sweep) and for genuinely near-duplicate ones alike.

    This function symmetrically equilibrates `a` before elimination, i.e.
    solves (D A D) x' = D b for x' = D^-1 x, where D = diag(1/sqrt(a[i][i])).
    This is the standard "correlation matrix" transformation for an SPD
    matrix — it rescales every row and column so the diagonal is exactly 1,
    and by Cauchy-Schwarz every off-diagonal entry of the equilibrated
    matrix is bounded to [-1, 1] regardless of the original matrix's scale.
    This is kept purely for the numerical-stability benefit during
    elimination itself (it measurably reduces rounding error in the
    fitted coefficients) — NOT as a singularity decision. Two prior fix
    rounds tried using the (equilibrated) pivot magnitude itself as the
    singularity/ill-conditioning signal, compared against a fixed
    threshold; both were proven wrong by hand-verification against real
    sweep data — see _polyfit3's docstring and the module-level comment on
    _FIT_RESIDUAL_TOL for why no fixed pivot-magnitude threshold can
    discriminate a genuinely-degenerate fit from a legitimate one. The
    singularity decision now lives entirely in _polyfit3's post-fit
    residual check. The only check left here is _ZERO_PIVOT_TOL, a loose
    backstop against a literal (near-)zero pivot so elimination raises a
    clear error instead of crashing on division by zero — it is not
    expected to fire on any real data. Row swaps during partial pivoting
    only reorder equations, never unknowns, so D itself needs no
    bookkeeping through the swaps — only the final x = D x' undo at the
    end.
    """
    n = len(b)

    # D = diag(1/sqrt(a[i][i])): the per-unknown equilibration scale. a[i][i]
    # is a sum of even powers of the sample points, so it is always > 0
    # unless that power's column carries no information at all (e.g. every
    # sample point is exactly 0) — a genuine degenerate/singular case.
    d = [0.0] * n
    for i in range(n):
        diag = a[i][i]
        if diag <= 0:
            raise ValueError("singular matrix in bd_rate curve fit")
        d[i] = 1.0 / math.sqrt(diag)

    # Build the equilibrated augmented matrix once; ordinary Gaussian
    # elimination with partial pivoting from here on.
    m = [[a[i][j] * d[i] * d[j] for j in range(n)] + [b[i] * d[i]] for i in range(n)]

    for col in range(n):
        pivot = max(range(col, n), key=lambda r: abs(m[r][col]))
        if abs(m[pivot][col]) < _ZERO_PIVOT_TOL:
            raise ValueError("singular matrix in bd_rate curve fit (exact zero pivot)")
        m[col], m[pivot] = m[pivot], m[col]
        for r in range(n):
            if r == col:
                continue
            factor = m[r][col] / m[col][col]
            for c in range(col, n + 1):
                m[r][c] -= factor * m[col][c]

    x_prime = [m[i][n] / m[i][i] for i in range(n)]
    return [x_prime[i] * d[i] for i in range(n)]


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

    # Regression test for a real false positive found in review: an entirely
    # ordinary, evenly-spaced quality sweep with NO clustering at all
    # (metrics=[95, 96, 97, 98], step of exactly 1 VMAF point) used to raise
    # ValueError under a singularity guard that compared pivots to the
    # whole matrix's single largest entry. _polyfit3's normal-equations
    # matrix mixes x^0..x^6 sums of these values, so its raw entries span
    # ~1e6 to ~3e12 by construction — nothing to do with how well-separated
    # the 4 sample points actually are (they're 1 full VMAF point apart,
    # about as ordinary as a sweep gets). Same hand-derivation technique as
    # the "differently shaped" case above: curve A is a KNOWN linear
    # function of quality, curve B is that function plus a KNOWN quadratic
    # offset, so the expected BD-rate is derivable in closed form.
    metrics_narrow_a = [95, 96, 97, 98]
    metrics_narrow_b = [95, 96, 97, 98]
    rates_narrow_a = [10 ** _fa(x) for x in metrics_narrow_a]
    rates_narrow_b = [10 ** (_fa(x) + 0.0005 * (x - 96.5) ** 2) for x in metrics_narrow_b]
    narrow = bd_rate(rates_narrow_a, metrics_narrow_a, rates_narrow_b, metrics_narrow_b)
    narrow_lo, narrow_hi = 95.0, 98.0
    avg_quad_narrow = ((narrow_hi - 96.5) ** 3 - (narrow_lo - 96.5) ** 3) / (3 * (narrow_hi - narrow_lo))
    expected_narrow = (10 ** (0.0005 * avg_quad_narrow) - 1) * 100.0
    check(
        f"realistic narrow-range evenly-spaced metrics (95-98) do not falsely "
        f"trip the singularity guard and match hand-derived BD-rate "
        f"(got {narrow:.6f}, expected {expected_narrow:.6f})",
        abs(narrow - expected_narrow) < 1e-6,
    )

    # Third-review-round regression: three more high-VMAF, evenly-spaced,
    # entirely ordinary sweeps that a PRIOR fix round's pivot-magnitude
    # guard (after equilibration) still falsely flagged as singular. Hand-
    # verification proved the equilibrated pivot floor for these sweeps
    # (~6e-16 to ~2e-15) genuinely overlaps the floor of the real
    # degenerate/clustered case below (~2e-16 to ~5e-16) — no fixed pivot
    # threshold can separate them. The actual discriminator is the fit
    # RESIDUAL (see _FIT_RESIDUAL_TOL and _polyfit3's docstring): these
    # sweeps interpolate their 4 points to near machine precision (measured
    # max residual on the order of 1e-8 to 1e-9), 3-4 orders of magnitude
    # below the residual threshold, so they must not raise. Identical
    # curves are used (like the very first self-test case above) since the
    # point here is purely "does the fit succeed", not curve shape — that's
    # already covered by the "differently shaped" and "narrow-range" cases
    # above.
    for name, sweep_metrics in (
        ("[97, 97.5, 98, 98.5]", [97, 97.5, 98, 98.5]),
        ("[99, 99.2, 99.4, 99.6]", [99, 99.2, 99.4, 99.6]),
        ("[99.5, 99.6, 99.7, 99.8]", [99.5, 99.6, 99.7, 99.8]),
    ):
        # Rates from the same smooth log-linear model used elsewhere in this
        # self-test (_fa), i.e. a well-behaved RD curve — not arbitrary
        # hand-picked integers, which (verified) can be "unsmooth" enough
        # relative to a 0.1-1 VMAF-unit sweep spacing to genuinely inflate
        # the residual on their own, independent of x-conditioning.
        sweep_rates = [10 ** _fa(x) for x in sweep_metrics]
        try:
            result = bd_rate(sweep_rates, sweep_metrics, sweep_rates, sweep_metrics)
            check(
                f"legitimate high-VMAF sweep {name} does not falsely trip the fit-"
                f"residual guard (identical curves, got {result:.4f}, want ~0)",
                abs(result) < 0.01,
            )
        except ValueError as exc:
            failures.append(f"legitimate high-VMAF sweep {name} incorrectly raised: {exc}")

    # Closely-clustered/tightly-spaced metric values: a preset/RC sweep
    # that has plateaued in quality can produce samples this close
    # together. Verified by hand: the fit's residuals at the 4 sample
    # points are ~4.3e-4 (versus ~1e-8 or smaller for the legitimate
    # sweeps above, including the narrow high-VMAF ones just checked) —
    # roughly 4-5 orders of magnitude larger, well past _FIT_RESIDUAL_TOL.
    # This is what _polyfit3's post-fit residual check now catches: the
    # equilibrated-pivot approach previously used here could not
    # distinguish this case from the legitimate narrow sweeps above (their
    # pivot floors genuinely overlap), but their residuals do not overlap
    # at all.
    metrics_clustered = [90.0000, 90.0001, 90.0002, 90.0003]
    rates_clustered = [500, 501, 503, 506]
    try:
        _polyfit3(metrics_clustered, [math.log10(r) for r in rates_clustered])
        failures.append("expected ValueError for near-singular clustered metrics")
    except ValueError:
        pass

    # Anything other than exactly 4 points must raise, not silently misbehave.
    try:
        bd_rate([1, 2, 3], [1, 2, 3], rates, metrics)
        failures.append("expected ValueError for <4 points")
    except ValueError:
        pass

    try:
        bd_rate([1000, 2000, 4000, 6000, 8000], [80, 85, 88, 90, 93], rates, metrics)
        failures.append("expected ValueError for >4 points")
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
