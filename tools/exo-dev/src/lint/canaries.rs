//! Proves every blocking clang-tidy check still fires, against a file written
//! to be rejected by it.
//!
//! The rule for adding a blocking check is that a full pass reports zero
//! findings in repository-owned files. That rule is only half a rule: a check
//! that does not run also reports zero findings, and the two are
//! indistinguishable from the report. On this tree,
//! `bugprone-unchecked-optional-access` once reported nothing at all in a
//! whole-tree advisory pass while a single-file run with the same check
//! configuration and the same compile database reported findings in a file
//! the whole-tree pass had covered. A promotion decision resting on that pass
//! would have been resting on silence.
//!
//! So each blocking check owns a canary under
//! `tools/exo-dev/tests/fixtures/lint-canaries`: a few lines written to violate
//! exactly that check and nothing else. A check that does not reject its own
//! canary is not working here, however clean the tree looks, and this fails.
//! This is the positive half of the contract; the zero-findings pass says the
//! tree is clean, this says the instrument is not.

use std::ffi::OsStr;
use std::path::{Path, PathBuf};

use anyhow::Context;

use crate::executor::ToolMissing;

/// The blocking clang-tidy checks: a check here fails a build only after a
/// full pass reports zero findings in repository-owned files (see
/// `docs/dev/static-analysis.md`). The single source of the rule metadata; a
/// caller that needs to enforce or configure them imports this rather than
/// keeping its own copy.
pub struct BlockingRule {
    pub check: &'static str,
    pub canary: &'static str,
    pub directories: &'static [&'static str],
}

impl BlockingRule {
    pub fn applies_to(&self, file: &str) -> bool {
        let file = file.to_ascii_lowercase();
        self.directories.is_empty()
            || self
                .directories
                .iter()
                .any(|directory| file.starts_with(directory))
    }
}

macro_rules! blocking_rules {
    ($(($check:literal, $canary:literal, $directories:expr)),* $(,)?) => {
        pub const BLOCKING_RULES: &[BlockingRule] = &[
            $(BlockingRule { check: $check, canary: $canary, directories: $directories }),*
        ];
        pub const BLOCKING_CHECKS: &[&str] = &[$($check),*];
        #[cfg(test)]
        const CANARY_FILES: &[&str] = &[$($canary),*];
    };
}

blocking_rules!(
    (
        "misc-unused-using-decls",
        "misc-unused-using-decls.cpp",
        &[]
    ),
    (
        "misc-include-cleaner",
        "misc-include-cleaner.cpp",
        &["libs/engine/", "libs/capability/", "libs/update/"]
    ),
    ("misc-unused-parameters", "misc-unused-parameters.cpp", &[]),
    (
        "misc-unused-alias-decls",
        "misc-unused-alias-decls.cpp",
        &[]
    ),
    (
        "readability-redundant-declaration",
        "readability-redundant-declaration.cpp",
        &[]
    ),
    (
        "bugprone-use-after-move",
        "bugprone-use-after-move.cpp",
        &[]
    ),
    (
        "bugprone-dangling-handle",
        "bugprone-dangling-handle.cpp",
        &[]
    ),
    (
        "readability-misleading-indentation",
        "readability-misleading-indentation.cpp",
        &[]
    ),
    (
        "clang-analyzer-core.CallAndMessage",
        "clang-analyzer-core.CallAndMessage.cpp",
        &[]
    ),
    (
        "clang-analyzer-core.uninitialized.*",
        "clang-analyzer-core.uninitialized.cpp",
        &[]
    ),
    (
        "clang-analyzer-cplusplus.NewDelete*",
        "clang-analyzer-cplusplus.NewDelete.cpp",
        &[]
    ),
);

#[derive(Debug)]
pub struct CanaryFinding {
    pub check: String,
    pub detail: String,
}

#[derive(Debug)]
pub struct CanaryReport {
    pub checked: usize,
    pub failures: Vec<CanaryFinding>,
}

impl CanaryReport {
    pub fn ok(&self) -> bool {
        self.failures.is_empty()
    }
}

/// Runs clang-tidy against the one canary fixture per check under
/// `tools/exo-dev/tests/fixtures/lint-canaries`, restricted to `only` when
/// non-empty (every check otherwise). `clang_tidy` overrides autodetection,
/// which searches PATH only, matching `Get-Command clang-tidy` in the ported
/// script: unlike clang-format's VS-LLVM/standalone-LLVM/PATH search, the
/// original never looked anywhere else for clang-tidy. Either an explicit
/// path that is not a file, or no tool found anywhere, is `ToolMissing`, never
/// a silent pass: a missing instrument and a clean tree must not look alike.
pub fn run_canaries(
    repo_root: &Path,
    clang_tidy: Option<&Path>,
    only: &[String],
) -> anyhow::Result<CanaryReport> {
    let resolved = clang_tidy
        .map(Path::to_path_buf)
        .or_else(|| autodetect_clang_tidy(&std::env::var_os("PATH").unwrap_or_default()))
        .filter(|path| path.is_file());
    let Some(resolved) = resolved else {
        return Err(ToolMissing("check-lint-canaries: clang-tidy was not found.".into()).into());
    };
    let canary_dir = repo_root.join("tools/exo-dev/tests/fixtures/lint-canaries");
    run(&canary_dir, only, |check, path| {
        invoke(&resolved, check, path)
    })
}

fn autodetect_clang_tidy(path_var: &OsStr) -> Option<PathBuf> {
    let exe_name = if cfg!(windows) {
        "clang-tidy.exe"
    } else {
        "clang-tidy"
    };
    std::env::split_paths(path_var)
        .map(|dir| dir.join(exe_name))
        .find(|candidate| candidate.is_file())
}

/// `run_canaries`'s check/skip/fail logic with clang-tidy's invocation
/// injected, so a test can exercise it without a real clang-tidy binary.
fn run(
    canary_dir: &Path,
    only: &[String],
    mut invoke: impl FnMut(&str, &Path) -> anyhow::Result<String>,
) -> anyhow::Result<CanaryReport> {
    let mut checked = 0;
    let mut failures = Vec::new();
    for wanted in only {
        anyhow::ensure!(
            BLOCKING_CHECKS.contains(&wanted.as_str()),
            "unknown blocking canary: {wanted}"
        );
    }
    let diagnostic = regex::Regex::new(r"^.+:\d+:\d+: (?:warning|error): .*\[([^\]]+)\]$")
        .expect("clang-tidy diagnostic");

    for rule in BLOCKING_RULES {
        let check = rule.check;
        let file = rule.canary;
        if !only.is_empty() && !only.iter().any(|wanted| wanted == check) {
            continue;
        }

        let path = canary_dir.join(file);
        if !path.is_file() {
            failures.push(CanaryFinding {
                check: check.to_string(),
                detail: format!("its canary {file} is missing"),
            });
            continue;
        }

        checked += 1;
        let output = invoke(check, &path)?;
        // Unknown-check warnings and echoed arguments are not findings.
        let needle = check.trim_end_matches('*');
        let fired = output
            .lines()
            .filter_map(|line| diagnostic.captures(line.trim_end()))
            .any(|record| {
                record[1].split(',').any(|reported| {
                    if check.ends_with('*') {
                        reported.starts_with(needle)
                    } else {
                        reported == check
                    }
                })
            });
        if !fired {
            failures.push(CanaryFinding {
                check: check.to_string(),
                detail: "did not fire on its own canary. The check is not working here, so a \
                          zero-findings pass proves nothing about it."
                    .to_string(),
            });
        }
    }

    Ok(CanaryReport { checked, failures })
}

/// `--checks="-*,<check>"` rather than the repository configuration: this asks
/// whether THIS check fires, and a canary that tripped some other check would
/// answer a different question. The canaries are not in any compile database,
/// so `--` and the trailing compile arguments stand in for one.
fn invoke(tool: &Path, check: &str, path: &Path) -> anyhow::Result<String> {
    let mut command = crate::process::command(&tool.to_string_lossy());
    command.args([
        format!("--checks=-*,{check}"),
        "--quiet".to_string(),
        path.display().to_string(),
        "--".to_string(),
        "-std=c++20".to_string(),
        "-x".to_string(),
        "c++".to_string(),
    ]);
    let output = command
        .output()
        .with_context(|| format!("could not run {}", tool.display()))?;
    let mut combined = String::from_utf8_lossy(&output.stdout).into_owned();
    combined.push_str(&String::from_utf8_lossy(&output.stderr));
    Ok(combined)
}

/// The items of `.clang-tidy`'s `Checks: >` YAML folded block scalar: every
/// comma-separated entry on the indented lines following it, up to the first
/// blank line. Not a YAML parser; `.clang-tidy` is not rewritten to fit one,
/// this only reads the one block it needs to compare against
/// `BLOCKING_CHECKS`.
#[cfg(test)]
fn parse_checks_block(text: &str) -> Vec<String> {
    let mut items = Vec::new();
    let mut in_block = false;
    for line in text.lines() {
        if !in_block {
            if line.trim_start().starts_with("Checks:") {
                in_block = true;
            }
            continue;
        }
        if line.starts_with(char::is_whitespace) && !line.trim().is_empty() {
            for item in line.split(',') {
                let item = item.trim();
                if !item.is_empty() {
                    items.push(item.to_string());
                }
            }
        } else {
            break;
        }
    }
    items
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write_canary_files(dir: &Path) {
        for file in CANARY_FILES {
            std::fs::write(dir.join(file), "canary\n").unwrap();
        }
    }

    #[test]
    fn blocking_checks_and_canary_files_stay_in_step() {
        assert_eq!(BLOCKING_CHECKS.len(), CANARY_FILES.len());
    }

    #[test]
    fn rule_scopes_match_whole_directories() {
        let rule = BlockingRule {
            check: "misc-include-cleaner",
            canary: "include.cpp",
            directories: &["libs/engine/"],
        };
        assert!(rule.applies_to("libs/engine/src/session.cpp"));
        assert!(!rule.applies_to("libs/engine-extra/session.cpp"));
        assert!(!rule.applies_to("app/session.cpp"));
        assert!(BLOCKING_RULES[0].applies_to("app/session.cpp"));
    }

    #[test]
    fn a_missing_canary_file_is_a_failure_not_a_skip() {
        let dir = tempfile::tempdir().unwrap();
        let report = run(dir.path(), &[], |_check, _path| Ok(String::new())).unwrap();
        assert_eq!(report.checked, 0);
        assert_eq!(report.failures.len(), BLOCKING_CHECKS.len());
        assert!(report.failures[0].detail.contains("is missing"));
    }

    #[test]
    fn a_canary_whose_output_does_not_mention_the_check_is_a_failure() {
        let dir = tempfile::tempdir().unwrap();
        write_canary_files(dir.path());
        let report = run(dir.path(), &[], |_check, _path| Ok(String::new())).unwrap();
        assert!(!report.ok());
        assert_eq!(report.checked, BLOCKING_CHECKS.len());
        assert_eq!(report.failures.len(), BLOCKING_CHECKS.len());
    }

    #[test]
    fn an_unknown_check_warning_does_not_prove_a_canary_fired() {
        let dir = tempfile::tempdir().unwrap();
        write_canary_files(dir.path());
        let report = run(dir.path(), &[], |check, _| {
            Ok(format!("warning: unknown check name '{check}'"))
        })
        .unwrap();
        assert_eq!(report.failures.len(), BLOCKING_CHECKS.len());
    }

    #[test]
    fn a_canary_whose_output_mentions_the_check_passes() {
        let dir = tempfile::tempdir().unwrap();
        write_canary_files(dir.path());
        let report = run(dir.path(), &[], |check, _path| {
            Ok(format!(
                "canary.cpp:1:1: warning: deliberate violation [{check}]"
            ))
        })
        .unwrap();
        assert!(report.ok());
        assert_eq!(report.checked, BLOCKING_CHECKS.len());
    }

    #[test]
    fn a_wildcard_check_is_matched_with_its_suffix_stripped() {
        let dir = tempfile::tempdir().unwrap();
        write_canary_files(dir.path());
        let only = vec!["clang-analyzer-cplusplus.NewDelete*".to_string()];
        let report = run(dir.path(), &only, |_check, _path| {
            Ok(
                "canary.cpp:1:1: warning: found a leak [clang-analyzer-cplusplus.NewDeleteLeaks]"
                    .to_string(),
            )
        })
        .unwrap();
        assert!(report.ok());
        assert_eq!(report.checked, 1);
    }

    #[test]
    fn only_restricts_to_the_named_checks() {
        let dir = tempfile::tempdir().unwrap();
        write_canary_files(dir.path());
        let only = vec!["bugprone-dangling-handle".to_string()];
        let report = run(dir.path(), &only, |check, _path| {
            Ok(format!(
                "canary.cpp:1:1: warning: deliberate violation [{check}]"
            ))
        })
        .unwrap();
        assert_eq!(report.checked, 1);
        assert!(report.ok());
    }

    #[test]
    fn an_unknown_requested_check_is_not_an_empty_success() {
        let dir = tempfile::tempdir().unwrap();
        let result = run(dir.path(), &["misspelled-check".into()], |_, _| {
            Ok(String::new())
        });
        assert!(result.is_err());
    }

    #[test]
    fn a_missing_clang_tidy_is_tool_missing_not_a_silent_pass() {
        let repo_root = tempfile::tempdir().unwrap();
        let missing_tool = repo_root.path().join("not-a-tool.exe");
        let error = run_canaries(repo_root.path(), Some(&missing_tool), &[]).unwrap_err();
        assert!(error.downcast_ref::<ToolMissing>().is_some());
    }

    #[test]
    fn a_path_var_with_no_clang_tidy_anywhere_finds_nothing() {
        let dir = tempfile::tempdir().unwrap();
        let path_var = std::env::join_paths([dir.path()]).unwrap();
        assert_eq!(autodetect_clang_tidy(&path_var), None);
    }

    #[test]
    fn a_path_var_with_clang_tidy_finds_it() {
        let dir = tempfile::tempdir().unwrap();
        let exe_name = if cfg!(windows) {
            "clang-tidy.exe"
        } else {
            "clang-tidy"
        };
        let tool = dir.path().join(exe_name);
        std::fs::write(&tool, b"").unwrap();
        let path_var = std::env::join_paths([dir.path()]).unwrap();
        assert_eq!(autodetect_clang_tidy(&path_var), Some(tool));
    }

    #[test]
    fn checks_block_parsing_stops_at_the_first_blank_line() {
        let text = "Checks: >\n  a-*,\n  b-*\n\nExtraArgs:\n  - x\n";
        assert_eq!(parse_checks_block(text), vec!["a-*", "b-*"]);
    }

    #[test]
    fn clang_tidy_checks_is_a_superset_of_blocking_checks() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let text = std::fs::read_to_string(root.join(".clang-tidy")).unwrap();
        let items = parse_checks_block(&text);
        for check in BLOCKING_CHECKS {
            assert!(
                items.iter().any(|item| item == check),
                "{check} is missing from .clang-tidy's Checks: line"
            );
        }
    }

    #[test]
    #[ignore = "requires a real clang-tidy on PATH"]
    fn configured_header_filter_reports_repository_headers() {
        let repo_root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let dir = tempfile::tempdir().unwrap();
        let header_dir = dir.path().join("libs/engine");
        std::fs::create_dir_all(&header_dir).unwrap();
        std::fs::write(
            header_dir.join("probe.h"),
            "inline int value(int unused) { return 1; }\n",
        )
        .unwrap();
        let source = header_dir.join("probe.cpp");
        std::fs::write(
            &source,
            "#include \"probe.h\"\nint main() { return value(1); }\n",
        )
        .unwrap();
        let tool = autodetect_clang_tidy(&std::env::var_os("PATH").unwrap_or_default())
            .expect("clang-tidy on PATH");
        let output = crate::process::command(&tool.to_string_lossy())
            .arg(format!(
                "--config-file={}",
                repo_root.join(".clang-tidy").display()
            ))
            .args([
                "--checks=-*,misc-unused-parameters",
                "--warnings-as-errors=-*",
            ])
            .arg(source)
            .args(["--", "-std=c++20"])
            .output()
            .unwrap();
        assert!(output.status.success());
        let diagnostics = String::from_utf8_lossy(&output.stdout);
        assert!(
            diagnostics.contains("probe.h:1:22: warning:"),
            "{diagnostics}"
        );
        assert!(diagnostics.contains("[misc-unused-parameters]"));
    }

    #[test]
    #[ignore = "requires a real clang-tidy on PATH"]
    fn every_blocking_check_fires_on_its_own_real_canary() {
        let repo_root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let report = run_canaries(&repo_root, None, &[]).unwrap();
        let unfired: Vec<&str> = report.failures.iter().map(|f| f.check.as_str()).collect();
        assert!(report.ok(), "check(s) that did not fire: {unfired:?}");
        assert_eq!(report.checked, BLOCKING_CHECKS.len());
    }
}
