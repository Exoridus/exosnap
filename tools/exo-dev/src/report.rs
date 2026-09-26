//! Console summary, end-of-run evidence list and the machine-readable receipt.

use std::path::Path;

use serde_json::{Map, Value, json};

use crate::run::{RunResult, Status};
use crate::scope::Scope;

/// One line per check, the status column padded so the lines align.
pub fn summary(run: &RunResult) -> Vec<String> {
    let width = run
        .checks
        .iter()
        .map(|c| c.status.as_str().len())
        .max()
        .unwrap_or(0);
    run.checks
        .iter()
        .map(|check| {
            let mut line = format!("{:<width$}  {}", check.status.as_str(), check.name);
            if check.duration_ms >= 1000 {
                line = format!(
                    "{:<pad$} {:>6.1}s",
                    line,
                    check.duration_ms as f64 / 1000.0,
                    pad = width + 18
                );
            }
            if !check.detail.is_empty() {
                line = format!("{line}  -- {}", check.detail);
            }
            line
        })
        .collect()
}

/// Where the evidence for each failed check is. Repeated at the end because the
/// log path a failing step printed has scrolled away by the time the run ends.
pub fn failure_report(run: &RunResult) -> Vec<String> {
    let mut lines = Vec::new();
    for check in run.checks.iter().filter(|c| c.status == Status::Fail) {
        lines.push(format!("{}:", check.name));
        if let Some(log) = check.evidence.get("log").and_then(Value::as_str) {
            lines.push(format!("  log: {log}"));
        }
        for diagnostic in &check.diagnostics {
            lines.push(format!("  diagnosis: {diagnostic}"));
        }
    }
    lines
}

pub struct ReceiptFacts<'a> {
    pub profile: &'a str,
    pub mode: &'a str,
    pub platform: &'a str,
    pub head: &'a str,
    pub base: &'a str,
    pub dirty: bool,
    pub scope: &'a Scope,
}

/// Deliberately small. It records which HEAD a run is about, whether the tree was
/// clean and which build a test result stood on, so an old green cannot be
/// mistaken for a statement about new source.
pub fn receipt(run: &RunResult, facts: &ReceiptFacts) -> Value {
    let checks: Vec<Value> = run
        .checks
        .iter()
        .map(|check| {
            let mut entry = Map::new();
            entry.insert("name".into(), check.name.into());
            entry.insert("status".into(), check.status.as_str().into());
            entry.insert("detail".into(), check.detail.clone().into());
            entry.insert("durationMs".into(), (check.duration_ms as u64).into());
            entry.insert("dependsOn".into(), json!(check.depends_on));
            entry.insert("implementation".into(), check.implementation.into());
            entry.insert("evidence".into(), Value::Object(check.evidence.clone()));
            entry.insert("diagnostics".into(), json!(check.diagnostics));
            Value::Object(entry)
        })
        .collect();
    json!({
        "schema": 1,
        "tool": concat!("exo-dev ", env!("CARGO_PKG_VERSION")),
        "profile": facts.profile,
        "mode": facts.mode,
        "platform": facts.platform,
        "head": facts.head,
        "base": facts.base,
        "dirty": facts.dirty,
        "result": if run.passed { "passed" } else { "failed" },
        "toolMissing": run.tool_missing,
        "scope": {
            "categories": facts.scope.categories,
            "changedFileCount": facts.scope.changed_files.len(),
            "escalationReasons": facts.scope.escalation_reasons,
        },
        "checks": checks,
    })
}

pub fn save(document: &Value, path: &Path) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let mut text = serde_json::to_string_pretty(document).expect("receipt serialises");
    text.push('\n');
    std::fs::write(path, text)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::plan::{self, tests::input};
    use crate::profile::Profile;
    use crate::run::{self, tests::Fake};

    #[test]
    fn the_summary_names_every_check_and_nothing_downstream_reads_as_passed() {
        let plan = plan::build(&input(Profile::PrePush, &Scope::everything()));
        let run = run::run(&plan, &mut Fake::failing(&["format"]));
        let lines = summary(&run);
        assert_eq!(lines.len(), plan.checks.len());
        let text = lines.join("\n");
        assert!(regex::Regex::new(r"FAIL\s+format").unwrap().is_match(&text));
        assert!(!regex::Regex::new(r"PASS\s+build").unwrap().is_match(&text));
    }

    #[test]
    fn the_failure_report_lists_log_and_diagnosis_of_every_failed_check() {
        let plan = plan::build(&input(Profile::PrePush, &Scope::everything()));
        let mut fake = Fake::failing(&["tests"]);
        fake.diagnosis = vec!["C:/logs/record_controls_qml_tests.txt".into()];
        let run = run::run(&plan, &mut fake);
        let report = failure_report(&run).join("\n");
        assert!(report.contains("tests:"));
        assert!(report.contains("log: C:/logs/x.log"));
        assert!(report.contains("diagnosis: C:/logs/record_controls_qml_tests.txt"));
        assert!(!report.contains("build"));
    }

    #[test]
    fn the_receipt_records_the_run_truthfully() {
        let scope = Scope::of(&["libs/engine/src/muxer.cpp".to_string()]);
        let plan = plan::build(&input(Profile::PreCommit, &scope));
        let run = run::run(&plan, &mut Fake::failing(&["build"]));
        let document = receipt(
            &run,
            &ReceiptFacts {
                profile: "pre-commit",
                mode: "Fast",
                platform: "windows",
                head: "abc1234",
                base: "def5678",
                dirty: true,
                scope: &scope,
            },
        );
        assert_eq!(document["mode"], "Fast");
        assert_eq!(document["profile"], "pre-commit");
        assert_eq!(document["head"], "abc1234");
        assert_eq!(document["base"], "def5678");
        assert_eq!(document["result"], "failed");
        assert_eq!(document["dirty"], true);
        let checks = document["checks"].as_array().unwrap();
        let tests = checks.iter().find(|c| c["name"] == "tests").unwrap();
        assert_eq!(tests["status"], "SKIPPED_DEPENDENCY");
        assert!(
            tests["dependsOn"]
                .as_array()
                .unwrap()
                .contains(&"build".into())
        );
        assert_eq!(tests["implementation"], "legacy");
        let build = checks.iter().find(|c| c["name"] == "build").unwrap();
        assert_eq!(
            build["evidence"]["buildDir"],
            "build/windows-x64-ninja-debug"
        );
    }

    #[test]
    fn the_receipt_names_a_missing_tool() {
        let plan = plan::build(&input(Profile::PrePush, &Scope::everything()));
        let mut fake = Fake::failing(&[]);
        fake.missing = vec!["cppcheck"];
        let run = run::run(&plan, &mut fake);
        let scope = Scope::everything();
        let document = receipt(
            &run,
            &ReceiptFacts {
                profile: "pre-push",
                mode: "Full",
                platform: "windows",
                head: "abc",
                base: "",
                dirty: false,
                scope: &scope,
            },
        );
        assert_eq!(document["result"], "failed");
        assert_eq!(document["toolMissing"], json!(["cppcheck"]));
    }
}
