//! Builds the ordered check list for one run.

use serde_json::{Map, Value};

use crate::profile::{ProfileSpec, TidyScope};
use crate::scope::Scope;
use crate::step::StepId;

/// The CI event a profile runs for, as GitHub names it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    Local,
    PullRequest,
    Other(String),
}

impl Event {
    pub fn parse(name: &str) -> Event {
        match name {
            "pull_request" => Event::PullRequest,
            other => Event::Other(other.to_string()),
        }
    }

    pub fn is_pull_request(&self) -> bool {
        *self == Event::PullRequest
    }
}

#[derive(Clone, Debug)]
pub struct PlanInput<'a> {
    pub profile: ProfileSpec,
    pub scope: &'a Scope,
    pub event: Event,
    pub base: Option<String>,
    pub staged: bool,
    pub packaging_changed: bool,
    pub windows: bool,
    pub preset: String,
    pub config: String,
}

#[derive(Clone, Debug)]
pub struct Check {
    pub id: StepId,
    pub applicable: bool,
    pub skip_reason: String,
    /// A failure is reported as WARN: it neither fails the run nor stops it.
    pub advisory: bool,
    /// Requests a QML diagnosis when the check fails.
    pub qml_diagnostics: bool,
    /// Facts the executor acts on and the receipt records.
    pub evidence: Map<String, Value>,
}

impl Check {
    pub fn name(&self) -> &'static str {
        self.id.name()
    }

    pub fn evidence_str(&self, key: &str) -> Option<&str> {
        self.evidence.get(key).and_then(Value::as_str)
    }
}

#[derive(Clone, Debug)]
pub struct Plan {
    pub profile: ProfileSpec,
    pub build_dir: String,
    pub preset: String,
    pub config: String,
    pub checks: Vec<Check>,
}

impl Plan {
    pub fn check(&self, id: StepId) -> Option<&Check> {
        self.checks.iter().find(|check| check.id == id)
    }
}

fn skip_unless(check: &mut Check, condition: bool, reason: &str) {
    if !condition && check.applicable {
        check.applicable = false;
        check.skip_reason = reason.to_string();
    }
}

pub fn build(input: &PlanInput) -> Plan {
    let profile = input.profile;
    let scope = input.scope;
    let scoped = profile.scoped;
    let pull_request = input.event.is_pull_request();
    let build_dir = format!("build/{}", input.preset);

    let want = |flag: bool| !scoped || flag;
    let checks = profile
        .planned_steps()
        .map(|id| {
            let mut check = Check {
                id,
                applicable: true,
                skip_reason: String::new(),
                advisory: false,
                qml_diagnostics: false,
                evidence: Map::new(),
            };
            match id {
                StepId::SourceHygiene if profile.ci && !pull_request => {
                    // A push or dispatch has no range a change can be attributed
                    // to, and the whole-tree backlog is older comments: visible on
                    // every run, never a gate.
                    check.advisory = true;
                    check.evidence.insert("scope".into(), "whole-tree".into());
                }
                StepId::SourceHygiene if profile.ci => {
                    check.evidence.insert("scope".into(), "range".into());
                }
                StepId::SourceHygiene => {
                    // The work in front of the developer, not the whole branch:
                    // that is what makes the rules adoptable on this tree.
                    check.evidence.insert("scope".into(), "working-tree".into());
                }
                StepId::CommitPolicy | StepId::ProseLines if profile.ci => {
                    skip_unless(&mut check, pull_request, "not a pull request");
                }
                StepId::ScriptTests => skip_unless(
                    &mut check,
                    want(scope.requires_script_tests),
                    "no script under scripts/ changed",
                ),
                StepId::Rust => skip_unless(
                    &mut check,
                    want(scope.requires_rust),
                    "nothing in the Rust workspace under tools/ changed",
                ),
                StepId::Configure => {
                    skip_unless(
                        &mut check,
                        want(scope.requires_configure),
                        "nothing that reaches the build system changed",
                    );
                    check
                        .evidence
                        .insert("preset".into(), input.preset.clone().into());
                    check
                        .evidence
                        .insert("buildDir".into(), build_dir.clone().into());
                }
                StepId::QmlLint => {
                    skip_unless(&mut check, want(scope.requires_qmllint), "no QML changed");
                    check.evidence.insert("target".into(), "all_qmllint".into());
                    check
                        .evidence
                        .insert("buildDir".into(), build_dir.clone().into());
                }
                StepId::Build => {
                    skip_unless(
                        &mut check,
                        want(scope.requires_build),
                        "no compiled source changed",
                    );
                    check
                        .evidence
                        .insert("preset".into(), input.preset.clone().into());
                    check
                        .evidence
                        .insert("buildDir".into(), build_dir.clone().into());
                    check
                        .evidence
                        .insert("config".into(), input.config.clone().into());
                }
                StepId::Tests => {
                    skip_unless(
                        &mut check,
                        want(scope.requires_tests),
                        "no compiled source or QML changed",
                    );
                    skip_unless(
                        &mut check,
                        !(profile.tests_skip_on_pull_request && pull_request),
                        "the debug leg carries the test suite on a pull request",
                    );
                    check.qml_diagnostics = true;
                    let filter = if scoped {
                        scope.test_filter.clone()
                    } else {
                        String::new()
                    };
                    check
                        .evidence
                        .insert("buildDir".into(), build_dir.clone().into());
                    check
                        .evidence
                        .insert("config".into(), input.config.clone().into());
                    check.evidence.insert("filter".into(), filter.into());
                    if let Some(label) = profile.exclude_label {
                        check.evidence.insert("excludeLabel".into(), label.into());
                    }
                }
                StepId::CppCheck => {
                    skip_unless(
                        &mut check,
                        want(scope.requires_static_analysis),
                        "no C++ changed",
                    );
                }
                StepId::ClangTidy => {
                    skip_unless(
                        &mut check,
                        want(scope.requires_static_analysis),
                        "no C++ changed",
                    );
                    let tidy_scope = match profile.tidy {
                        TidyScope::WholeTree => "whole-tree",
                        TidyScope::ChangedSinceBase => "changed",
                        TidyScope::ChangedSinceParent => "changed-since-parent",
                    };
                    check
                        .evidence
                        .insert("buildDir".into(), build_dir.clone().into());
                    check.evidence.insert("scope".into(), tidy_scope.into());
                }
                StepId::PackagingSmoke => {
                    skip_unless(
                        &mut check,
                        pull_request,
                        "not a pull request; the candidate workflow packages release builds",
                    );
                    skip_unless(
                        &mut check,
                        input.packaging_changed,
                        "no packaging-relevant path changed",
                    );
                }
                _ => {}
            }
            if id.info().windows_only && !input.windows {
                skip_unless(&mut check, false, "Windows only");
            }
            check
        })
        .collect();

    Plan {
        profile,
        build_dir,
        preset: input.preset.clone(),
        config: input.config.clone(),
        checks,
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::profile::Profile;

    pub fn input(profile: Profile, scope: &Scope) -> PlanInput<'_> {
        let spec = profile.spec();
        PlanInput {
            profile: spec,
            scope,
            event: if spec.ci {
                Event::PullRequest
            } else {
                Event::Local
            },
            base: Some("base".into()),
            staged: false,
            packaging_changed: false,
            windows: true,
            preset: spec.preset.into(),
            config: spec.config.into(),
        }
    }

    pub fn files(files: &[&str]) -> Scope {
        Scope::of(&files.iter().map(|f| f.to_string()).collect::<Vec<_>>())
    }

    fn names(plan: &Plan) -> Vec<&'static str> {
        plan.checks.iter().map(Check::name).collect()
    }

    #[test]
    fn full_ignores_the_change_set_entirely() {
        let scope = files(&["README.md"]);
        let plan = build(&input(Profile::PrePush, &scope));
        for check in &plan.checks {
            assert!(
                check.applicable,
                "pre-push skipped {} because of the change set",
                check.name()
            );
        }
    }

    #[test]
    fn the_blocking_clang_tidy_set_is_planned_once_and_whole_tree_before_a_push() {
        let scope = files(&["libs/engine/src/muxer.cpp"]);
        let fast = build(&input(Profile::PreCommit, &scope));
        let full = build(&input(Profile::PrePush, &scope));
        for plan in [&fast, &full] {
            assert_eq!(
                names(plan).iter().filter(|n| **n == "clang-tidy").count(),
                1
            );
        }
        assert_eq!(
            fast.check(StepId::ClangTidy).unwrap().evidence_str("scope"),
            Some("changed")
        );
        assert_eq!(
            full.check(StepId::ClangTidy).unwrap().evidence_str("scope"),
            Some("whole-tree")
        );
    }

    #[test]
    fn a_check_is_planned_at_most_once() {
        let scope = files(&[]);
        for profile in Profile::ALL {
            let plan = build(&input(profile, &scope));
            let mut seen = names(&plan);
            seen.sort();
            seen.dedup();
            assert_eq!(
                seen.len(),
                plan.checks.len(),
                "{} plans a duplicate",
                profile.spec().name
            );
        }
    }

    #[test]
    fn source_hygiene_runs_in_both_local_contracts_before_the_compile() {
        let scope = files(&[]);
        for profile in [Profile::PreCommit, Profile::PrePush] {
            let n = names(&build(&input(profile, &scope)));
            let hygiene = n.iter().position(|x| *x == "source-hygiene").unwrap();
            let compile = n.iter().position(|x| *x == "build").unwrap();
            assert!(hygiene < compile);
        }
    }

    #[test]
    fn no_plan_ever_lets_tests_run_without_a_build_in_scope() {
        for set in [
            &["libs/engine/src/muxer.cpp"][..],
            &["app/quick/ExoSnap/Quick/RecordPage.qml"][..],
            &["libs/engine/include/exosnap/engine/session.h"][..],
            &["CMakeLists.txt"][..],
            &["scripts/run-tests.ps1"][..],
            &[".github/workflows/ci.yml"][..],
            &[][..],
        ] {
            let scope = files(set);
            let plan = build(&input(Profile::PreCommit, &scope));
            let tests = plan.check(StepId::Tests).unwrap();
            if tests.applicable {
                assert!(plan.check(StepId::Build).unwrap().applicable, "{set:?}");
            }
        }
    }

    #[test]
    fn a_qml_change_narrows_the_scoped_suite_but_never_a_complete_one() {
        let scope = files(&["app/quick/ExoSnap/Quick/RecordPage.qml"]);
        let fast = build(&input(Profile::PreCommit, &scope));
        let full = build(&input(Profile::PrePush, &scope));
        assert_eq!(
            fast.check(StepId::Tests).unwrap().evidence_str("filter"),
            Some(r"^quick\.")
        );
        assert_eq!(
            full.check(StepId::Tests).unwrap().evidence_str("filter"),
            Some("")
        );
    }

    #[test]
    fn pull_request_only_guardrails_skip_on_other_events() {
        let scope = Scope::everything();
        let mut push = input(Profile::CiGuardrails, &scope);
        push.event = Event::parse("push");
        let plan = build(&push);
        assert!(!plan.check(StepId::CommitPolicy).unwrap().applicable);
        assert!(!plan.check(StepId::ProseLines).unwrap().applicable);
        let hygiene = plan.check(StepId::SourceHygiene).unwrap();
        assert!(
            hygiene.applicable && hygiene.advisory,
            "the backlog is reported, never a gate"
        );

        let pr = build(&input(Profile::CiGuardrails, &scope));
        let hygiene = pr.check(StepId::SourceHygiene).unwrap();
        assert!(
            hygiene.applicable && !hygiene.advisory,
            "the PR range is a gate"
        );
        assert!(pr.check(StepId::CommitPolicy).unwrap().applicable);
    }

    #[test]
    fn the_release_leg_tests_only_outside_pull_requests_and_packages_only_on_packaging_changes() {
        let scope = Scope::everything();
        let pr = build(&input(Profile::CiBuildRelease, &scope));
        assert!(!pr.check(StepId::Tests).unwrap().applicable);
        assert!(!pr.check(StepId::PackagingSmoke).unwrap().applicable);

        let mut packaging = input(Profile::CiBuildRelease, &scope);
        packaging.packaging_changed = true;
        assert!(
            build(&packaging)
                .check(StepId::PackagingSmoke)
                .unwrap()
                .applicable
        );

        let mut tag = input(Profile::CiBuildRelease, &scope);
        tag.event = Event::parse("push");
        tag.packaging_changed = true;
        let plan = build(&tag);
        assert!(plan.check(StepId::Tests).unwrap().applicable);
        assert!(!plan.check(StepId::PackagingSmoke).unwrap().applicable);
    }

    #[test]
    fn ci_build_legs_exclude_live_tests() {
        let scope = Scope::everything();
        for profile in [Profile::CiBuildDebug, Profile::CiBuildRelease] {
            let mut i = input(profile, &scope);
            i.event = Event::parse("workflow_dispatch");
            let plan = build(&i);
            assert_eq!(
                plan.check(StepId::Tests)
                    .unwrap()
                    .evidence_str("excludeLabel"),
                Some("live")
            );
        }
    }

    #[test]
    fn windows_only_steps_skip_elsewhere() {
        let scope = Scope::everything();
        let mut linux = input(Profile::PrePush, &scope);
        linux.windows = false;
        let plan = build(&linux);
        let build_check = plan.check(StepId::Build).unwrap();
        assert!(!build_check.applicable);
        assert_eq!(build_check.skip_reason, "Windows only");
        assert!(plan.check(StepId::Drift).unwrap().applicable);
    }
}
