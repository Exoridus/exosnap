#!/usr/bin/env python3
"""Analyze ExoSnap encode-performance records from an engine.jsonl log.

The recording engine writes structured JSONL perf records (component == "perf"):

  * "video-pipeline-window"  -- one line every ~10 s while recording, carrying
    rolling-window encode-latency and frame-time percentiles.
  * "session-perf-summary"   -- one line per recording at collector shutdown,
    carrying the whole-session latency/frame-time histograms (the authoritative
    distribution; the live windows are noisy over ~2 s).

This script groups records into sessions (each summary closes a session), prints
a per-session table (p50/p95/p99/max for encode latency and frame time, drops,
budget overruns) and, given two logs, a before/after delta. Whole-session
percentiles are recomputed from the summary histograms so they are independent of
the noisy live windows.

Standard library only; no third-party dependencies.
"""

import argparse
import json
import sys

# Histogram schema mirror of libs/recorder_core/src/perf_histogram.h. The summary
# record carries hist_lo_ms / hist_hi_ms / hist_buckets so these are only defaults.
DEFAULT_LO_MS = 0.05
DEFAULT_HI_MS = 500.0
DEFAULT_BUCKETS = 64


def _geo_buckets(n_buckets):
    # kGeoBuckets == kBucketCount - 1 (last bucket is the overflow).
    return n_buckets - 1


def bucket_low_edge(b, lo, hi, n_buckets):
    geo = _geo_buckets(n_buckets)
    if b <= 0:
        return 0.0
    if b >= geo:
        return hi
    ratio = (hi / lo) ** (1.0 / geo)
    return lo * (ratio ** b)


def bucket_high_edge(b, lo, hi, n_buckets):
    geo = _geo_buckets(n_buckets)
    if b >= geo:
        return hi
    ratio = (hi / lo) ** (1.0 / geo)
    return lo * (ratio ** (b + 1))


def histogram_quantile(counts, q, lo, hi, n_buckets):
    """Linear-interpolated quantile over fixed-bucket counts.

    Mirrors LatencyHistogram::Quantile so a summary round-trips to the same value
    (within the bucket-interpolation error) the engine would have reported.
    """
    total = sum(counts)
    if total == 0:
        return 0.0
    q = min(1.0, max(0.0, q))
    target = q * total
    geo = _geo_buckets(n_buckets)
    cumulative = 0
    for b, c in enumerate(counts):
        if c == 0:
            continue
        if cumulative + c >= target:
            if b >= geo:
                return hi  # overflow bucket: report its floor
            low = bucket_low_edge(b, lo, hi, n_buckets)
            high = bucket_high_edge(b, lo, hi, n_buckets)
            into = (target - cumulative) / c
            into = min(1.0, max(0.0, into))
            return low + into * (high - low)
        cumulative += c
    return hi


def _to_float(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def _to_int(v, default=0):
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


def parse_perf_records(path):
    """Yield the fields dict of every component=="perf" record in a JSONL file.

    Tolerant of the two on-disk shapes: a nested {"fields": {...}} object and a
    flat record where keys live at the top level. Non-JSON / non-perf lines are
    skipped silently so a mixed engine.jsonl parses cleanly.
    """
    records = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if obj.get("component") != "perf":
                continue
            fields = obj.get("fields")
            if not isinstance(fields, dict):
                # Flat shape: everything except the envelope keys is a field.
                fields = {k: v for k, v in obj.items()
                          if k not in ("component", "level", "message", "timestamp", "msg")}
            msg = obj.get("message") or obj.get("msg") or fields.get("message", "")
            records.append((msg, fields))
    return records


def group_sessions(records):
    """Group (message, fields) records into sessions.

    A "session-perf-summary" closes the current session; preceding
    "video-pipeline-window" records belong to it. Windows with no trailing
    summary form a final open session.
    """
    sessions = []
    current = {"windows": [], "summary": None}
    for msg, fields in records:
        if msg == "session-perf-summary":
            current["summary"] = fields
            sessions.append(current)
            current = {"windows": [], "summary": None}
        elif msg == "video-pipeline-window":
            current["windows"].append(fields)
    if current["windows"] or current["summary"]:
        sessions.append(current)
    return sessions


def summarize_session(session):
    """Reduce one session to a flat dict of comparable metrics."""
    windows = session["windows"]
    summary = session["summary"]
    out = {
        "windows": len(windows),
        "encode_p50_ms": 0.0,
        "encode_p99_ms": 0.0,
        "tick_p50_ms": 0.0,
        "tick_p99_ms": 0.0,
        "encode_count": 0,
        "tick_count": 0,
        "dropped_backpressure": 0,
        "slot_stalls": 0,
        "tick_budget_ms": 0.0,
        "tick_budget_overruns": 0,
        "preset": "",
        "codec": "",
        "resolution": "",
        "has_summary": summary is not None,
    }

    # Budget overruns are only observable from the window series (the summary holds
    # a distribution, not the budget crossing per window).
    budget = 0.0
    overruns = 0
    for w in windows:
        budget = _to_float(w.get("tick_budget_ms"), budget)
        if budget > 0.0 and _to_float(w.get("tick_p99_ms")) > budget:
            overruns += 1
        out["preset"] = w.get("preset", out["preset"])
        out["codec"] = w.get("codec", out["codec"])
        out["resolution"] = w.get("resolution", out["resolution"])
    out["tick_budget_ms"] = budget
    out["tick_budget_overruns"] = overruns

    if summary is not None:
        lo = _to_float(summary.get("hist_lo_ms"), DEFAULT_LO_MS)
        hi = _to_float(summary.get("hist_hi_ms"), DEFAULT_HI_MS)
        n_buckets = _to_int(summary.get("hist_buckets"), DEFAULT_BUCKETS)
        enc = _parse_buckets(summary.get("encode_hist"))
        tick = _parse_buckets(summary.get("tick_hist"))
        if enc:
            out["encode_p50_ms"] = histogram_quantile(enc, 0.50, lo, hi, n_buckets)
            out["encode_p99_ms"] = histogram_quantile(enc, 0.99, lo, hi, n_buckets)
            out["encode_count"] = sum(enc)
        if tick:
            out["tick_p50_ms"] = histogram_quantile(tick, 0.50, lo, hi, n_buckets)
            out["tick_p99_ms"] = histogram_quantile(tick, 0.99, lo, hi, n_buckets)
            out["tick_count"] = sum(tick)
        out["dropped_backpressure"] = _to_int(summary.get("dropped_backpressure"))
        out["slot_stalls"] = _to_int(summary.get("slot_stalls"))
        out["preset"] = summary.get("preset", out["preset"])
        out["codec"] = summary.get("codec", out["codec"])
        out["resolution"] = summary.get("resolution", out["resolution"])
    elif windows:
        # No summary (open session): fall back to the last window's percentiles so
        # the session is still reportable, clearly labelled as window-derived.
        last = windows[-1]
        out["encode_p50_ms"] = _to_float(last.get("encode_p50_ms"))
        out["encode_p99_ms"] = _to_float(last.get("encode_p99_ms"))
        out["tick_p50_ms"] = _to_float(last.get("tick_p50_ms"))
        out["tick_p99_ms"] = _to_float(last.get("tick_p99_ms"))
        out["dropped_backpressure"] = _to_int(last.get("dropped_backpressure"))
        out["slot_stalls"] = _to_int(last.get("slot_stalls"))
    return out


def _parse_buckets(raw):
    if not raw:
        return []
    try:
        return [int(x) for x in str(raw).split(",") if x != ""]
    except ValueError:
        return []


def analyze_file(path):
    records = parse_perf_records(path)
    sessions = group_sessions(records)
    return [summarize_session(s) for s in sessions]


def _fmt_row(idx, s):
    src = "session-hist" if s["has_summary"] else "window-last"
    return (
        f"  #{idx:<2} {s['codec']:>5} {s['preset']:>3} {s['resolution']:>12}"
        f"  enc p50/p99={s['encode_p50_ms']:7.3f}/{s['encode_p99_ms']:7.3f} ms"
        f"  tick p50/p99={s['tick_p50_ms']:7.3f}/{s['tick_p99_ms']:7.3f} ms"
        f"  drops(bp)={s['dropped_backpressure']:>6} stalls={s['slot_stalls']:>6}"
        f"  overruns={s['tick_budget_overruns']:>3}  [{src}]"
    )


def print_report(path, sessions):
    print(f"Perf report: {path}")
    if not sessions:
        print("  (no perf records found)")
        return
    for i, s in enumerate(sessions):
        print(_fmt_row(i, s))
    print("  NOTE: session-hist rows use the whole-session histogram (authoritative);")
    print("        window-last rows are an open session's last 2 s window (noisy).")


def print_delta(before, after):
    print("Before/after delta (matched by session index):")
    n = min(len(before), len(after))
    if n == 0:
        print("  (nothing to compare)")
        return
    for i in range(n):
        b = before[i]
        a = after[i]

        def d(key):
            return a[key] - b[key]

        print(
            f"  #{i:<2} {a['codec']:>5} {a['preset']:>3}"
            f"  Δenc_p99={d('encode_p99_ms'):+8.3f} ms"
            f"  Δtick_p99={d('tick_p99_ms'):+8.3f} ms"
            f"  Δdrops(bp)={d('dropped_backpressure'):+6}"
            f"  Δstalls={d('slot_stalls'):+6}"
        )


def main(argv=None):
    parser = argparse.ArgumentParser(description="Analyze ExoSnap encode-performance JSONL records.")
    parser.add_argument("engine_jsonl", help="Path to engine.jsonl (or any file with perf records).")
    parser.add_argument("compare_jsonl", nargs="?", help="Optional second log for a before/after delta.")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON instead of a table.")
    args = parser.parse_args(argv)

    primary = analyze_file(args.engine_jsonl)
    secondary = analyze_file(args.compare_jsonl) if args.compare_jsonl else None

    if args.json:
        payload = {"file": args.engine_jsonl, "sessions": primary}
        if secondary is not None:
            payload["compare_file"] = args.compare_jsonl
            payload["compare_sessions"] = secondary
        json.dump(payload, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0

    print_report(args.engine_jsonl, primary)
    if secondary is not None:
        print()
        print_report(args.compare_jsonl, secondary)
        print()
        print_delta(primary, secondary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
