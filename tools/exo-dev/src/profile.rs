//! Named step selections: the local hooks and the CI jobs.
//!
//! A CI job's YAML prepares the machine and calls one profile. What that job
//! blocks on is decided here, next to the local contract it has to agree with.

use crate::step::StepId;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Profile {
    PreCommit,
    PrePush,
    CiLint,
    CiGuardrails,
    PrPolicy,
    CiDevScripts,
    CiBuildDebug,
    CiBuildRelease,
}

/// How the blocking clang-tidy set chooses its translation units.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TidyScope {
    /// What the change against the run's base reaches.
    ChangedSinceBase,
    /// Every translation unit.
    WholeTree,
    /// What the checked-out commit changes against its first parent. On a
    /// pull_request checkout that parent is the base branch tip.
    ChangedSinceParent,
}

#[derive(Clone, Copy, Debug)]
pub struct ProfileSpec {
    pub name: &'static str,
    pub steps: &'static [StepId],
    /// Narrowed by the change set. Only the pre-commit contract is.
    pub scoped: bool,
    /// A CI job: requires `--event`, streams output, and decides per-event steps.
    pub ci: bool,
    pub preset: &'static str,
    pub config: &'static str,
    pub configure_args: &'static [&'static str],
    pub exclude_label: Option<&'static str>,
    pub tidy: TidyScope,
    /// The Release leg's pull-request tier is configure, build and link: the
    /// debug leg already ran the full suite.
    pub tests_skip_on_pull_request: bool,
}

impl ProfileSpec {
    /// Whether the profile claims completeness. A complete run fails when a tool
    /// it needed is not installed; a gate that never started established nothing.
    pub fn complete(&self) -> bool {
        !self.scoped
    }

    pub fn contains(&self, step: StepId) -> bool {
        step == StepId::Sanity || self.steps.contains(&step)
    }

    pub fn planned_steps(&self) -> impl Iterator<Item = StepId> + '_ {
        StepId::ALL.into_iter().filter(|step| self.contains(*step))
    }
}

const LOCAL_STEPS: &[StepId] = &[
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
    StepId::ScriptTests,
    StepId::Rust,
    StepId::Configure,
    StepId::QmlLint,
    StepId::Build,
    StepId::Tests,
    StepId::CppCheck,
    StepId::ClangTidy,
];

const DEBUG_PRESET: &str = "windows-x64-ninja-debug";
const RELEASE_PRESET: &str = "windows-x64-ninja-release";
const SCCACHE: &[&str] = &["-DEXOSNAP_USE_SCCACHE=ON"];

const BASE: ProfileSpec = ProfileSpec {
    name: "",
    steps: &[],
    scoped: false,
    ci: true,
    preset: DEBUG_PRESET,
    config: "Debug",
    configure_args: &[],
    // The runners have no GPU. Tests labelled `live` read real machine state
    // and are excluded rather than trusted to skip themselves.
    exclude_label: Some("live"),
    tidy: TidyScope::ChangedSinceParent,
    tests_skip_on_pull_request: false,
};

impl Profile {
    pub const ALL: [Profile; 8] = [
        Profile::PreCommit,
        Profile::PrePush,
        Profile::CiLint,
        Profile::CiGuardrails,
        Profile::PrPolicy,
        Profile::CiDevScripts,
        Profile::CiBuildDebug,
        Profile::CiBuildRelease,
    ];

    pub fn from_name(name: &str) -> Option<Profile> {
        Profile::ALL
            .into_iter()
            .find(|profile| profile.spec().name == name)
    }

    pub fn spec(self) -> ProfileSpec {
        match self {
            Profile::PreCommit => ProfileSpec {
                name: "pre-commit",
                steps: LOCAL_STEPS,
                scoped: true,
                ci: false,
                exclude_label: None,
                tidy: TidyScope::ChangedSinceBase,
                ..BASE
            },
            Profile::PrePush => ProfileSpec {
                name: "pre-push",
                steps: LOCAL_STEPS,
                ci: false,
                exclude_label: None,
                tidy: TidyScope::WholeTree,
                ..BASE
            },
            Profile::CiLint => ProfileSpec {
                name: "ci-lint",
                steps: &[
                    StepId::Format,
                    StepId::PackagingVersion,
                    StepId::MsiHarvest,
                    StepId::PrivacyAllowlist,
                    StepId::NetworkEgress,
                ],
                ..BASE
            },
            Profile::CiGuardrails => ProfileSpec {
                name: "ci-guardrails",
                steps: &[
                    StepId::Drift,
                    StepId::SourceHygiene,
                    StepId::CommitPolicy,
                    StepId::ProseLines,
                    StepId::Actionlint,
                    StepId::Zizmor,
                ],
                ..BASE
            },
            Profile::PrPolicy => ProfileSpec {
                name: "pr-policy",
                steps: &[StepId::CommitPolicy],
                ..BASE
            },
            Profile::CiDevScripts => ProfileSpec {
                name: "ci-dev-scripts",
                steps: &[StepId::AvSyncGolden],
                ..BASE
            },
            Profile::CiBuildDebug => ProfileSpec {
                name: "ci-build-debug",
                steps: &[
                    StepId::Configure,
                    StepId::QmlLint,
                    StepId::Build,
                    StepId::Tests,
                    StepId::ClangTidy,
                ],
                configure_args: SCCACHE,
                ..BASE
            },
            Profile::CiBuildRelease => ProfileSpec {
                name: "ci-build-release",
                steps: &[
                    StepId::Configure,
                    StepId::Build,
                    StepId::Tests,
                    StepId::PackagingSmoke,
                ],
                preset: RELEASE_PRESET,
                config: "Release",
                configure_args: SCCACHE,
                tests_skip_on_pull_request: true,
                ..BASE
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const LINUX_PROFILES: [Profile; 2] = [Profile::CiGuardrails, Profile::PrPolicy];

    #[test]
    fn every_blocking_ci_step_also_blocks_before_a_push_or_says_why_not() {
        let pre_push = Profile::PrePush.spec();
        for profile in Profile::ALL.into_iter().filter(|p| p.spec().ci) {
            for step in profile.spec().planned_steps() {
                if pre_push.contains(step) {
                    continue;
                }
                assert!(
                    step.info().ci_only.is_some(),
                    "{} blocks in {} but not before a push, and declares no ci_only reason",
                    step.name(),
                    profile.spec().name
                );
            }
        }
    }

    #[test]
    fn a_ci_only_step_is_really_absent_locally() {
        let pre_push = Profile::PrePush.spec();
        for step in StepId::ALL {
            if step.info().ci_only.is_some() {
                assert!(
                    !pre_push.contains(step),
                    "{} declares ci_only but pre-push runs it",
                    step.name()
                );
            }
        }
    }

    #[test]
    fn pre_push_contains_everything_pre_commit_can_run() {
        let fast = Profile::PreCommit.spec();
        let full = Profile::PrePush.spec();
        for step in fast.planned_steps() {
            assert!(full.contains(step), "pre-push is missing {}", step.name());
        }
        assert!(full.complete() && !fast.complete());
    }

    #[test]
    fn every_profile_plans_the_dependencies_of_its_steps() {
        for profile in Profile::ALL {
            let spec = profile.spec();
            for step in spec.planned_steps() {
                for dependency in step.info().depends_on {
                    assert!(
                        spec.contains(*dependency),
                        "{} plans {} without its dependency {}",
                        spec.name,
                        step.name(),
                        dependency.name()
                    );
                }
            }
        }
    }

    #[test]
    fn linux_profiles_contain_no_windows_only_step() {
        for profile in LINUX_PROFILES {
            for step in profile.spec().planned_steps() {
                assert!(
                    !step.info().windows_only,
                    "{} runs on Linux but plans the Windows-only step {}",
                    profile.spec().name,
                    step.name()
                );
            }
        }
    }

    /// The guardrails title check is a cost gate for the Windows legs; the
    /// authoritative verdict is the pr-policy workflow's. Both must be the same
    /// step, or a preflight could reject a title the required check accepts.
    #[test]
    fn the_title_preflight_and_the_required_title_check_are_the_same_step() {
        assert!(Profile::CiGuardrails.spec().contains(StepId::CommitPolicy));
        assert_eq!(
            Profile::PrPolicy.spec().planned_steps().collect::<Vec<_>>(),
            vec![StepId::Sanity, StepId::CommitPolicy]
        );
        assert!(Profile::CiGuardrails.spec().ci && Profile::PrPolicy.spec().ci);
    }

    #[test]
    fn only_the_pre_commit_contract_is_scoped() {
        for profile in Profile::ALL {
            assert_eq!(profile.spec().scoped, profile == Profile::PreCommit);
        }
    }

    #[test]
    fn names_are_unique_and_resolve() {
        for profile in Profile::ALL {
            assert_eq!(Profile::from_name(profile.spec().name), Some(profile));
        }
    }
}
