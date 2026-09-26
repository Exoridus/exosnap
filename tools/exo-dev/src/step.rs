//! The closed set of verification steps.
//!
//! Declaration order is execution order, cheapest first, so a whitespace error
//! never waits behind a compile. `configure` precedes `qmllint` because qmllint is
//! the CMake target `all_qmllint`: a bare qmllint call on this repository lacks the
//! generated import paths and reports resolution failures the target does not.

/// One verification step.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum StepId {
    /// HEAD resolves and the files every later step reads exist.
    Sanity,
    /// Whitespace and conflict-marker damage in the change.
    Diff,
    /// The Qt version, Qt setup and SDK paths agree across their declarations.
    Drift,
    /// Development provenance in source comments. Scoped to the lines a change
    /// adds: the whole-tree backlog is older comments, reported on pushes and
    /// dispatches but never a gate, because a release is not where it gets paid
    /// down.
    SourceHygiene,
    DocsSuperpowersRemoved,
    /// Commit subjects locally; the pull request title in CI.
    CommitPolicy,
    /// A blocking clang-tidy check that stopped firing reports zero findings,
    /// exactly like a clean tree. The canaries prove each one still fires.
    LintCanaries,
    /// Advisory: the tree predates the rule.
    ProseLines,
    Format,
    /// The product version is declared once and repeated by hand across the
    /// Chocolatey, WinGet and Scoop packaging. A bump that missed one published a
    /// package pointing at a release that does not exist. Version axis only: no
    /// installer hash or moderation bar can be true between a bump and its release.
    PackagingVersion,
    /// Package.wxs stays metadata-only and harvests the staging tree, so a
    /// hand-maintained file list cannot silently omit DLLs again.
    MsiHarvest,
    /// The crash-report tag allowlist stays identical to what PRIVACY.md and the
    /// product specification document as sent.
    PrivacyAllowlist,
    /// A network primitive or disallowed host literal outside the known GitHub and
    /// Sentry call sites fails, so a new egress point cannot land unnoticed.
    NetworkEgress,
    /// Pinned by version and digest; a linter fetched as "latest" changes what it
    /// flags between two runs of the same workflow.
    Actionlint,
    /// Offline and high severity only. The audits that need the GitHub API change
    /// answer between runs; the known high findings carry an inline ignore at the
    /// step that is the decision, so a new one fails. Medium findings are backlog.
    Zizmor,
    ScriptTests,
    Rust,
    Configure,
    /// `all_qmllint` over every QML module rather than per-module targets, so a
    /// new module cannot slip past by not being named.
    QmlLint,
    Build,
    /// Through run-tests.ps1, which isolates configuration and Qt and writes the
    /// receipt. CI excludes the `live` label: those tests read real machine state
    /// and are excluded rather than trusted to skip themselves on a GPU-less
    /// runner. `live` and the execution phases are separate axes; selecting by
    /// phase alone would lose the offscreen desktop tests or keep the live ones.
    Tests,
    CppCheck,
    ClangTidy,
    PackagingSmoke,
    /// The A/V-sync analyzer against the committed golden clapper clip. The
    /// 25 ms budget is loose on purpose: a 4 s clip's residual drift is bounded by
    /// the 60 fps frame grid and FFmpeg version differences, not by a real A/V
    /// budget. Five markers, because the analyzer refuses a verdict from a
    /// two-marker reference and such a fixture would only exercise the refusal.
    AvSyncGolden,
}

/// Who implements a step today.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Implementation {
    /// exo-dev itself, possibly driving an external tool that stays a process.
    Native,
    /// A PowerShell or Python script run unchanged: its exit code is the verdict
    /// and its output is passed through. `owner` is the Rust module that replaces
    /// it; a legacy step is a migration state, never a new extension point.
    Legacy { owner: &'static str },
}

/// Which host locks a step holds while it runs.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Locks {
    /// The build-tree lock: the step reads or rewrites the build directory.
    pub tree: bool,
    /// The host build lock: the step starts compiler processes.
    pub build: bool,
}

/// Static facts about a step.
#[derive(Clone, Copy, Debug)]
pub struct StepInfo {
    pub name: &'static str,
    pub depends_on: &'static [StepId],
    pub implementation: Implementation,
    pub windows_only: bool,
    /// Set when the step blocks in CI but is deliberately absent from `pre-push`,
    /// with the reason. Every other blocking CI step must also block locally.
    pub ci_only: Option<&'static str>,
    pub locks: Locks,
}

const SANITY: &[StepId] = &[StepId::Sanity];
const CONFIGURE: &[StepId] = &[StepId::Configure];
const BUILD: &[StepId] = &[StepId::Build];
const TREE_AND_BUILD: Locks = Locks {
    tree: true,
    build: true,
};

impl StepId {
    pub const ALL: [StepId; 25] = [
        StepId::Sanity,
        StepId::Diff,
        StepId::Drift,
        StepId::SourceHygiene,
        StepId::DocsSuperpowersRemoved,
        StepId::CommitPolicy,
        StepId::LintCanaries,
        StepId::ProseLines,
        StepId::Format,
        StepId::PackagingVersion,
        StepId::MsiHarvest,
        StepId::PrivacyAllowlist,
        StepId::NetworkEgress,
        StepId::Actionlint,
        StepId::Zizmor,
        StepId::ScriptTests,
        StepId::Rust,
        StepId::Configure,
        StepId::QmlLint,
        StepId::Build,
        StepId::Tests,
        StepId::CppCheck,
        StepId::ClangTidy,
        StepId::PackagingSmoke,
        StepId::AvSyncGolden,
    ];

    pub fn name(self) -> &'static str {
        self.info().name
    }

    pub fn from_name(name: &str) -> Option<StepId> {
        StepId::ALL.into_iter().find(|step| step.name() == name)
    }

    pub fn info(self) -> StepInfo {
        use Implementation::{Legacy, Native};
        let step = |name, depends_on, implementation| StepInfo {
            name,
            depends_on,
            implementation,
            windows_only: false,
            ci_only: None,
            locks: Locks::default(),
        };
        match self {
            StepId::Sanity => step("sanity", &[], Native),
            StepId::Diff => step("diff", SANITY, Native),
            StepId::Drift => step("drift", SANITY, Native),
            StepId::SourceHygiene => step("source-hygiene", SANITY, Native),
            StepId::DocsSuperpowersRemoved => step(
                "docs-superpowers-removed",
                SANITY,
                Legacy {
                    owner: "exo-verify docs check",
                },
            ),
            StepId::CommitPolicy => step("commit-policy", SANITY, Native),
            StepId::LintCanaries => step("lint-canaries", SANITY, Native),
            StepId::ProseLines => step(
                "prose-lines",
                SANITY,
                Legacy {
                    owner: "none (retired)",
                },
            ),
            StepId::Format => step("format", SANITY, Native),
            StepId::PackagingVersion => step(
                "packaging-version",
                SANITY,
                Legacy {
                    owner: "exo-dev release module",
                },
            ),
            StepId::MsiHarvest => step(
                "msi-harvest",
                SANITY,
                Legacy {
                    owner: "exo-dev packaging module",
                },
            ),
            StepId::PrivacyAllowlist => step(
                "privacy-allowlist",
                SANITY,
                Legacy {
                    owner: "exo-dev privacy module",
                },
            ),
            StepId::NetworkEgress => step(
                "network-egress",
                SANITY,
                Legacy {
                    owner: "exo-dev privacy module",
                },
            ),
            StepId::Actionlint => StepInfo {
                ci_only: Some(
                    "a pinned Linux binary the workflow installs by digest; not a developer prerequisite",
                ),
                ..step("actionlint", SANITY, Native)
            },
            StepId::Zizmor => StepInfo {
                ci_only: Some(
                    "a pinned tool the workflow installs by version; not a developer prerequisite",
                ),
                ..step("zizmor", SANITY, Native)
            },
            StepId::ScriptTests => step(
                "script-tests",
                SANITY,
                Legacy {
                    owner: "Rust unit tests of each ported module",
                },
            ),
            StepId::Rust => step("rust", SANITY, Native),
            StepId::Configure => StepInfo {
                windows_only: true,
                locks: TREE_AND_BUILD,
                ..step("configure", SANITY, Native)
            },
            StepId::QmlLint => StepInfo {
                windows_only: true,
                locks: TREE_AND_BUILD,
                ..step("qmllint", CONFIGURE, Native)
            },
            StepId::Build => StepInfo {
                windows_only: true,
                locks: TREE_AND_BUILD,
                ..step("build", CONFIGURE, Native)
            },
            // No lock here: run-tests.ps1 takes the tree lock and the host device
            // lock itself, for the whole span from its build to its receipt.
            // Holding either out here as well would only widen the span.
            StepId::Tests => StepInfo {
                windows_only: true,
                ..step(
                    "tests",
                    BUILD,
                    Legacy {
                        owner: "exo-dev test",
                    },
                )
            },
            StepId::CppCheck => StepInfo {
                windows_only: true,
                ..step("cppcheck", SANITY, Native)
            },
            // clang-tidy reads compile_commands.json, which configure writes and
            // the build keeps in step with the source. The tree lock keeps a
            // concurrent configure from rewriting it mid-read.
            StepId::ClangTidy => StepInfo {
                windows_only: true,
                locks: TREE_AND_BUILD,
                ..step("clang-tidy", BUILD, Native)
            },
            StepId::PackagingSmoke => StepInfo {
                windows_only: true,
                ci_only: Some(
                    "packaging is not part of the local blocking contract; CI and the candidate workflow own it",
                ),
                ..step(
                    "packaging-smoke",
                    BUILD,
                    Legacy {
                        owner: "exo-verify package",
                    },
                )
            },
            StepId::AvSyncGolden => StepInfo {
                windows_only: true,
                ci_only: Some(
                    "needs Python and a system FFmpeg with signalstats; replaced by the exo-dev av-sync port",
                ),
                ..step(
                    "av-sync-golden",
                    SANITY,
                    Legacy {
                        owner: "exo-dev av-sync-check",
                    },
                )
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_step_is_listed_once_in_declaration_order() {
        let mut sorted = StepId::ALL;
        sorted.sort();
        assert_eq!(
            sorted,
            StepId::ALL,
            "ALL must follow the declaration (cost) order"
        );
        let mut names: Vec<_> = StepId::ALL.iter().map(|s| s.name()).collect();
        names.sort();
        names.dedup();
        assert_eq!(names.len(), StepId::ALL.len(), "step names must be unique");
    }

    #[test]
    fn dependencies_always_run_earlier() {
        for step in StepId::ALL {
            for dependency in step.info().depends_on {
                assert!(
                    *dependency < step,
                    "{} depends on {}, which is ordered after it",
                    step.name(),
                    dependency.name()
                );
            }
        }
    }

    #[test]
    fn tests_and_clang_tidy_stand_on_the_build() {
        assert!(StepId::Tests.info().depends_on.contains(&StepId::Build));
        assert!(StepId::ClangTidy.info().depends_on.contains(&StepId::Build));
    }

    #[test]
    fn every_step_that_writes_or_reads_the_build_tree_takes_the_tree_lock() {
        for step in [
            StepId::Configure,
            StepId::QmlLint,
            StepId::Build,
            StepId::ClangTidy,
        ] {
            let locks = step.info().locks;
            assert!(locks.tree, "{} must hold the tree lock", step.name());
            assert!(
                locks.build,
                "{} starts compiler processes and must hold the build lock",
                step.name()
            );
        }
    }

    #[test]
    fn names_round_trip() {
        for step in StepId::ALL {
            assert_eq!(StepId::from_name(step.name()), Some(step));
        }
    }
}
