//! Ports check-drift.ps1's four build/Qt drift invariants.
//!
//! Not a Qt linter and not a style checker. Four rules, each one a shape that has
//! already gone wrong here or is one copy-paste away from doing so:
//!
//!   qt-version-consistency  Every three-part Qt version literal in build
//!                           machinery equals the canonical .qt-version.
//!   setup-qt-centralized    install-qt-action is used only by the composite
//!                           action that owns Qt provisioning.
//!   no-qmake-project        No qmake-era .pro/.pri project file appears.
//!   qt-sdk-path-allowlist   An absolute Qt SDK path appears only where it is
//!                           already known and accepted.
//!
//! File discovery is `git ls-files`, never a recursive directory walk: the
//! repository carries gitignored full second checkouts under .claude/worktrees/
//! while parallel agent sessions run, plus build trees, and a recursive scan
//! would read those as source.
//!
//! Rust sources are deliberately NOT scanned. check-drift.ps1 never covered
//! `.rs` files, and doing so here would make this module trip over its own
//! `#[cfg(test)]` fixtures: a Qt version or SDK path embedded in a Rust string
//! literal below is test evidence, not build machinery.

use std::path::Path;

pub struct Violation {
    pub rule: &'static str,
    pub file: String,
    pub line: usize,
    pub message: String,
}

pub struct DriftReport {
    pub violations: Vec<Violation>,
}

/// Absolute Qt SDK paths that already exist and are accepted. This list is the
/// point of the rule: it is not meant to grow silently. A new entry is a
/// decision. Ported verbatim from check-drift.ps1's `$script:QtSdkPathAllowlist`.
const QT_SDK_PATH_ALLOWLIST: &[&str] = &[
    "CMakeLists.txt",
    "scripts/check-quality.ps1",
    "scripts/run-tests.ps1",
];

/// Blanks whole-line comments, keeping line numbering intact. Only whole-line
/// comments: a trailing comment on a real line is left alone, since stripping it
/// would need to know about string literals.
fn strip_comment_lines(text: &str) -> Vec<String> {
    text.lines()
        .map(|line| {
            let trimmed = line.trim_start();
            if trimmed.starts_with('#') || trimmed.starts_with("//") || trimmed.starts_with("<!--")
            {
                String::new()
            } else {
                line.to_string()
            }
        })
        .collect()
}

pub fn check(root: &Path) -> anyhow::Result<DriftReport> {
    // ls-files first, matching check-drift.ps1's own order: a missing/broken git
    // repository is a refusal (Err), never an empty, falsely-clean violation list.
    let files = crate::git::Git::new(root).ls_files()?;

    let mut violations = Vec::new();
    let canonical_path = root.join(".qt-version");
    let Ok(canonical_raw) = std::fs::read_to_string(&canonical_path) else {
        violations.push(Violation {
            rule: "qt-version-consistency",
            file: ".qt-version".into(),
            line: 0,
            message: "the canonical Qt version file is missing".into(),
        });
        return Ok(DriftReport { violations });
    };
    let canonical = canonical_raw.trim();
    let version_re = regex::Regex::new(r"^\d+\.\d+\.\d+$").unwrap();
    if !version_re.is_match(canonical) {
        violations.push(Violation {
            rule: "qt-version-consistency",
            file: ".qt-version".into(),
            line: 0,
            message: format!("'{canonical}' is not a three-part version"),
        });
        return Ok(DriftReport { violations });
    }

    let excluded = regex::Regex::new(r"(?i)^scripts/tests/").unwrap();
    // Where a Qt version literal is load-bearing. Documentation and changelogs
    // legitimately name older versions and are not checked.
    let version_scanned = regex::Regex::new(
        r"(?i)(^CMakeLists\.txt$|(^|/)CMakeLists\.txt$|\.cmake$|^CMakePresets\.json$|^scripts/.*\.ps1$|^\.github/.*\.ya?ml$)",
    )
    .unwrap();
    let yaml = regex::Regex::new(r"(?i)\.ya?ml$").unwrap();
    let qmake = regex::Regex::new(r"(?i)\.(pro|pri)$").unwrap();
    let uses_install_qt =
        regex::Regex::new(r"(?i)^\s*(-\s*)?uses:\s*\S*install-qt-action").unwrap();
    let sdk_path = regex::Regex::new(r"(?i)[A-Z]:[\\/]{1,2}Qt[\\/]{1,2}(\d+\.\d+\.\d+)").unwrap();
    let version_input = regex::Regex::new(r"(?i)^\s*version:\s*'?(\d+\.\d+\.\d+)'?\s*$").unwrap();

    for relative in files {
        let path = root.join(&relative);
        if !path.is_file() || excluded.is_match(&relative) {
            continue;
        }
        if qmake.is_match(&relative) {
            violations.push(Violation {
                rule: "no-qmake-project",
                file: relative,
                line: 0,
                message: "qmake-era project file; this project is CMake-only".into(),
            });
            continue;
        }
        let is_yaml = yaml.is_match(&relative);
        if !version_scanned.is_match(&relative) && !is_yaml {
            continue;
        }
        let Ok(text) = std::fs::read_to_string(&path) else {
            continue;
        };
        for (idx, line) in strip_comment_lines(&text).iter().enumerate() {
            if line.is_empty() {
                continue;
            }
            let number = idx + 1;

            // Structured match on a `uses:` step, not on the bare name: the
            // composite action's own prose mentions install-qt-action, and so
            // does its own file.
            if uses_install_qt.is_match(line) && relative != ".github/actions/setup-qt/action.yml" {
                violations.push(Violation {
                    rule: "setup-qt-centralized",
                    file: relative.clone(),
                    line: number,
                    message:
                        "install-qt-action is used directly; use ./.github/actions/setup-qt instead"
                            .into(),
                });
            }

            if sdk_path.is_match(line) && !QT_SDK_PATH_ALLOWLIST.contains(&relative.as_str()) {
                violations.push(Violation {
                    rule: "qt-sdk-path-allowlist",
                    file: relative.clone(),
                    line: number,
                    message: "absolute Qt SDK path outside the accepted locations".into(),
                });
            }

            // Two load-bearing shapes only: an absolute Qt SDK path (drive
            // letter through three-part version) and a workflow `version:`
            // input pinned to a three-part version. A two-part
            // find_package(Qt6 6.11 REQUIRED ...) states a MINIMUM, not the
            // installed SDK, and is deliberately not matched.
            let mut found: Vec<String> = sdk_path
                .captures_iter(line)
                .map(|c| c[1].to_string())
                .collect();
            if is_yaml && let Some(c) = version_input.captures(line) {
                found.push(c[1].to_string());
            }
            found.sort();
            found.dedup();
            for version in found {
                if version != canonical {
                    violations.push(Violation {
                        rule: "qt-version-consistency",
                        file: relative.clone(),
                        line: number,
                        message: format!(
                            "Qt {version} does not match the canonical {canonical} from .qt-version"
                        ),
                    });
                }
            }
        }
    }
    Ok(DriftReport { violations })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;
    use std::fs;
    use std::process::Command;

    /// A clean base repository shape (check-drift.ps1's own `New-FixtureRepo`):
    /// a composite Qt setup action, a workflow that uses it, and a CMakeLists
    /// that pins the same version, all internally consistent. `remove` drops
    /// base entries (e.g. to test a missing canonical version); `overrides` adds
    /// to or replaces them.
    fn fixture(remove: &[&str], overrides: &[(&str, &str)]) -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        Command::new("git")
            .args(["init", "-q"])
            .current_dir(dir.path())
            .status()
            .unwrap();

        let mut base: BTreeMap<&str, String> = BTreeMap::new();
        base.insert(".qt-version", "6.11.1\n".into());
        base.insert(
            ".github/actions/setup-qt/action.yml",
            concat!(
                "name: Set up Qt 6\n",
                "description: Owns Qt provisioning.\n",
                "runs:\n",
                "  using: composite\n",
                "  steps:\n",
                "    - name: Install Qt\n",
                "      uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1\n",
                "      with:\n",
                "        version: ${{ steps.resolve.outputs.version }}\n",
            )
            .into(),
        );
        base.insert(
            ".github/workflows/ci.yml",
            concat!(
                "jobs:\n",
                "  build:\n",
                "    steps:\n",
                "      - name: Install Qt 6\n",
                "        uses: ./.github/actions/setup-qt\n",
                "        with:\n",
                "          profile: quick\n",
            )
            .into(),
        );
        base.insert(
            "CMakeLists.txt",
            concat!(
                "cmake_minimum_required(VERSION 3.25)\n",
                "list(APPEND CMAKE_PREFIX_PATH \"C:/Qt/6.11.1/msvc2022_64\")\n",
                "find_package(Qt6 6.11 REQUIRED COMPONENTS Core Quick)\n",
            )
            .into(),
        );

        for key in remove {
            base.remove(*key);
        }
        for (path, content) in overrides {
            base.insert(path, (*content).to_string());
        }

        for (path, content) in &base {
            let full = dir.path().join(path);
            fs::create_dir_all(full.parent().unwrap()).unwrap();
            fs::write(&full, content).unwrap();
        }
        Command::new("git")
            .args(["add", "-A"])
            .current_dir(dir.path())
            .status()
            .unwrap();
        dir
    }

    #[test]
    fn the_clean_fixture_shape_passes() {
        let dir = fixture(&[], &[]);
        let report = check(dir.path()).unwrap();
        assert!(report.violations.is_empty());
    }

    /// This is the case that must never regress again: an earlier version of
    /// this port scanned its own Rust test fixtures as build machinery and
    /// failed against the real tree it was meant to guard.
    #[test]
    fn the_real_repository_passes_with_zero_violations() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let report = check(&root).unwrap();
        let messages: Vec<String> = report
            .violations
            .iter()
            .map(|v| format!("[{}] {}:{}: {}", v.rule, v.file, v.line, v.message))
            .collect();
        assert!(
            report.violations.is_empty(),
            "this repository must satisfy its own guard:\n{}",
            messages.join("\n")
        );
    }

    #[test]
    fn qt_version_literal_disagreeing_with_canonical_is_a_violation() {
        let dir = fixture(
            &[],
            &[("CMakeLists.txt", "set(QT_ROOT C:/Qt/6.10.0/msvc2022_64)\n")],
        );
        let report = check(dir.path()).unwrap();
        assert_eq!(report.violations.len(), 1);
        assert_eq!(report.violations[0].rule, "qt-version-consistency");
    }

    #[test]
    fn a_workflow_pinning_a_different_qt_version_is_rejected() {
        let dir = fixture(
            &[],
            &[(
                ".github/workflows/old.yml",
                "jobs:\n  build:\n    steps:\n      - uses: ./.github/actions/setup-qt\n        with:\n          version: '6.9.2'\n",
            )],
        );
        let report = check(dir.path()).unwrap();
        assert!(
            report
                .violations
                .iter()
                .any(|v| v.rule == "qt-version-consistency" && v.message.contains("6.9.2"))
        );
    }

    #[test]
    fn a_two_part_find_package_minimum_is_not_an_sdk_version() {
        let dir = fixture(
            &[],
            &[(
                "app/CMakeLists.txt",
                "find_package(Qt6 6.11 REQUIRED COMPONENTS Quick QuickTest)\n",
            )],
        );
        let report = check(dir.path()).unwrap();
        assert!(report.violations.is_empty());
    }

    #[test]
    fn documentation_naming_an_older_qt_is_not_a_violation() {
        let dir = fixture(
            &[],
            &[
                (
                    "docs/history.md",
                    "Until August 2026 the project built against Qt 6.9.2 in C:/Qt/6.9.2/msvc2022_64.\n",
                ),
                ("CHANGELOG.md", "- Moved from Qt 6.9.2 to Qt 6.11.1.\n"),
            ],
        );
        let report = check(dir.path()).unwrap();
        assert!(report.violations.is_empty());
    }

    #[test]
    fn bumping_qt_version_alone_leaves_stale_literals_and_is_rejected() {
        let dir = fixture(&[], &[(".qt-version", "6.12.0\n")]);
        let report = check(dir.path()).unwrap();
        assert!(report.violations.iter().any(|v| {
            v.rule == "qt-version-consistency"
                && v.message
                    .contains("6.11.1 does not match the canonical 6.12.0")
        }));
    }

    #[test]
    fn qmake_project_file_is_rejected() {
        let dir = fixture(&[], &[("legacy/app.pro", "")]);
        let report = check(dir.path()).unwrap();
        assert!(
            report
                .violations
                .iter()
                .any(|v| v.rule == "no-qmake-project")
        );
    }

    #[test]
    fn a_file_merely_mentioning_qmake_is_not_a_violation() {
        let dir = fixture(
            &[],
            &[(
                "docs/build.md",
                "This project never used qmake or .pro files; it is CMake-only.\n",
            )],
        );
        let report = check(dir.path()).unwrap();
        assert!(report.violations.is_empty());
    }

    #[test]
    fn install_qt_action_outside_the_composite_action_is_rejected() {
        let dir = fixture(
            &[],
            &[(
                ".github/workflows/asan.yml",
                "  - uses: jurplel/install-qt-action@v4\n",
            )],
        );
        let report = check(dir.path()).unwrap();
        assert!(
            report
                .violations
                .iter()
                .any(|v| v.rule == "setup-qt-centralized")
        );
    }

    #[test]
    fn install_qt_action_inside_the_composite_action_is_allowed() {
        let dir = fixture(&[], &[]);
        let report = check(dir.path()).unwrap();
        assert!(
            report
                .violations
                .iter()
                .all(|v| v.rule != "setup-qt-centralized")
        );
    }

    #[test]
    fn prose_naming_install_qt_action_is_not_a_violation() {
        let dir = fixture(
            &[],
            &[(
                ".github/workflows/notes.yml",
                concat!(
                    "# Historical note: this job used jurplel/install-qt-action directly until the\n",
                    "# uses: jurplel/install-qt-action line moved into the composite action.\n",
                    "jobs:\n  noop:\n    steps:\n      - run: echo ok\n",
                ),
            )],
        );
        let report = check(dir.path()).unwrap();
        assert!(report.violations.is_empty());
    }

    #[test]
    fn absolute_qt_sdk_path_outside_the_allowlist_is_rejected() {
        let dir = fixture(
            &[],
            &[(
                "scripts/new-thing.ps1",
                "$root = 'C:/Qt/6.11.1/msvc2022_64'\n",
            )],
        );
        let report = check(dir.path()).unwrap();
        assert!(
            report
                .violations
                .iter()
                .any(|v| v.rule == "qt-sdk-path-allowlist")
        );
    }

    #[test]
    fn the_already_accepted_sdk_paths_stay_accepted() {
        let dir = fixture(
            &[],
            &[
                (
                    "scripts/check-quality.ps1",
                    "$qtBin = 'C:\\Qt\\6.11.1\\msvc2022_64\\bin'\n",
                ),
                (
                    "scripts/run-tests.ps1",
                    "$qtBin = 'C:\\Qt\\6.11.1\\msvc2022_64\\bin'\n",
                ),
            ],
        );
        let report = check(dir.path()).unwrap();
        assert!(
            report
                .violations
                .iter()
                .all(|v| v.rule != "qt-sdk-path-allowlist")
        );
    }

    #[test]
    fn comment_lines_are_stripped_before_matching() {
        let dir = fixture(
            &[],
            &[(
                "CMakeLists.txt",
                "# set(QT_ROOT C:/Qt/6.10.0/msvc2022_64)\n",
            )],
        );
        let report = check(dir.path()).unwrap();
        assert!(report.violations.is_empty());
    }

    #[test]
    fn scripts_tests_directory_is_excluded_entirely() {
        let dir = fixture(&[], &[("scripts/tests/fixture.pro", "")]);
        let report = check(dir.path()).unwrap();
        assert!(report.violations.is_empty());
    }

    #[test]
    fn missing_canonical_version_file_is_the_only_violation() {
        let dir = fixture(&[".qt-version"], &[]);
        let report = check(dir.path()).unwrap();
        assert_eq!(report.violations.len(), 1);
        assert_eq!(report.violations[0].rule, "qt-version-consistency");
        assert_eq!(report.violations[0].file, ".qt-version");
    }

    #[test]
    fn an_untracked_nested_checkout_is_not_scanned() {
        // Agent sessions keep full copies of this repository under
        // .claude/worktrees/. `git ls-files` alone already keeps an untracked
        // nested checkout invisible; this asserts that stays true rather than
        // reintroducing a recursive walk.
        let dir = fixture(&[], &[]);
        let nested = dir
            .path()
            .join(".claude/worktrees/agent-x/.github/workflows");
        fs::create_dir_all(&nested).unwrap();
        fs::write(
            nested.join("ci.yml"),
            "jobs:\n  build:\n    steps:\n      - uses: jurplel/install-qt-action@v4\n        with:\n          version: '6.9.2'\n",
        )
        .unwrap();
        let report = check(dir.path()).unwrap();
        assert!(report.violations.is_empty());
    }

    #[test]
    fn a_directory_that_is_not_a_git_repository_is_a_hard_error_not_zero_violations() {
        // Review Focus: a git failure must surface as a refusal, never as a
        // silently empty (and therefore falsely clean) violation list.
        let dir = tempfile::tempdir().unwrap();
        let result = check(dir.path());
        assert!(result.is_err());
    }
}
