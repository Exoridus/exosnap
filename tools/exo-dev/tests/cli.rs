//! The binary end to end, without a compiler: `--dry-run` runs the real scope,
//! plan and receipt code against simulated step results in this repository.

use std::path::{Path, PathBuf};
use std::process::Command;

use serde_json::Value;

fn repo_root() -> PathBuf {
    std::fs::canonicalize(Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")).unwrap()
}

fn exo_dev(args: &[&str], receipt: &Path) -> (i32, Value) {
    let output = Command::new(env!("CARGO_BIN_EXE_exo-dev"))
        .args(args)
        .arg("--result-path")
        .arg(receipt)
        .current_dir(repo_root())
        .output()
        .unwrap();
    let code = output.status.code().unwrap_or(-1);
    let text = std::fs::read_to_string(receipt).unwrap_or_else(|_| {
        panic!(
            "no receipt; stdout:\n{}\nstderr:\n{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        )
    });
    (code, serde_json::from_str(&text).unwrap())
}

fn check<'a>(receipt: &'a Value, name: &str) -> &'a Value {
    receipt["checks"]
        .as_array()
        .unwrap()
        .iter()
        .find(|c| c["name"] == name)
        .unwrap_or_else(|| panic!("{name} is not in the receipt"))
}

#[test]
fn an_all_green_scoped_run_exits_zero_and_says_so() {
    let dir = tempfile::tempdir().unwrap();
    let (code, receipt) = exo_dev(
        &["verify", "--fast", "--dry-run"],
        &dir.path().join("r.json"),
    );
    assert_eq!(code, 0);
    assert_eq!(receipt["result"], "passed");
    assert_eq!(receipt["mode"], "Fast");
    assert_eq!(receipt["profile"], "pre-commit");
}

#[test]
fn a_simulated_build_failure_exits_non_zero_and_skips_the_tests() {
    let dir = tempfile::tempdir().unwrap();
    let (code, receipt) = exo_dev(
        &["verify", "--full", "--dry-run", "--simulate-fail", "build"],
        &dir.path().join("r.json"),
    );
    assert_eq!(code, 1);
    assert_eq!(receipt["result"], "failed");
    if cfg!(windows) {
        assert_eq!(check(&receipt, "tests")["status"], "SKIPPED_DEPENDENCY");
    }
}

#[cfg(windows)]
#[test]
fn a_simulated_test_failure_carries_qml_evidence() {
    let dir = tempfile::tempdir().unwrap();
    let (code, receipt) = exo_dev(
        &["verify", "--full", "--dry-run", "--simulate-fail", "tests"],
        &dir.path().join("r.json"),
    );
    assert_ne!(code, 0);
    assert!(
        !check(&receipt, "tests")["diagnostics"]
            .as_array()
            .unwrap()
            .is_empty()
    );
}

#[test]
fn a_ci_profile_refuses_to_run_without_its_event() {
    let output = Command::new(env!("CARGO_BIN_EXE_exo-dev"))
        .args(["verify", "--profile", "ci-lint", "--dry-run"])
        .current_dir(repo_root())
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&output.stderr).contains("--event"));
}

#[test]
fn a_ci_profile_plans_its_event_specific_steps() {
    let dir = tempfile::tempdir().unwrap();
    let (code, receipt) = exo_dev(
        &[
            "verify",
            "--profile",
            "ci-guardrails",
            "--event",
            "push",
            "--dry-run",
        ],
        &dir.path().join("r.json"),
    );
    assert_eq!(code, 0);
    assert_eq!(check(&receipt, "commit-policy")["status"], "SKIP");
    assert_eq!(
        check(&receipt, "source-hygiene")["evidence"]["scope"],
        "whole-tree"
    );
    assert_eq!(receipt["mode"], "Full");
}
