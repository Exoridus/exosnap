//! Turns a changed-file list into what has to be verified.
//!
//! Deliberately coarse and one-directional: every rule may only widen the scope.
//! Where a dependency cannot be resolved honestly (a header's dependents, an
//! unrecognised file type) the answer is "build and test everything". A scoped run
//! may check more than it strictly had to; it may never be falsely safe.

use std::collections::BTreeSet;
use std::sync::LazyLock;

use regex::Regex;

pub const QUICK_TEST_FILTER: &str = r"^quick\.";

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Scope {
    pub changed_files: Vec<String>,
    pub categories: Vec<String>,
    pub requires_configure: bool,
    pub requires_build: bool,
    pub requires_qmllint: bool,
    pub requires_tests: bool,
    pub requires_full_tests: bool,
    pub test_filter: String,
    pub requires_script_tests: bool,
    pub requires_static_analysis: bool,
    pub requires_rust: bool,
    pub escalation_reasons: Vec<String>,
}

impl Scope {
    /// The scope of a run that is not narrowed by a change set.
    pub fn everything() -> Scope {
        let mut scope = Scope::of(&[]);
        scope.escalation_reasons.clear();
        scope
    }

    pub fn of(changed_files: &[String]) -> Scope {
        let mut scope = Scope {
            changed_files: changed_files.to_vec(),
            ..Scope::default()
        };

        if changed_files.is_empty() {
            // No detectable change set is not "nothing changed": a shallow clone,
            // a detached HEAD or a missing base all land here.
            scope.categories = vec!["unknown".into()];
            scope.requires_configure = true;
            scope.requires_build = true;
            scope.requires_qmllint = true;
            scope.requires_tests = true;
            scope.requires_full_tests = true;
            scope.requires_script_tests = true;
            scope.requires_static_analysis = true;
            scope.requires_rust = true;
            scope.escalation_reasons =
                vec!["no change set could be determined; verifying everything".into()];
            return scope;
        }

        let mut categories = BTreeSet::new();
        let mut test_filters = BTreeSet::new();
        // Aggregated rather than listed per file: a branch-wide change set has
        // hundreds of headers, and 200 identical sentences bury the one line that
        // is informative.
        let mut reasons: Vec<(&'static str, usize, String)> = Vec::new();
        let mut add_reason = |kind: &'static str, example: &str| match reasons
            .iter_mut()
            .find(|(k, _, _)| *k == kind)
        {
            Some(entry) => entry.1 += 1,
            None => reasons.push((kind, 1, example.to_string())),
        };

        for file in changed_files {
            let path = file.replace('\\', "/");
            let class = classify(&path);
            categories.insert(class.category());
            match class {
                Class::Rust => scope.requires_rust = true,
                Class::Cmake => {
                    scope.requires_configure = true;
                    scope.requires_build = true;
                    scope.requires_qmllint = true;
                    scope.requires_tests = true;
                    scope.requires_full_tests = true;
                    scope.requires_static_analysis = true;
                    add_reason(
                        "build infrastructure changed; configure, build and the full test suite are in scope",
                        &path,
                    );
                }
                Class::Header => {
                    scope.requires_configure = true;
                    scope.requires_build = true;
                    scope.requires_tests = true;
                    scope.requires_full_tests = true;
                    scope.requires_static_analysis = true;
                    add_reason(
                        "a header changed; its dependents are not resolved here, so the whole build and test suite is in scope",
                        &path,
                    );
                }
                Class::Cpp => {
                    scope.requires_configure = true;
                    scope.requires_build = true;
                    scope.requires_tests = true;
                    scope.requires_static_analysis = true;
                    // No narrowing by source root. app/ holds dozens of gtest
                    // binaries under their own prefixes, so narrowing app/ to the
                    // Quick tests skipped the tests of the file that changed.
                    scope.requires_full_tests = true;
                }
                Class::Qml => {
                    scope.requires_configure = true;
                    scope.requires_qmllint = true;
                    // QML is compiled into the module and staged as a resource,
                    // so a QML edit that is not rebuilt is not under test.
                    scope.requires_build = true;
                    scope.requires_tests = true;
                    test_filters.insert(QUICK_TEST_FILTER);
                }
                Class::Scripts => scope.requires_script_tests = true,
                Class::Workflow | Class::Docs | Class::Data => {}
                Class::Other => {
                    scope.requires_configure = true;
                    scope.requires_build = true;
                    scope.requires_tests = true;
                    scope.requires_full_tests = true;
                    scope.requires_static_analysis = true;
                    add_reason(
                        "unrecognised file type; escalating rather than guessing",
                        &path,
                    );
                }
            }
        }

        if scope.requires_full_tests {
            scope.test_filter.clear();
        } else if test_filters.len() == 1 {
            scope.test_filter = test_filters.first().unwrap().to_string();
        } else if test_filters.len() > 1 {
            // Two disjoint narrow filters are not worth a composed regex that has
            // to stay correct; run everything instead.
            scope.requires_full_tests = true;
            scope.test_filter.clear();
            add_reason(
                "changes span more than one test area; running the full suite",
                "",
            );
        }

        scope.categories = categories.into_iter().map(str::to_string).collect();
        scope.escalation_reasons = reasons
            .into_iter()
            .map(|(kind, count, example)| {
                if example.is_empty() {
                    kind.to_string()
                } else {
                    format!("{kind} (x{count}, e.g. {example})")
                }
            })
            .collect();
        scope
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Class {
    Rust,
    Cmake,
    Header,
    Cpp,
    Qml,
    Scripts,
    Workflow,
    Docs,
    Data,
    Other,
}

impl Class {
    fn category(self) -> &'static str {
        match self {
            Class::Rust => "rust",
            Class::Cmake => "cmake",
            Class::Header => "header",
            Class::Cpp => "cpp",
            Class::Qml => "qml",
            Class::Scripts => "scripts",
            Class::Workflow => "workflow",
            Class::Docs => "docs",
            Class::Data => "data",
            Class::Other => "other",
        }
    }
}

fn classify(path: &str) -> Class {
    static RULES: LazyLock<Vec<(Regex, Class)>> = LazyLock::new(|| {
        [
            (
                r"(?i)^tools/(Cargo\.(toml|lock)|(exo-verify|exo-dev)/(Cargo\.toml|.+\.(rs|toml|json|md)))$|(?i)^\.cargo/config\.toml$",
                Class::Rust,
            ),
            (
                r"(?i)(^|/)CMakeLists\.txt$|(?i)^CMakePresets\.json$|(?i)\.cmake$|(?i)^cmake/|(?i)\.in$",
                Class::Cmake,
            ),
            (r"(?i)\.(h|hpp|hxx|inl)$", Class::Header),
            (r"(?i)\.(cpp|cc|cxx)$", Class::Cpp),
            (r"(?i)\.(qml|mjs)$", Class::Qml),
            (r"(?i)^scripts/", Class::Scripts),
            (
                r"(?i)^\.github/|(?i)^\.githooks/|(?i)^\.gitattributes$|(?i)^\.gitignore$|(?i)^\.qt-version$",
                Class::Workflow,
            ),
            (
                r"(?i)^docs/|(?i)^\.workspace/|(?i)^(README|AGENTS|CLAUDE|PRIVACY|CHANGELOG|LICENSE)",
                Class::Docs,
            ),
            (r"(?i)\.(md|txt|png|svg|ico|json|toml|ya?ml|rc)$", Class::Data),
        ]
        .into_iter()
        .map(|(pattern, class)| (Regex::new(pattern).expect("scope rule"), class))
        .collect()
    });
    RULES
        .iter()
        .find(|(rule, _)| rule.is_match(path))
        .map_or(Class::Other, |(_, class)| *class)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scope(files: &[&str]) -> Scope {
        Scope::of(&files.iter().map(|f| f.to_string()).collect::<Vec<_>>())
    }

    #[test]
    fn a_cpp_change_requires_a_build_and_the_whole_suite() {
        let s = scope(&["libs/engine/src/muxer.cpp"]);
        assert!(s.requires_build && s.requires_tests && s.requires_full_tests);
        assert_eq!(s.test_filter, "");
    }

    #[test]
    fn a_cpp_test_under_app_runs_its_own_tests_not_only_the_quick_ones() {
        let s = scope(&["app/tests/test_whats_new_payload.cpp"]);
        assert!(s.requires_full_tests);
        assert!(!s.test_filter.contains("quick"));
    }

    #[test]
    fn a_header_change_escalates_to_the_full_suite_and_says_so() {
        let s = scope(&["libs/engine/include/exosnap/engine/session.h"]);
        assert!(s.requires_full_tests);
        assert_eq!(s.test_filter, "");
        assert!(!s.escalation_reasons.is_empty());
    }

    #[test]
    fn a_cmake_change_escalates() {
        for file in [
            "CMakeLists.txt",
            "app/CMakeLists.txt",
            "CMakePresets.json",
            "cmake/tests/x.cmake",
        ] {
            let s = scope(&[file]);
            assert!(s.requires_configure, "{file} must force a configure");
            assert!(s.requires_full_tests, "{file} must force the full suite");
            assert!(s.requires_qmllint, "{file} must force qmllint");
        }
    }

    #[test]
    fn a_qml_change_maps_to_qmllint_a_rebuild_and_the_quick_tests() {
        let s = scope(&["app/quick/ExoSnap/Quick/RecordPage.qml"]);
        assert!(s.requires_qmllint);
        assert!(s.requires_build, "QML is compiled into the module");
        assert_eq!(s.test_filter, QUICK_TEST_FILTER);
    }

    #[test]
    fn workflow_hook_and_docs_changes_compile_nothing() {
        for files in [
            &[
                ".github/workflows/ci.yml",
                ".github/actions/setup-qt/action.yml",
            ][..],
            &[".githooks/pre-commit", ".githooks/pre-push"][..],
            &["docs/product-spec.md", "README.md", ".workspace/notes.md"][..],
        ] {
            let s = scope(files);
            assert!(!s.requires_build, "{files:?} must not build");
            assert!(!s.requires_tests, "{files:?} must not test");
            assert!(!s.requires_configure, "{files:?} must not configure");
        }
    }

    #[test]
    fn a_script_change_runs_the_script_tests_and_nothing_heavier() {
        let s = scope(&["scripts/run-tests.ps1"]);
        assert!(s.requires_script_tests);
        assert!(!s.requires_build);
    }

    #[test]
    fn a_rust_tooling_change_runs_the_rust_gate_without_building_the_product() {
        let s = scope(&[
            "tools/exo-verify/src/scenarios/audio.rs",
            "tools/exo-dev/src/plan.rs",
            "tools/exo-dev/Cargo.toml",
            "tools/Cargo.toml",
            "tools/Cargo.lock",
            ".cargo/config.toml",
        ]);
        assert!(s.requires_rust);
        assert!(!s.requires_build && !s.requires_configure && !s.requires_full_tests);
        assert_eq!(s.categories, vec!["rust".to_string()]);
    }

    #[test]
    fn an_undetermined_change_set_verifies_everything_including_rust() {
        let s = scope(&[]);
        assert!(s.requires_build && s.requires_full_tests && s.requires_rust);
    }

    #[test]
    fn unrecognised_files_escalate_rather_than_being_ignored() {
        for file in [
            "libs/engine/src/mystery.zzz",
            "tools/exo-verify/mystery.zzz",
        ] {
            let s = scope(&[file]);
            assert!(s.requires_full_tests, "{file} must widen, never narrow");
            assert!(s.escalation_reasons.join(" ").contains("unrecognised"));
        }
    }

    #[test]
    fn mixed_quick_and_engine_changes_widen_to_the_full_suite() {
        let s = scope(&["libs/engine/src/muxer.cpp", "app/quick/ExoSnap/Quick/x.qml"]);
        assert!(s.requires_full_tests);
        assert_eq!(s.test_filter, "");
    }

    #[test]
    fn escalation_reasons_are_aggregated_per_kind() {
        let s = scope(&["a/x.h", "a/y.h", "a/z.h"]);
        assert_eq!(s.escalation_reasons.len(), 1);
        assert!(s.escalation_reasons[0].contains("(x3, e.g. a/x.h)"));
    }
}
