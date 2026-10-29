use std::io::Write;
use std::path::Path;
use std::process::{Command, Output, Stdio};

use exo_dev::git::Git;
use exo_dev::test_support::fixture_repo_committed;

fn push(root: &Path, input: &str) -> Output {
    let mut child = Command::new(env!("CARGO_BIN_EXE_exo-dev"))
        .args(["hook", "pre-push"])
        .current_dir(root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    child
        .stdin
        .take()
        .unwrap()
        .write_all(input.as_bytes())
        .unwrap();
    child.wait_with_output().unwrap()
}

#[test]
fn stdin_tag_deletion_and_unchanged_pushes_skip_verification() {
    let dir = fixture_repo_committed(&[("README.md", "docs\n")]);
    let head = Git::new(dir.path()).head().unwrap();
    let zero = "0".repeat(40);
    for input in [
        String::new(),
        format!(
            "refs/tags/v1 {head} refs/tags/v1 {zero}\n(delete) {zero} refs/heads/deleted {head}\nrefs/heads/same {head} refs/heads/same {head}\n"
        ),
    ] {
        let output = push(dir.path(), &input);
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(output.stdout.is_empty());
    }
}

#[test]
fn stdin_checks_later_branch_heads_before_running_any_guardrail() {
    let dir = fixture_repo_committed(&[("README.md", "docs\n")]);
    let head = Git::new(dir.path()).head().unwrap();
    let zero = "0".repeat(40);
    let other = "b".repeat(40);
    let output = push(
        dir.path(),
        &format!(
            "refs/heads/first {head} refs/heads/first {zero}\nrefs/heads/second {other} refs/heads/second {zero}\n"
        ),
    );
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("checked-out HEAD"));
    assert!(output.stdout.is_empty());
}

#[test]
fn malformed_stdin_refuses_the_push() {
    let dir = fixture_repo_committed(&[("README.md", "docs\n")]);
    let output = push(dir.path(), "bad input\n");
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("malformed pre-push"));
}

#[test]
fn staged_broken_rust_cannot_be_certified_by_an_unstaged_fix() {
    let dir = fixture_repo_committed(&[("tools/tool.rs", "fn main() {}\n")]);
    std::fs::write(dir.path().join("tools/tool.rs"), "this is broken Rust\n").unwrap();
    assert_eq!(Git::new(dir.path()).run(&["add", "tools/tool.rs"]).0, 0);
    std::fs::write(dir.path().join("tools/tool.rs"), "fn main() {}\n").unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_exo-dev"))
        .args([
            "verify",
            "--fast",
            "--staged",
            "--dry-run",
            "--output",
            "silent",
        ])
        .current_dir(dir.path())
        .output()
        .unwrap();
    assert!(
        !output.status.success(),
        "staged verification accepted unstaged Rust bytes"
    );
    assert!(String::from_utf8_lossy(&output.stderr).contains("index"));
}

#[test]
fn staged_cpp_allows_unrelated_unstaged_cpp() {
    let dir = fixture_repo_committed(&[("a.cpp", "int a;\n"), ("b.cpp", "int b;\n")]);
    std::fs::write(dir.path().join("a.cpp"), "int a = 1;\n").unwrap();
    assert_eq!(Git::new(dir.path()).run(&["add", "a.cpp"]).0, 0);
    std::fs::write(dir.path().join("b.cpp"), "int b = 2;\n").unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_exo-dev"))
        .args([
            "verify",
            "--fast",
            "--staged",
            "--dry-run",
            "--output",
            "silent",
        ])
        .current_dir(dir.path())
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn staged_cpp_violation_cannot_be_hidden_by_an_unstaged_fix() {
    let dir = fixture_repo_committed(&[("a.cpp", "int a;\n")]);
    std::fs::write(dir.path().join("a.cpp"), "int a; // Task 123\n").unwrap();
    assert_eq!(Git::new(dir.path()).run(&["add", "a.cpp"]).0, 0);
    std::fs::write(dir.path().join("a.cpp"), "int a;\n").unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_exo-dev"))
        .args([
            "verify",
            "--fast",
            "--staged",
            "--dry-run",
            "--output",
            "silent",
        ])
        .current_dir(dir.path())
        .output()
        .unwrap();
    assert!(
        !output.status.success(),
        "staged verification accepted unstaged C++ bytes"
    );
    assert!(String::from_utf8_lossy(&output.stderr).contains("index"));
}

fn install_staged_verification_hook(root: &Path) -> std::path::PathBuf {
    let receipt = root.join(".git/candidate-verification.json");
    let binary = env!("CARGO_BIN_EXE_exo-dev").replace('\\', "/");
    let receipt_arg = receipt.to_string_lossy().replace('\\', "/");
    let hook = root.join(".git/hooks/pre-commit");
    std::fs::write(&hook, format!("#!/bin/sh\nexec \"{binary}\" verify --fast --staged --dry-run --output silent --result-path \"{receipt_arg}\"\n")).unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(hook, std::fs::Permissions::from_mode(0o755)).unwrap();
    }
    receipt
}

#[test]
fn commit_all_verifies_gits_temporary_candidate_index() {
    let dir = fixture_repo_committed(&[("tools/tool.rs", "fn main() {}\n")]);
    let receipt = install_staged_verification_hook(dir.path());
    std::fs::write(
        dir.path().join("tools/tool.rs"),
        "fn main() { println!(\"changed\"); }\n",
    )
    .unwrap();
    let output = Command::new("git")
        .args(["commit", "-am", "fix(tooling): change tool"])
        .current_dir(dir.path())
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let value: serde_json::Value =
        serde_json::from_slice(&std::fs::read(receipt).unwrap()).unwrap();
    assert_eq!(value["scope"]["changedFileCount"], 1);
    assert_eq!(value["scope"]["categories"], serde_json::json!(["rust"]));
    let rust = value["checks"]
        .as_array()
        .unwrap()
        .iter()
        .find(|check| check["name"] == "rust")
        .unwrap();
    assert_eq!(rust["status"], "PASS");
}

#[test]
fn commit_only_uses_candidate_scope_and_preserves_other_staged_paths() {
    let dir = fixture_repo_committed(&[
        ("tools/tool.rs", "fn main() {}\n"),
        ("docs/other.md", "original\n"),
    ]);
    let receipt = install_staged_verification_hook(dir.path());
    std::fs::write(dir.path().join("docs/other.md"), "staged separately\n").unwrap();
    assert_eq!(Git::new(dir.path()).run(&["add", "docs/other.md"]).0, 0);
    std::fs::write(
        dir.path().join("tools/tool.rs"),
        "fn main() { println!(\"changed\"); }\n",
    )
    .unwrap();
    let output = Command::new("git")
        .args([
            "commit",
            "--only",
            "tools/tool.rs",
            "-m",
            "fix(tooling): change tool",
        ])
        .current_dir(dir.path())
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let value: serde_json::Value =
        serde_json::from_slice(&std::fs::read(receipt).unwrap()).unwrap();
    assert_eq!(value["scope"]["changedFileCount"], 1);
    assert_eq!(value["scope"]["categories"], serde_json::json!(["rust"]));
    assert_eq!(
        Git::new(dir.path())
            .run(&["diff", "--cached", "--name-only"])
            .1
            .trim(),
        "docs/other.md"
    );
}

#[test]
fn an_unsafe_candidate_index_cannot_be_hidden_by_safe_permanent_and_working_trees() {
    let dir = fixture_repo_committed(&[("tools/tool.rs", "fn main() {}\n")]);
    let candidate = dir.path().join(".git/candidate-index");
    std::fs::copy(dir.path().join(".git/index"), &candidate).unwrap();
    std::fs::write(dir.path().join("tools/tool.rs"), "broken Rust\n").unwrap();
    let mut command = exo_dev::process::command("git");
    command
        .arg("-C")
        .arg(dir.path())
        .args(["add", "tools/tool.rs"])
        .env("GIT_INDEX_FILE", &candidate);
    assert_eq!(exo_dev::process::query(command).unwrap().0, 0);
    std::fs::write(dir.path().join("tools/tool.rs"), "fn main() {}\n").unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_exo-dev"))
        .args([
            "verify",
            "--fast",
            "--staged",
            "--dry-run",
            "--output",
            "silent",
        ])
        .current_dir(dir.path())
        .env("GIT_INDEX_FILE", &candidate)
        .output()
        .unwrap();
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("index"));
}
