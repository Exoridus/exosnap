//! `<BuildDir>/Testing/last-run-receipt.json`: what one test run established.
//!
//! The field names, their order and their JSON types are a contract that
//! `docs/dev/build-and-test.md` documents and downstream readers parse. A
//! receipt starts out as an invalid run: every field has to be earned before
//! `reusable` can become true.

use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use serde_json::{Value, json};
use sha2::{Digest, Sha256};

use super::catalog::PhaseViolations;
use super::fingerprint::SourceIdentity;

/// The exit code of a run that produced no trustworthy verdict: the suite may
/// even have passed, but the run cannot be read as one. Distinct from a test
/// failure on purpose, and never the only record: the receipt carries the raw
/// build and ctest codes beside it.
pub const EXIT_INVALID_RUN: i32 = 4;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Freshness {
    Fresh,
    Stale,
    Unknown,
}

impl Freshness {
    pub fn as_str(self) -> &'static str {
        match self {
            Freshness::Fresh => "fresh",
            Freshness::Stale => "stale",
            Freshness::Unknown => "unknown",
        }
    }
}

#[derive(Clone, Debug)]
pub struct Receipt {
    pub run_id: String,
    pub started_utc: String,
    pub finished_utc: Option<String>,
    pub build_dir: String,
    pub generator: String,
    pub config: String,
    pub jobs: usize,
    pub freshness: Freshness,
    pub freshness_detail: String,
    pub allow_stale: bool,
    pub no_build: bool,
    /// `skipped`, `not-started`, `failed` or `succeeded`.
    pub build_status: &'static str,
    pub build_exit_code: Option<i32>,
    pub ctest_args: Vec<String>,
    pub exclude_label: String,
    pub exclude_pattern: String,
    /// The phase name, or empty for every phase.
    pub phase: String,
    pub filter: String,
    pub tests_registered: usize,
    pub tests_disabled: usize,
    pub tests_selected: usize,
    pub tests_accounted: usize,
    pub tests_expected: usize,
    pub tests_passed: usize,
    pub tests_failed: usize,
    pub census_mismatch: bool,
    pub phase_violations: Option<PhaseViolations>,
    pub ctest_exit_code: Option<i32>,
    pub source_before: Option<SourceIdentity>,
    pub source_after: Option<SourceIdentity>,
    pub source_drift: Option<bool>,
    pub log: String,
    pub rescued_config_dir: Option<String>,
    /// `not-needed`, `rescued` or `failed`.
    pub rescue_status: &'static str,
    pub rescue_detail: Option<String>,
    pub invalid_reasons: Vec<String>,
    pub exit_code: i32,
    pub reusable: bool,
}

impl Receipt {
    pub fn to_json(&self) -> Value {
        json!({
            "run_id": self.run_id,
            "started_utc": self.started_utc,
            "finished_utc": self.finished_utc,
            "build_dir": self.build_dir,
            "generator": self.generator,
            "config": self.config,
            "jobs": self.jobs,
            "freshness": self.freshness.as_str(),
            "freshness_detail": self.freshness_detail,
            "allow_stale": self.allow_stale,
            "no_build": self.no_build,
            "build_status": self.build_status,
            "build_exit_code": self.build_exit_code,
            "ctest_args": self.ctest_args,
            "exclude_label": self.exclude_label,
            "exclude_pattern": self.exclude_pattern,
            "phase": self.phase,
            "filter": self.filter,
            "tests_registered": self.tests_registered,
            "tests_disabled": self.tests_disabled,
            "tests_selected": self.tests_selected,
            "tests_accounted": self.tests_accounted,
            "tests_expected": self.tests_expected,
            "tests_passed": self.tests_passed,
            "tests_failed": self.tests_failed,
            "census_mismatch": self.census_mismatch,
            "phase_violations": self.phase_violations.as_ref().map(|v| json!({
                "missing": v.missing,
                "multiple": v.multiple,
                "unknown": v.unknown,
            })),
            "ctest_exit_code": self.ctest_exit_code,
            "source_before": self.source_before.as_ref().map(source_identity),
            "source_after": self.source_after.as_ref().map(source_identity),
            "source_drift": self.source_drift,
            "log": self.log,
            "rescued_config_dir": self.rescued_config_dir,
            "rescue_status": self.rescue_status,
            "rescue_detail": self.rescue_detail,
            "invalid_reasons": self.invalid_reasons,
            "exit_code": self.exit_code,
            "reusable": self.reusable,
        })
    }
}

/// A source identity as the receipt records it: `ok` and `reason` beside the
/// identity, with every identity field null (and the counts zero) when none
/// could be produced.
pub fn source_identity(identity: &SourceIdentity) -> Value {
    match identity {
        SourceIdentity::Known {
            head,
            dirty,
            fingerprint,
            untracked_files,
            untracked_bytes,
        } => json!({
            "ok": true,
            "reason": null,
            "head": head,
            "dirty": dirty,
            "fingerprint": fingerprint,
            "untracked_files": untracked_files,
            "untracked_bytes": untracked_bytes,
        }),
        SourceIdentity::Unavailable { reason } => json!({
            "ok": false,
            "reason": reason,
            "head": null,
            "dirty": null,
            "fingerprint": null,
            "untracked_files": 0,
            "untracked_bytes": 0,
        }),
    }
}

/// Writes the receipt through a temporary file moved into place, so a reader
/// never sees half a receipt and an older one is replaced whole.
pub fn publish(path: &Path, receipt: &Receipt) -> Result<(), String> {
    let write = || -> std::io::Result<()> {
        if let Some(directory) = path.parent() {
            std::fs::create_dir_all(directory)?;
        }
        let mut temporary = path.as_os_str().to_os_string();
        temporary.push(format!(".{}.tmp", std::process::id()));
        let mut text = serde_json::to_string_pretty(&receipt.to_json())?;
        text.push('\n');
        std::fs::write(&temporary, text)?;
        std::fs::rename(&temporary, path).inspect_err(|_| {
            let _ = std::fs::remove_file(&temporary);
        })
    };
    write().map_err(|error| error.to_string())
}

/// The current UTC time in round-trip form, `2026-09-26T17:31:26.9794453Z`.
pub fn utc_now() -> String {
    let since = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    format_utc(since.as_secs(), since.subsec_nanos())
}

fn format_utc(seconds: u64, nanos: u32) -> String {
    let days = seconds / 86_400;
    let rest = seconds % 86_400;
    let (year, month, day) = civil_from_days(days);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}.{:07}Z",
        rest / 3600,
        rest % 3600 / 60,
        rest % 60,
        nanos / 100
    )
}

/// The proleptic Gregorian date `days` after 1970-01-01.
fn civil_from_days(days: u64) -> (u64, u64, u64) {
    let z = days + 719_468;
    let era = z / 146_097;
    let day_of_era = z - era * 146_097;
    let year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let mp = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = year_of_era + era * 400 + u64::from(month <= 2);
    (year, month, day)
}

/// 32 lowercase hex digits, unique per run on this host: it names the
/// per-run evidence directory and the throwaway configuration directory.
pub fn new_run_id() -> String {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let since = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let seed = format!(
        "{}:{}:{}:{:?}",
        std::process::id(),
        since.as_nanos(),
        COUNTER.fetch_add(1, Ordering::Relaxed),
        std::thread::current().id()
    );
    hex::encode(&Sha256::digest(seed.as_bytes())[..16])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utc_timestamps_use_the_round_trip_form() {
        assert_eq!(format_utc(0, 0), "1970-01-01T00:00:00.0000000Z");
        // 2026-09-26T17:31:26.9794453Z
        assert_eq!(
            format_utc(1_790_443_886, 979_445_300),
            "2026-09-26T17:31:26.9794453Z"
        );
        // A leap day.
        assert_eq!(format_utc(951_782_400, 0), "2000-02-29T00:00:00.0000000Z");
    }

    #[test]
    fn run_ids_are_32_hex_digits_and_differ() {
        let a = new_run_id();
        let b = new_run_id();
        assert_eq!(a.len(), 32);
        assert!(
            a.bytes()
                .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
        );
        assert_ne!(a, b);
    }

    #[test]
    fn an_unavailable_identity_is_recorded_with_null_identity_fields() {
        let value = source_identity(&SourceIdentity::Unavailable {
            reason: "git rev-parse HEAD failed".into(),
        });
        assert_eq!(value["ok"], false);
        assert_eq!(value["reason"], "git rev-parse HEAD failed");
        assert!(value["head"].is_null() && value["fingerprint"].is_null());
        assert_eq!(value["untracked_files"], 0);
    }
}
