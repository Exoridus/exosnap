//! Reading build and test output into evidence a reader can act on.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::LazyLock;

use regex::Regex;
use serde_json::Value;
use sha2::{Digest, Sha256};

use crate::process::read_lines;

/// The CTest label that marks a Qt QuickTest runner, i.e. one that understands
/// `-o <file>,txt`. Declared at the registration site; read here.
pub const QUICK_TEST_LABEL: &str = "quicktest";

/// One line of a build log that explains a failure: compiler, linker, MSBuild,
/// GNU-style, CMake and ninja. Anchored on the diagnostic token, so a progress
/// line that merely mentions a file called error.cpp does not match.
static BUILD_ERROR: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
        r"(?i)(\berror [CD]\d{4}\b|\berror LNK\d{3,4}\b|\berror MSB\d{4}\b|\bfatal error\b|:\s*error:|^CMake Error|^ninja: error)",
    )
    .unwrap()
});

/// The first error line of a build log. A parallel build keeps compiling after
/// the first failure, so the tail is cascade; the cause scrolled past earlier.
pub fn first_build_error(log: &Path) -> Option<String> {
    read_lines(log)
        .into_iter()
        .find(|line| BUILD_ERROR.is_match(line))
        .map(|line| line.trim().to_string())
}

/// The CTest names that failed, from a run-tests.ps1 summary or a raw ctest log.
/// run-tests.ps1 keeps the full ctest output in a separate file named on a
/// "Full log:" line; only that file carries the failure block, so the pointer is
/// followed.
pub fn failed_ctest_names(log: &Path) -> Vec<String> {
    static POINTER: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"^\s*Full log:\s*(.+?)\s*$").unwrap());
    static FAILED: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r"^\s*\d+\s+-\s+(\S+)\s+\((Failed|Timeout|Subprocess aborted)").unwrap()
    });
    let mut lines = read_lines(log);
    if let Some(full) = lines
        .iter()
        .find_map(|line| POINTER.captures(line).map(|c| PathBuf::from(&c[1])))
        && full != log
    {
        lines.extend(read_lines(&full));
    }
    let names: BTreeSet<String> = lines
        .iter()
        .filter_map(|line| FAILED.captures(line).map(|c| c[1].to_string()))
        .collect();
    names.into_iter().collect()
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Registration {
    pub file_path: String,
    pub labels: Vec<String>,
}

/// Test name to executable and labels, from `ctest --show-only=json-v1` output.
pub fn parse_registration(json: &str) -> BTreeMap<String, Registration> {
    let mut registration = BTreeMap::new();
    let Ok(document) = serde_json::from_str::<Value>(json) else {
        return registration;
    };
    for test in document["tests"].as_array().into_iter().flatten() {
        let (Some(name), Some(command)) = (test["name"].as_str(), test["command"].as_array())
        else {
            continue;
        };
        let Some(file_path) = command.first().and_then(Value::as_str) else {
            continue;
        };
        let labels = test["properties"]
            .as_array()
            .into_iter()
            .flatten()
            .find(|p| p["name"] == "LABELS")
            .and_then(|p| p["value"].as_array())
            .map(|values| {
                values
                    .iter()
                    .filter_map(Value::as_str)
                    .map(str::to_string)
                    .collect()
            })
            .unwrap_or_default();
        registration.insert(
            name.to_string(),
            Registration {
                file_path: file_path.to_string(),
                labels,
            },
        );
    }
    registration
}

/// What CTest registered in a configured tree. Empty when the tree is not
/// configured or ctest is unavailable: the worst outcome stays "no diagnosis",
/// never "ran the wrong binary".
pub fn ctest_registration(
    build_dir: &Path,
    config: &str,
    env: &[(std::ffi::OsString, std::ffi::OsString)],
) -> BTreeMap<String, Registration> {
    if !build_dir.join("CTestTestfile.cmake").is_file() {
        return BTreeMap::new();
    }
    let mut command = crate::process::command("ctest");
    command
        .arg("--test-dir")
        .arg(build_dir)
        .args(["-C", config, "--show-only=json-v1"]);
    for (key, value) in env {
        command.env(key, value);
    }
    match crate::process::query(command) {
        Ok((_, stdout)) => parse_registration(&stdout),
        Err(_) => BTreeMap::new(),
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiagnosticCommand {
    pub test_name: String,
    pub executable: String,
    pub file_path: String,
    pub arguments: Vec<String>,
    pub output_path: PathBuf,
}

/// How to make failing QuickTest runners write a per-function text report.
///
/// Only runners labelled `quicktest` take `-o`; the `exosnap --*-test` entries
/// registered beside them would reject it. The binary is the one CTest
/// registered, never a composed path: generators place runners differently, and a
/// composed path that misses makes the diagnosis silently absent.
pub fn qml_diagnostic_commands(
    failed: &[String],
    registration: &BTreeMap<String, Registration>,
    log_dir: &Path,
) -> Vec<DiagnosticCommand> {
    let names: BTreeSet<&String> = failed.iter().collect();
    names
        .into_iter()
        .filter_map(|name| {
            let entry = registration.get(name)?;
            if !entry.labels.iter().any(|l| l == QUICK_TEST_LABEL) {
                return None;
            }
            let executable = Path::new(&entry.file_path.replace('\\', "/"))
                .file_stem()?
                .to_string_lossy()
                .into_owned();
            let output_path = log_dir.join(format!("{executable}.txt"));
            Some(DiagnosticCommand {
                test_name: name.clone(),
                arguments: vec!["-o".into(), format!("{},txt", output_path.display())],
                executable,
                file_path: entry.file_path.clone(),
                output_path,
            })
        })
        .collect()
}

/// The script suites CTest already runs, read from the file that registers them.
/// They run from CTest only whenever the test step runs unfiltered: running the
/// orchestrator's own contracts from inside it would let a broken one pass
/// itself. An unreadable registration site yields nothing, which runs every suite.
pub fn ctest_script_suites(repo_root: &Path) -> Vec<String> {
    static SUITE: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"scripts/tests/([A-Za-z0-9._-]+\.tests\.ps1)").unwrap());
    let Ok(text) = std::fs::read_to_string(repo_root.join("app/CMakeLists.txt")) else {
        return Vec::new();
    };
    let names: BTreeSet<String> = SUITE
        .captures_iter(&text)
        .map(|c| c[1].to_string())
        .collect();
    names.into_iter().collect()
}

/// Where one analysis tool keeps reusable results on this machine: outside the
/// repository and every build tree, which a fresh configure, a `git clean` or a
/// new worktree wipe exactly when replaying results would pay most. The leaf is
/// derived from the toolchain fingerprint, so a different toolchain addresses a
/// different directory. Same derivation as the PowerShell callers, so both share
/// entries.
pub fn tool_cache_dir(tool: &str, fingerprint: &[String], root: Option<&Path>) -> PathBuf {
    let root =
        root.map(Path::to_path_buf)
            .unwrap_or_else(|| match std::env::var_os("LOCALAPPDATA") {
                Some(local) => Path::new(&local).join("ExoSnap/tool-cache"),
                None => std::env::temp_dir().join("exosnap-tool-cache"),
            });
    let mut parts: Vec<&str> = fingerprint
        .iter()
        .map(String::as_str)
        .filter(|p| !p.is_empty())
        .collect();
    if parts.is_empty() {
        parts.push("unqualified-toolchain");
    }
    let digest = Sha256::digest(parts.join("\n").as_bytes());
    root.join(tool).join(&hex::encode(digest)[..16])
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write(dir: &Path, name: &str, lines: &[&str]) -> PathBuf {
        let path = dir.join(name);
        std::fs::write(&path, lines.join("\n")).unwrap();
        path
    }

    fn registration() -> BTreeMap<String, Registration> {
        let entry = |path: &str, labels: &[&str]| Registration {
            file_path: path.into(),
            labels: labels.iter().map(|l| l.to_string()).collect(),
        };
        BTreeMap::from([
            (
                "quick.qml.record_controls".into(),
                entry(
                    "C:/build/Debug/record_controls_qml_tests.exe",
                    &["quick", "quicktest"],
                ),
            ),
            (
                "quick.qml.edit_timeline".into(),
                entry(
                    "C:/build/edit_timeline_qml_tests.exe",
                    &["quick", "quicktest"],
                ),
            ),
            (
                "quick.qml.record_source_picker".into(),
                entry(
                    "C:/build/record_source_picker_qml_tests.exe",
                    &["quick", "quicktest"],
                ),
            ),
            (
                "quick.qml.about_smoke".into(),
                entry("C:/build/exosnap.exe", &["quick"]),
            ),
            (
                "engine.muxer".into(),
                entry("C:/build/engine_tests.exe", &["engine"]),
            ),
        ])
    }

    #[test]
    fn the_first_compiler_error_is_found_above_the_cascade() {
        let dir = tempfile::tempdir().unwrap();
        let log = write(
            dir.path(),
            "b.log",
            &[
                "[12/300] Building CXX object libs/engine/error_reporting.cpp.obj",
                "C:\\src\\libs\\engine\\src\\muxer.cpp(41): error C2065: 'frame': undeclared identifier",
                "C:\\src\\libs\\engine\\src\\muxer.cpp(42): error C2228: left of '.pts'",
                "ninja: build stopped: subcommand failed.",
            ],
        );
        let first = first_build_error(&log).unwrap();
        assert!(first.contains("C2065") && !first.contains("error_reporting"));
    }

    #[test]
    fn linker_cmake_and_ninja_errors_are_found_and_a_clean_log_has_none() {
        let dir = tempfile::tempdir().unwrap();
        for line in [
            "muxer.obj : error LNK2019: unresolved external symbol",
            "CMake Error at CMakeLists.txt:12 (find_package):",
            "ninja: error: loading 'build.ninja': No such file or directory",
            "C:/src/tools/x.c:12:3: error: expected ';'",
        ] {
            let log = write(dir.path(), "b.log", &["[1/2] progress", line]);
            assert_eq!(first_build_error(&log).as_deref(), Some(line));
        }
        let log = write(dir.path(), "b.log", &["[1/2] progress", "[2/2] Linking"]);
        assert_eq!(first_build_error(&log), None);
    }

    #[test]
    fn failing_test_names_are_read_through_the_summary_pointer() {
        let dir = tempfile::tempdir().unwrap();
        let full = write(
            dir.path(),
            "last-run.log",
            &[
                "The following tests FAILED:",
                "\t219 - quick.qml.record_controls (Failed)                quick",
            ],
        );
        let summary = write(
            dir.path(),
            "tests.log",
            &[
                "ctest --test-dir x -C Debug",
                &format!("Full log: {}", full.display()),
                "88% tests passed, 1 tests failed out of 8",
            ],
        );
        assert_eq!(
            failed_ctest_names(&summary),
            vec!["quick.qml.record_controls"]
        );
        let raw = write(
            dir.path(),
            "raw.log",
            &[
                "The following tests FAILED:",
                "  12 - engine.muxer (Timeout)",
            ],
        );
        assert_eq!(failed_ctest_names(&raw), vec!["engine.muxer"]);
    }

    #[test]
    fn the_registration_reads_labels_and_the_registered_command() {
        let json = r#"{"tests":[
            {"name":"quick.qml.edit_timeline","command":["C:/b/edit_timeline_qml_tests.exe"],
             "properties":[{"name":"LABELS","value":["quick","quicktest"]},{"name":"TIMEOUT","value":120}]},
            {"name":"quick.qml.about_smoke","command":["C:/b/exosnap.exe","--smoke-test"],
             "properties":[{"name":"LABELS","value":["quick"]}]},
            {"name":"pipeline.no_props","command":["C:/b/x.exe"]}
        ]}"#;
        let r = parse_registration(json);
        assert_eq!(r.len(), 3);
        assert!(
            r["quick.qml.edit_timeline"]
                .labels
                .contains(&"quicktest".to_string())
        );
        assert!(
            !r["quick.qml.about_smoke"]
                .labels
                .contains(&"quicktest".to_string())
        );
        assert!(r["pipeline.no_props"].labels.is_empty());
        assert_eq!(
            r["quick.qml.edit_timeline"].file_path,
            "C:/b/edit_timeline_qml_tests.exe"
        );
        assert!(parse_registration("not json").is_empty());
    }

    #[test]
    fn an_unconfigured_tree_registers_nothing() {
        let dir = tempfile::tempdir().unwrap();
        assert!(ctest_registration(dir.path(), "Debug", &[]).is_empty());
    }

    #[test]
    fn quicktest_runners_are_rerun_with_a_text_report_and_nothing_else_is() {
        let log_dir = Path::new("C:/logs");
        let commands = qml_diagnostic_commands(
            &["quick.qml.record_controls".into()],
            &registration(),
            log_dir,
        );
        assert_eq!(commands.len(), 1);
        assert_eq!(commands[0].executable, "record_controls_qml_tests");
        assert_eq!(
            commands[0].file_path,
            "C:/build/Debug/record_controls_qml_tests.exe"
        );
        assert_eq!(commands[0].arguments[0], "-o");
        assert!(commands[0].arguments[1].ends_with(",txt"));

        let both = qml_diagnostic_commands(
            &[
                "quick.qml.record_source_picker".into(),
                "quick.qml.edit_timeline".into(),
            ],
            &registration(),
            log_dir,
        );
        assert_eq!(both.len(), 2);

        let none = qml_diagnostic_commands(
            &["quick.qml.about_smoke".into(), "engine.muxer".into()],
            &registration(),
            log_dir,
        );
        assert!(none.is_empty(), "only QuickTest runners take -o");
        assert!(
            qml_diagnostic_commands(
                &["quick.qml.record_controls".into()],
                &BTreeMap::new(),
                log_dir
            )
            .is_empty()
        );
    }

    #[test]
    fn the_ctest_script_suites_are_read_from_their_registration_site() {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let suites = ctest_script_suites(&repo);
        assert!(!suites.is_empty());
        for name in &suites {
            assert!(
                repo.join("scripts/tests").join(name).is_file(),
                "{name} is registered but missing"
            );
        }
        let empty = tempfile::tempdir().unwrap();
        assert!(ctest_script_suites(empty.path()).is_empty());
    }

    #[test]
    fn the_tool_cache_is_derived_per_toolchain_and_outside_the_repository() {
        let repo =
            std::fs::canonicalize(Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")).unwrap();
        let dir = tool_cache_dir("clang-tidy", &["14.44".into(), "x64".into()], None);
        assert!(!dir.starts_with(&repo));
        assert!(
            dir.to_string_lossy()
                .replace('\\', "/")
                .contains("/clang-tidy/")
        );

        let root = Path::new("R:/cache");
        let one = tool_cache_dir("clang-tidy", &["14.44".into()], Some(root));
        assert_eq!(
            one,
            tool_cache_dir("clang-tidy", &["14.44".into()], Some(root))
        );
        assert_ne!(
            one,
            tool_cache_dir("clang-tidy", &["14.45".into()], Some(root))
        );
        assert_ne!(one, tool_cache_dir("clang-tidy", &[], Some(root)));
    }
}
