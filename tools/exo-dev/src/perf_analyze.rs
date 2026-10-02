//! Recomputes encode-latency and frame-time percentiles from an engine.jsonl
//! perf log, and diffs two logs session by session.
//!
//! The recording engine writes structured JSONL perf records (`component`
//! `"perf"`): a `"video-pipeline-window"` line every rolling window while
//! recording, carrying noisy live percentiles, and one closing
//! `"session-perf-summary"` line per recording, carrying the whole-session
//! encode-latency and frame-time histograms. This module groups records into
//! sessions, recomputes whole-session percentiles from the summary
//! histograms (independent of the noisy live windows) and reports them as a
//! table or as JSON, with an optional before/after delta between two logs.
//!
//! The bucket-edge and quantile math mirrors `libs/engine/src/perf_histogram.h`
//! bit for bit: same geometric bucket layout, same linear interpolation inside
//! the containing bucket, same overflow-bucket handling. A summary's recomputed
//! percentile therefore agrees with what the engine's own histogram would have
//! reported for the same samples.

use std::fs;
use std::path::Path;

use anyhow::Context as _;
use serde::Serialize;
use serde_json::{Map, Value};

/// Histogram defaults, used only when a summary record omits its own
/// `hist_lo_ms` / `hist_hi_ms` / `hist_buckets` fields.
pub const DEFAULT_LO_MS: f64 = 0.05;
pub const DEFAULT_HI_MS: f64 = 500.0;
pub const DEFAULT_BUCKETS: usize = 64;

/// One session's metrics, in the exact key set and order the JSON report uses.
/// Field order is alphabetical by name (matching the sorted-keys JSON output);
/// it has no bearing on how the fields are computed.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct SessionSummary {
    pub codec: String,
    pub dropped_backpressure: i64,
    pub encode_count: u64,
    pub encode_p50_ms: f64,
    pub encode_p99_ms: f64,
    pub has_summary: bool,
    pub preset: String,
    pub resolution: String,
    pub slot_stalls: i64,
    pub tick_budget_ms: f64,
    pub tick_budget_overruns: u32,
    pub tick_count: u64,
    pub tick_p50_ms: f64,
    pub tick_p99_ms: f64,
    pub windows: u64,
}

impl Default for SessionSummary {
    fn default() -> Self {
        SessionSummary {
            codec: String::new(),
            dropped_backpressure: 0,
            encode_count: 0,
            encode_p50_ms: 0.0,
            encode_p99_ms: 0.0,
            has_summary: false,
            preset: String::new(),
            resolution: String::new(),
            slot_stalls: 0,
            tick_budget_ms: 0.0,
            tick_budget_overruns: 0,
            tick_count: 0,
            tick_p50_ms: 0.0,
            tick_p99_ms: 0.0,
            windows: 0,
        }
    }
}

/// The number of geometric buckets: the last bucket (index `n_buckets - 1`) is
/// the overflow bucket, not a geometric one. Saturates at 0 for a malformed
/// (non-positive) bucket count instead of underflowing.
fn geo_buckets(n_buckets: usize) -> usize {
    n_buckets.saturating_sub(1)
}

fn ratio(lo: f64, hi: f64, geo: usize) -> f64 {
    (hi / lo).powf(1.0 / geo as f64)
}

/// Lower edge (inclusive) of bucket `b`, in ms. Bucket 0 absorbs sub-`lo`
/// samples so its floor is reported as 0. The overflow bucket floors at `hi`.
pub fn bucket_low_edge(b: usize, lo: f64, hi: f64, n_buckets: usize) -> f64 {
    let geo = geo_buckets(n_buckets);
    if b == 0 {
        return 0.0;
    }
    if b >= geo {
        return hi;
    }
    lo * ratio(lo, hi, geo).powf(b as f64)
}

/// Upper edge (exclusive) of bucket `b`, in ms. The overflow bucket has no
/// finite upper edge; `hi` is returned as a conservative floor.
pub fn bucket_high_edge(b: usize, lo: f64, hi: f64, n_buckets: usize) -> f64 {
    let geo = geo_buckets(n_buckets);
    if b >= geo {
        return hi;
    }
    lo * ratio(lo, hi, geo).powf((b + 1) as f64)
}

/// Linear-interpolated quantile, `q` in `[0, 1]`, over fixed-bucket counts.
/// An empty histogram (all-zero counts) returns 0.0.
pub fn histogram_quantile(counts: &[u64], q: f64, lo: f64, hi: f64, n_buckets: usize) -> f64 {
    let total: u64 = counts.iter().sum();
    if total == 0 {
        return 0.0;
    }
    let q = q.clamp(0.0, 1.0);
    let target = q * total as f64;
    let geo = geo_buckets(n_buckets);
    let mut cumulative: u64 = 0;
    for (b, &c) in counts.iter().enumerate() {
        if c == 0 {
            continue;
        }
        if (cumulative + c) as f64 >= target {
            if b >= geo {
                return hi; // overflow bucket: report its floor
            }
            let low = bucket_low_edge(b, lo, hi, n_buckets);
            let high = bucket_high_edge(b, lo, hi, n_buckets);
            let into = ((target - cumulative as f64) / c as f64).clamp(0.0, 1.0);
            return low + into * (high - low);
        }
        cumulative += c;
    }
    hi
}

/// A record's `fields` object read from `map`, tolerant of a JSON string that
/// wraps a number: the engine's logging layer serializes every field value as
/// a string, so a summary's `hist_buckets`, bucket counts and per-window
/// percentiles all arrive that way.
fn field_f64(map: &Map<String, Value>, key: &str, default: f64) -> f64 {
    match map.get(key) {
        Some(Value::Number(n)) => n.as_f64().unwrap_or(default),
        Some(Value::String(s)) => s.trim().parse::<f64>().unwrap_or(default),
        _ => default,
    }
}

fn field_i64(map: &Map<String, Value>, key: &str, default: i64) -> i64 {
    match map.get(key) {
        Some(Value::Number(n)) => n
            .as_i64()
            .or_else(|| n.as_f64().map(|f| f as i64))
            .unwrap_or(default),
        Some(Value::String(s)) => s.trim().parse::<i64>().unwrap_or(default),
        _ => default,
    }
}

/// A string field, present only when `map[key]` is itself a JSON string. Used
/// for the fields (`preset`, `codec`, `resolution`) that a caller only
/// overwrites when the record actually carries them, keeping whatever value a
/// previous record in the same session set otherwise.
fn field_str(map: &Map<String, Value>, key: &str) -> Option<String> {
    match map.get(key) {
        Some(Value::String(s)) => Some(s.clone()),
        _ => None,
    }
}

/// Parses a comma-separated bucket-count field. Absent, empty or malformed
/// (any element not a non-negative integer) all report an empty histogram,
/// same as an absent one, rather than a partially-parsed one.
fn parse_buckets(map: &Map<String, Value>, key: &str) -> Vec<u64> {
    let raw = match map.get(key) {
        Some(Value::String(s)) if !s.is_empty() => s,
        _ => return Vec::new(),
    };
    let mut out = Vec::new();
    for part in raw.split(',') {
        if part.is_empty() {
            continue;
        }
        match part.trim().parse::<u64>() {
            Ok(n) => out.push(n),
            Err(_) => return Vec::new(),
        }
    }
    out
}

/// One `(message, fields)` perf record, as `parse_perf_records` yields it.
type Record = (String, Map<String, Value>);

/// Reads every `component == "perf"` record from a JSONL file. Tolerant of a
/// non-JSON or non-perf line (skipped silently, so a mixed engine.jsonl parses
/// cleanly) and of a flat record shape where the fields live at the top level
/// instead of under a nested `"fields"` object.
fn parse_perf_records(path: &Path) -> anyhow::Result<Vec<Record>> {
    let content =
        fs::read_to_string(path).with_context(|| format!("reading perf log {}", path.display()))?;
    let mut records = Vec::new();
    for line in content.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let Ok(Value::Object(obj)) = serde_json::from_str::<Value>(line) else {
            continue;
        };
        if obj.get("component").and_then(Value::as_str) != Some("perf") {
            continue;
        }
        let fields = match obj.get("fields") {
            Some(Value::Object(f)) => f.clone(),
            _ => {
                let mut flat = Map::new();
                for (k, v) in &obj {
                    if !matches!(
                        k.as_str(),
                        "component" | "level" | "message" | "timestamp" | "msg"
                    ) {
                        flat.insert(k.clone(), v.clone());
                    }
                }
                flat
            }
        };
        let message = field_str(&obj, "message")
            .filter(|s| !s.is_empty())
            .or_else(|| field_str(&obj, "msg").filter(|s| !s.is_empty()))
            .or_else(|| field_str(&fields, "message"))
            .unwrap_or_default();
        records.push((message, fields));
    }
    Ok(records)
}

/// One session: the "video-pipeline-window" records that preceded its closing
/// "session-perf-summary" (or, for a still-open session, that have no
/// following summary at all).
struct Session {
    windows: Vec<Map<String, Value>>,
    summary: Option<Map<String, Value>>,
}

/// Groups records into sessions. A `"session-perf-summary"` closes the
/// current session; preceding `"video-pipeline-window"` records belong to it.
/// Windows with no trailing summary form a final open session.
fn group_sessions(records: Vec<Record>) -> Vec<Session> {
    let mut sessions = Vec::new();
    let mut current = Session {
        windows: Vec::new(),
        summary: None,
    };
    for (message, fields) in records {
        match message.as_str() {
            "session-perf-summary" => {
                current.summary = Some(fields);
                sessions.push(current);
                current = Session {
                    windows: Vec::new(),
                    summary: None,
                };
            }
            "video-pipeline-window" => current.windows.push(fields),
            _ => {}
        }
    }
    if !current.windows.is_empty() || current.summary.is_some() {
        sessions.push(current);
    }
    sessions
}

/// Reduces one session to its flat metrics. Budget overruns are only
/// observable from the window series (the summary holds a distribution, not
/// the budget crossing per window). A session with no summary (an open
/// session) falls back to its last window's own percentiles, clearly labelled
/// as window-derived by `has_summary: false`.
fn summarize_session(session: &Session) -> SessionSummary {
    let mut out = SessionSummary {
        has_summary: session.summary.is_some(),
        windows: session.windows.len() as u64,
        ..SessionSummary::default()
    };

    let mut budget = 0.0_f64;
    let mut overruns: u32 = 0;
    for w in &session.windows {
        budget = field_f64(w, "tick_budget_ms", budget);
        if budget > 0.0 && field_f64(w, "tick_p99_ms", 0.0) > budget {
            overruns += 1;
        }
        if let Some(v) = field_str(w, "preset") {
            out.preset = v;
        }
        if let Some(v) = field_str(w, "codec") {
            out.codec = v;
        }
        if let Some(v) = field_str(w, "resolution") {
            out.resolution = v;
        }
    }
    out.tick_budget_ms = budget;
    out.tick_budget_overruns = overruns;

    if let Some(summary) = &session.summary {
        let lo = field_f64(summary, "hist_lo_ms", DEFAULT_LO_MS);
        let hi = field_f64(summary, "hist_hi_ms", DEFAULT_HI_MS);
        let n_buckets = field_i64(summary, "hist_buckets", DEFAULT_BUCKETS as i64).max(0) as usize;
        let enc = parse_buckets(summary, "encode_hist");
        let tick = parse_buckets(summary, "tick_hist");
        if !enc.is_empty() {
            out.encode_p50_ms = histogram_quantile(&enc, 0.50, lo, hi, n_buckets);
            out.encode_p99_ms = histogram_quantile(&enc, 0.99, lo, hi, n_buckets);
            out.encode_count = enc.iter().sum();
        }
        if !tick.is_empty() {
            out.tick_p50_ms = histogram_quantile(&tick, 0.50, lo, hi, n_buckets);
            out.tick_p99_ms = histogram_quantile(&tick, 0.99, lo, hi, n_buckets);
            out.tick_count = tick.iter().sum();
        }
        out.dropped_backpressure = field_i64(summary, "dropped_backpressure", 0);
        out.slot_stalls = field_i64(summary, "slot_stalls", 0);
        if let Some(v) = field_str(summary, "preset") {
            out.preset = v;
        }
        if let Some(v) = field_str(summary, "codec") {
            out.codec = v;
        }
        if let Some(v) = field_str(summary, "resolution") {
            out.resolution = v;
        }
    } else if let Some(last) = session.windows.last() {
        out.encode_p50_ms = field_f64(last, "encode_p50_ms", 0.0);
        out.encode_p99_ms = field_f64(last, "encode_p99_ms", 0.0);
        out.tick_p50_ms = field_f64(last, "tick_p50_ms", 0.0);
        out.tick_p99_ms = field_f64(last, "tick_p99_ms", 0.0);
        out.dropped_backpressure = field_i64(last, "dropped_backpressure", 0);
        out.slot_stalls = field_i64(last, "slot_stalls", 0);
    }
    out
}

/// Reads and summarizes every session in a perf log, in file order.
pub fn analyze_file(path: &Path) -> anyhow::Result<Vec<SessionSummary>> {
    let records = parse_perf_records(path)?;
    let sessions = group_sessions(records);
    Ok(sessions.iter().map(summarize_session).collect())
}

fn fmt_row(idx: usize, s: &SessionSummary) -> String {
    let src = if s.has_summary {
        "session-hist"
    } else {
        "window-last"
    };
    format!(
        "  #{idx:<2} {codec:>5} {preset:>3} {resolution:>12}  \
enc p50/p99={enc_p50:7.3}/{enc_p99:7.3} ms  \
tick p50/p99={tick_p50:7.3}/{tick_p99:7.3} ms  \
drops(bp)={drops:>6} stalls={stalls:>6}  overruns={overruns:>3}  [{src}]",
        idx = idx,
        codec = s.codec,
        preset = s.preset,
        resolution = s.resolution,
        enc_p50 = s.encode_p50_ms,
        enc_p99 = s.encode_p99_ms,
        tick_p50 = s.tick_p50_ms,
        tick_p99 = s.tick_p99_ms,
        drops = s.dropped_backpressure,
        stalls = s.slot_stalls,
        overruns = s.tick_budget_overruns,
        src = src,
    )
}

/// Renders the per-session table: one row per session (its histogram
/// percentiles, drop/stall counts and budget overruns), or a placeholder line
/// when the log held no perf records. The caller prints the "Perf report:
/// <path>" header itself, since this function only sees the sessions.
pub fn print_report(sessions: &[SessionSummary]) -> String {
    let mut out = String::new();
    if sessions.is_empty() {
        out.push_str("  (no perf records found)\n");
        return out;
    }
    for (i, s) in sessions.iter().enumerate() {
        out.push_str(&fmt_row(i, s));
        out.push('\n');
    }
    out.push_str("  NOTE: session-hist rows use the whole-session histogram (authoritative);\n");
    out.push_str("        window-last rows are an open session's last 2 s window (noisy).\n");
    out
}

/// Renders a before/after delta, matching sessions by index (the Nth session
/// of the before log against the Nth session of the after log). Unmatched
/// trailing sessions on either side are silently ignored, same as the table
/// being one row shorter than the other log's session count.
pub fn print_delta(before: &[SessionSummary], after: &[SessionSummary]) -> String {
    let mut out = String::from("Before/after delta (matched by session index):\n");
    let n = before.len().min(after.len());
    if n == 0 {
        out.push_str("  (nothing to compare)\n");
        return out;
    }
    for i in 0..n {
        let b = &before[i];
        let a = &after[i];
        let d_enc_p99 = a.encode_p99_ms - b.encode_p99_ms;
        let d_tick_p99 = a.tick_p99_ms - b.tick_p99_ms;
        let d_drops = a.dropped_backpressure - b.dropped_backpressure;
        let d_stalls = a.slot_stalls - b.slot_stalls;
        out.push_str(&format!(
            "  #{i:<2} {codec:>5} {preset:>3}  \u{394}enc_p99={d_enc_p99:+8.3} ms  \u{394}tick_p99={d_tick_p99:+8.3} ms  \u{394}drops(bp)={d_drops:+6}  \u{394}stalls={d_stalls:+6}\n",
            i = i,
            codec = a.codec,
            preset = a.preset,
            d_enc_p99 = d_enc_p99,
            d_tick_p99 = d_tick_p99,
            d_drops = d_drops,
            d_stalls = d_stalls,
        ));
    }
    out
}

#[derive(Serialize)]
struct JsonPayload<'a> {
    #[serde(skip_serializing_if = "Option::is_none")]
    compare_file: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    compare_sessions: Option<&'a [SessionSummary]>,
    file: String,
    sessions: &'a [SessionSummary],
}

/// Renders the pretty-printed JSON report: `{"file", "sessions"}`, plus
/// `"compare_file"` / `"compare_sessions"` when a second log was analyzed.
/// Key order is alphabetical (matching Python's `sort_keys=True`, the
/// contract the engine gtest and any other JSON consumer reads).
pub fn render_json(
    file: &Path,
    sessions: &[SessionSummary],
    compare: Option<(&Path, &[SessionSummary])>,
) -> anyhow::Result<String> {
    let payload = JsonPayload {
        compare_file: compare.map(|(p, _)| p.display().to_string()),
        compare_sessions: compare.map(|(_, s)| s),
        file: file.display().to_string(),
        sessions,
    };
    serde_json::to_string_pretty(&payload).context("rendering perf-analyze JSON report")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn write_fixture(dir: &Path, name: &str, lines: &[&str]) -> PathBuf {
        let path = dir.join(name);
        fs::write(&path, lines.join("\n") + "\n").expect("write fixture");
        path
    }

    fn window_line(fields_json: &str) -> String {
        format!(
            r#"{{"timestamp_unix_ms":1000,"level":"info","component":"perf","message":"video-pipeline-window","fields":{fields_json}}}"#
        )
    }

    fn summary_line(fields_json: &str) -> String {
        format!(
            r#"{{"timestamp_unix_ms":1000,"level":"info","component":"perf","message":"session-perf-summary","fields":{fields_json}}}"#
        )
    }

    // --- bucket-edge math against libs/engine/src/perf_histogram.h's formula ---

    #[test]
    fn bucket_zero_low_edge_is_zero() {
        assert_eq!(
            bucket_low_edge(0, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS),
            0.0
        );
    }

    #[test]
    fn overflow_bucket_edges_are_hi() {
        let geo = DEFAULT_BUCKETS - 1; // 63
        assert_eq!(
            bucket_low_edge(geo, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS),
            DEFAULT_HI_MS
        );
        assert_eq!(
            bucket_high_edge(geo, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS),
            DEFAULT_HI_MS
        );
    }

    #[test]
    fn top_geometric_bucket_high_edge_lands_on_hi() {
        // Ratio()^kGeoBuckets == kHiMs/kLoMs exactly (by construction), so bucket
        // 62's high edge (kGeoBuckets - 1) must equal kHiMs within float error.
        let high = bucket_high_edge(62, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS);
        assert!((high - DEFAULT_HI_MS).abs() < 1e-9, "high={high}");
    }

    #[test]
    fn bucket_edges_match_the_header_formula_at_known_indices() {
        // Independently computed from libs/engine/src/perf_histogram.h's own
        // formula (BucketLowEdge/BucketHighEdge): lo * Ratio()^b.
        let cases: &[(usize, f64, f64)] = &[
            (1, 0.05787114402960286, f64::NAN),
            (29, 3.4692839393685904, 4.0154286106957535),
            (45, 35.984283650057556, 41.64903323829128),
            (62, 431.99422474198354, 499.9999999999991),
        ];
        for &(b, low, high) in cases {
            let got_low = bucket_low_edge(b, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS);
            assert!(
                (got_low - low).abs() < 1e-9,
                "bucket {b} low: got {got_low}, want {low}"
            );
            if !high.is_nan() {
                let got_high = bucket_high_edge(b, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS);
                assert!(
                    (got_high - high).abs() < 1e-9,
                    "bucket {b} high: got {got_high}, want {high}"
                );
            }
        }
    }

    // --- quantile recomputation against a synthetic, hand-computed distribution ---

    fn bucket_index(ms: f64, lo: f64, hi: f64, n_buckets: usize) -> usize {
        let geo = geo_buckets(n_buckets);
        if ms.is_nan() || ms <= 0.0 {
            return 0;
        }
        if ms >= hi {
            return geo;
        }
        if ms < lo {
            return 0;
        }
        let idx = (ms / lo).ln() / ratio(lo, hi, geo).ln();
        (idx as usize).min(geo - 1)
    }

    fn synthetic_hist(samples: &[f64]) -> Vec<u64> {
        let mut counts = vec![0u64; DEFAULT_BUCKETS];
        for &ms in samples {
            counts[bucket_index(ms, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS)] += 1;
        }
        counts
    }

    #[test]
    fn quantile_matches_hand_computed_percentile_for_a_two_mode_distribution() {
        // 200 samples near 4 ms (bucket 29), 100 samples near 40 ms (bucket 45).
        // p50 target = 150 of 300 -> falls inside bucket 29 at 75% depth.
        // p99 target = 297 of 300 -> falls inside bucket 45 at 97% depth.
        let mut samples = vec![4.0_f64; 200];
        samples.extend(std::iter::repeat_n(40.0_f64, 100));
        let hist = synthetic_hist(&samples);

        let low29 = bucket_low_edge(29, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS);
        let high29 = bucket_high_edge(29, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS);
        let expect_p50 = low29 + 0.75 * (high29 - low29);

        let low45 = bucket_low_edge(45, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS);
        let high45 = bucket_high_edge(45, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS);
        let expect_p99 = low45 + 0.97 * (high45 - low45);

        let p50 = histogram_quantile(&hist, 0.50, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS);
        let p99 = histogram_quantile(&hist, 0.99, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS);

        assert!(
            (p50 - expect_p50).abs() < 1e-9,
            "p50 got {p50}, want {expect_p50}"
        );
        assert!(
            (p99 - expect_p99).abs() < 1e-9,
            "p99 got {p99}, want {expect_p99}"
        );
        // Cross-checked against libs/engine/src/perf_histogram.h's formula run
        // independently: p50 ~= 3.878892442863963, p99 ~= 41.47909075064427.
        assert!((p50 - 3.878892442863963).abs() < 1e-9);
        assert!((p99 - 41.47909075064427).abs() < 1e-9);
    }

    #[test]
    fn empty_histogram_quantile_is_zero() {
        let hist = vec![0u64; DEFAULT_BUCKETS];
        assert_eq!(
            histogram_quantile(&hist, 0.50, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS),
            0.0
        );
    }

    #[test]
    fn overflow_bucket_quantile_reports_hi() {
        let mut hist = vec![0u64; DEFAULT_BUCKETS];
        hist[DEFAULT_BUCKETS - 1] = 5;
        assert_eq!(
            histogram_quantile(&hist, 0.99, DEFAULT_LO_MS, DEFAULT_HI_MS, DEFAULT_BUCKETS),
            DEFAULT_HI_MS
        );
    }

    // --- session grouping ---

    #[test]
    fn groups_a_window_and_its_closing_summary_into_one_session() {
        let dir = tempfile::tempdir().unwrap();
        let win = window_line(
            r#"{"preset":"P4","codec":"av1","resolution":"1920x1080@60","tick_budget_ms":"16.667","tick_p99_ms":"12.0","dropped_backpressure":"0","slot_stalls":"0"}"#,
        );
        let hist = synthetic_hist(&[4.0; 10]);
        let hist_csv = hist
            .iter()
            .map(|c| c.to_string())
            .collect::<Vec<_>>()
            .join(",");
        let sum = summary_line(&format!(
            r#"{{"hist_lo_ms":"0.05","hist_hi_ms":"500.0","hist_buckets":"64","encode_hist":"{hist_csv}","tick_hist":"{hist_csv}","dropped_backpressure":"2","slot_stalls":"1","preset":"P4","codec":"av1","resolution":"1920x1080@60"}}"#
        ));
        let path = write_fixture(dir.path(), "engine.jsonl", &[&win, &sum]);

        let sessions = analyze_file(&path).expect("analyze_file");
        assert_eq!(sessions.len(), 1);
        let s = &sessions[0];
        assert!(s.has_summary);
        assert_eq!(s.windows, 1);
        assert_eq!(s.codec, "av1");
        assert_eq!(s.preset, "P4");
        assert_eq!(s.resolution, "1920x1080@60");
        assert_eq!(s.dropped_backpressure, 2);
        assert_eq!(s.slot_stalls, 1);
        assert_eq!(s.encode_count, 10);
        assert_eq!(s.tick_budget_overruns, 0); // tick_p99 12.0 < budget 16.667
    }

    #[test]
    fn windows_with_no_trailing_summary_form_an_open_session() {
        let dir = tempfile::tempdir().unwrap();
        let win = window_line(
            r#"{"preset":"P5","codec":"h264","resolution":"1280x720@30","tick_budget_ms":"10.0","tick_p99_ms":"12.0","encode_p50_ms":"4.0","encode_p99_ms":"40.0","dropped_backpressure":"1","slot_stalls":"0"}"#,
        );
        let path = write_fixture(dir.path(), "engine.jsonl", &[&win]);

        let sessions = analyze_file(&path).expect("analyze_file");
        assert_eq!(sessions.len(), 1);
        let s = &sessions[0];
        assert!(!s.has_summary);
        assert_eq!(s.windows, 1);
        assert_eq!(s.encode_p50_ms, 4.0);
        assert_eq!(s.encode_p99_ms, 40.0);
        assert_eq!(s.tick_budget_overruns, 1); // tick_p99 12.0 > budget 10.0
    }

    #[test]
    fn non_perf_and_non_json_lines_are_skipped() {
        let dir = tempfile::tempdir().unwrap();
        let win = window_line(r#"{"preset":"P4","codec":"av1","resolution":"r"}"#);
        let path = write_fixture(
            dir.path(),
            "engine.jsonl",
            &[
                &win,
                r#"{"component":"app","message":"unrelated","fields":{"x":"1"}}"#,
                "not json at all",
                "",
            ],
        );

        let sessions = analyze_file(&path).expect("analyze_file");
        assert_eq!(sessions.len(), 1);
        assert_eq!(sessions[0].windows, 1);
    }

    // --- report / delta text formatting ---

    #[test]
    fn print_report_renders_no_records_placeholder() {
        assert_eq!(print_report(&[]), "  (no perf records found)\n");
    }

    #[test]
    fn print_report_row_matches_the_scripts_layout() {
        let s = SessionSummary {
            codec: "av1".to_string(),
            preset: "P4".to_string(),
            resolution: "1920x1080@60".to_string(),
            encode_p50_ms: 3.878892442863963,
            encode_p99_ms: 41.47909075064427,
            tick_p50_ms: 12.052624830481395,
            tick_p99_ms: 12.914491902461863,
            dropped_backpressure: 3,
            slot_stalls: 1,
            tick_budget_overruns: 0,
            has_summary: true,
            ..SessionSummary::default()
        };
        let report = print_report(std::slice::from_ref(&s));
        let expected_row = "  #0    av1  P4 1920x1080@60  enc p50/p99=  3.879/ 41.479 ms  tick p50/p99= 12.053/ 12.914 ms  drops(bp)=     3 stalls=     1  overruns=  0  [session-hist]\n";
        assert!(report.starts_with(expected_row), "report was:\n{report}");
    }

    #[test]
    fn print_delta_with_no_sessions_reports_nothing_to_compare() {
        let delta = print_delta(&[], &[]);
        assert_eq!(
            delta,
            "Before/after delta (matched by session index):\n  (nothing to compare)\n"
        );
    }

    #[test]
    fn print_delta_row_matches_the_scripts_layout() {
        let before = SessionSummary {
            codec: "av1".to_string(),
            preset: "P4".to_string(),
            encode_p99_ms: 41.47909075064427,
            tick_p99_ms: 12.914491902461863,
            dropped_backpressure: 3,
            slot_stalls: 1,
            ..SessionSummary::default()
        };
        let after = SessionSummary {
            codec: "av1".to_string(),
            preset: "P4".to_string(),
            encode_p99_ms: 35.78247275851701,
            tick_p99_ms: 11.157772848100033,
            dropped_backpressure: 5,
            slot_stalls: 2,
            ..SessionSummary::default()
        };
        let delta = print_delta(std::slice::from_ref(&before), std::slice::from_ref(&after));
        let expected_row = "  #0    av1  P4  \u{394}enc_p99=  -5.697 ms  \u{394}tick_p99=  -1.757 ms  \u{394}drops(bp)=    +2  \u{394}stalls=    +1\n";
        assert!(delta.ends_with(expected_row), "delta was:\n{delta}");
    }

    // --- JSON output contract (the engine gtest greps these substrings) ---

    #[test]
    fn json_output_carries_the_keys_the_engine_gtest_greps_for() {
        let s = SessionSummary {
            codec: "av1".to_string(),
            preset: "P4".to_string(),
            resolution: "1920x1080@60".to_string(),
            encode_p50_ms: 3.878892442863963,
            encode_p99_ms: 41.47909075064427,
            has_summary: true,
            ..SessionSummary::default()
        };
        let json = render_json(Path::new("engine.jsonl"), std::slice::from_ref(&s), None).unwrap();

        assert!(json.contains("\"sessions\""), "{json}");
        assert!(json.contains("\"has_summary\": true"), "{json}");
        assert!(json.contains("\"codec\": \"av1\""), "{json}");
        assert!(json.contains("\"preset\": \"P4\""), "{json}");
        assert!(
            json.contains("\"encode_p50_ms\": 3.878892442863963"),
            "{json}"
        );
        assert!(
            json.contains("\"encode_p99_ms\": 41.47909075064427"),
            "{json}"
        );
    }

    #[test]
    fn json_keys_are_alphabetically_sorted_matching_the_scripts_sort_keys_output() {
        let s = SessionSummary::default();
        let json = render_json(Path::new("f"), std::slice::from_ref(&s), None).unwrap();
        let value: serde_json::Value = serde_json::from_str(&json).unwrap();
        let obj = value.as_object().unwrap();
        let keys: Vec<&String> = obj.keys().collect();
        let mut sorted = keys.clone();
        sorted.sort();
        assert_eq!(keys, sorted, "top-level keys not alphabetical: {keys:?}");

        let session = obj["sessions"][0].as_object().unwrap();
        let session_keys: Vec<&String> = session.keys().collect();
        let mut sorted_session = session_keys.clone();
        sorted_session.sort();
        assert_eq!(
            session_keys, sorted_session,
            "session keys not alphabetical: {session_keys:?}"
        );
    }

    #[test]
    fn json_output_omits_compare_fields_without_a_second_log() {
        let s = SessionSummary::default();
        let json = render_json(Path::new("f"), std::slice::from_ref(&s), None).unwrap();
        assert!(!json.contains("compare_file"));
        assert!(!json.contains("compare_sessions"));
    }

    #[test]
    fn json_output_includes_compare_fields_with_a_second_log() {
        let s = SessionSummary::default();
        let compare = vec![SessionSummary::default()];
        let json = render_json(
            Path::new("f"),
            std::slice::from_ref(&s),
            Some((Path::new("g"), compare.as_slice())),
        )
        .unwrap();
        assert!(json.contains("\"compare_file\": \"g\""));
        assert!(json.contains("\"compare_sessions\""));
    }
}
