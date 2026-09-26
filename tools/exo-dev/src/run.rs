//! Runs a plan against an executor and records what actually happened.
//!
//! The rule this module enforces: a check whose prerequisite did not pass is never
//! PASS. Tests that run against binaries not built from the current source in the
//! same invocation are not evidence about that source, so a failed build leaves
//! SKIPPED_DEPENDENCY behind it, which is not a green.

use std::time::Instant;

use serde_json::{Map, Value};

use crate::plan::{Check, Plan};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Pass,
    Fail,
    /// An advisory check that failed. Reported, never a gate.
    Warn,
    /// Out of scope for this run.
    Skip,
    /// A prerequisite did not pass.
    SkippedDependency,
    /// An earlier failure stopped the run.
    NotRun,
    /// The tool the check needs is not installed. Nobody decided not to run it; it
    /// could not run, so it establishes nothing.
    ToolMissing,
}

impl Status {
    pub fn as_str(self) -> &'static str {
        match self {
            Status::Pass => "PASS",
            Status::Fail => "FAIL",
            Status::Warn => "WARN",
            Status::Skip => "SKIP",
            Status::SkippedDependency => "SKIPPED_DEPENDENCY",
            Status::NotRun => "NOT_RUN",
            Status::ToolMissing => "TOOL_MISSING",
        }
    }

    fn satisfies_dependents(self) -> bool {
        matches!(self, Status::Pass | Status::Skip | Status::Warn)
    }
}

#[derive(Clone, Debug)]
pub struct Outcome {
    pub status: Status,
    pub detail: String,
    pub evidence: Map<String, Value>,
}

impl Outcome {
    pub fn pass(detail: impl Into<String>) -> Outcome {
        Outcome::new(Status::Pass, detail)
    }

    pub fn fail(detail: impl Into<String>) -> Outcome {
        Outcome::new(Status::Fail, detail)
    }

    pub fn new(status: Status, detail: impl Into<String>) -> Outcome {
        Outcome {
            status,
            detail: detail.into(),
            evidence: Map::new(),
        }
    }

    pub fn with_log(mut self, log: &str) -> Outcome {
        self.evidence.insert("log".into(), log.into());
        self
    }
}

pub trait Executor {
    fn execute(&mut self, check: &Check) -> Outcome;

    /// Called when a check that asks for a diagnosis fails. Returns evidence paths.
    fn diagnose(&mut self, _check: &Check, _outcome: &Outcome) -> Vec<String> {
        Vec::new()
    }
}

#[derive(Clone, Debug)]
pub struct CheckResult {
    pub name: &'static str,
    pub status: Status,
    pub detail: String,
    pub duration_ms: u128,
    pub depends_on: Vec<&'static str>,
    pub implementation: &'static str,
    pub evidence: Map<String, Value>,
    pub diagnostics: Vec<String>,
}

#[derive(Clone, Debug)]
pub struct RunResult {
    pub passed: bool,
    pub tool_missing: Vec<&'static str>,
    pub checks: Vec<CheckResult>,
}

impl RunResult {
    pub fn check(&self, name: &str) -> &CheckResult {
        self.checks
            .iter()
            .find(|c| c.name == name)
            .unwrap_or_else(|| panic!("check '{name}' is not in the run"))
    }
}

/// Sequential on purpose. The independent read-only checks after `sanity` take
/// about twenty-five seconds together on a sixteen-core machine, and the checks
/// that dominate a run (build, tests, script tests) each own a host lock or the
/// whole job budget and cannot overlap anything. A parallel phase would buy a few
/// percent for a second failure ordering to reason about; the durations in the
/// receipt are where to see whether that has changed.
pub fn run(plan: &Plan, executor: &mut dyn Executor) -> RunResult {
    let mut results: Vec<CheckResult> = Vec::with_capacity(plan.checks.len());
    let mut failed_by: Option<&'static str> = None;
    let mut tool_missing = Vec::new();

    for check in &plan.checks {
        let info = check.id.info();
        let mut result = CheckResult {
            name: check.name(),
            status: Status::Skip,
            detail: String::new(),
            duration_ms: 0,
            depends_on: info.depends_on.iter().map(|d| d.name()).collect(),
            implementation: match info.implementation {
                crate::step::Implementation::Native => "native",
                crate::step::Implementation::Legacy { .. } => "legacy",
            },
            evidence: check.evidence.clone(),
            diagnostics: Vec::new(),
        };

        let blocking: Vec<&str> = info
            .depends_on
            .iter()
            .filter_map(|dependency| {
                results
                    .iter()
                    .find(|r| r.name == dependency.name())
                    .filter(|r| !r.status.satisfies_dependents())
                    .map(|r| r.name)
            })
            .collect();

        if !check.applicable {
            result.detail = check.skip_reason.clone();
        } else if !blocking.is_empty() {
            result.status = Status::SkippedDependency;
            result.detail = format!("depends on {}, which did not pass", blocking.join(", "));
        } else if let Some(failed) = failed_by {
            result.status = Status::NotRun;
            result.detail = format!("stopped after '{failed}' failed");
        } else {
            let started = Instant::now();
            let outcome = executor.execute(check);
            result.duration_ms = started.elapsed().as_millis();
            result.status = outcome.status;
            result.detail = outcome.detail.clone();
            for (key, value) in &outcome.evidence {
                result.evidence.insert(key.clone(), value.clone());
            }

            match outcome.status {
                Status::Fail if check.advisory => result.status = Status::Warn,
                Status::Fail => {
                    if check.qml_diagnostics {
                        result.diagnostics = executor.diagnose(check, &outcome);
                        if result.diagnostics.is_empty() {
                            result.detail = format!(
                                "{} (no qml diagnostics were produced; an exit code is not a test report)",
                                result.detail
                            )
                            .trim()
                            .to_string();
                        }
                    }
                    failed_by = Some(check.name());
                }
                // Deliberately not fail-fast: an absent tool says nothing about the
                // checks after it, so the run continues and reports every verdict
                // it can still establish.
                Status::ToolMissing => tool_missing.push(check.name()),
                _ => {}
            }
        }
        results.push(result);
    }

    let incomplete = !tool_missing.is_empty() && plan.profile.complete();
    RunResult {
        passed: failed_by.is_none() && !incomplete,
        tool_missing,
        checks: results,
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::plan::tests::{files, input};
    use crate::plan::{self, Check};
    use crate::profile::Profile;
    use crate::scope::Scope;

    /// Passes everything except the named checks and records what it started.
    pub struct Fake {
        pub failing: Vec<&'static str>,
        pub missing: Vec<&'static str>,
        pub started: Vec<&'static str>,
        pub diagnosis: Vec<String>,
    }

    impl Fake {
        pub fn failing(names: &[&'static str]) -> Fake {
            Fake {
                failing: names.to_vec(),
                missing: Vec::new(),
                started: Vec::new(),
                diagnosis: Vec::new(),
            }
        }
    }

    impl Executor for Fake {
        fn execute(&mut self, check: &Check) -> Outcome {
            self.started.push(check.name());
            if self.missing.contains(&check.name()) {
                return Outcome::new(Status::ToolMissing, "not installed");
            }
            if self.failing.contains(&check.name()) {
                return Outcome::fail("fake failure").with_log("C:/logs/x.log");
            }
            Outcome::pass("")
        }

        fn diagnose(&mut self, _: &Check, _: &Outcome) -> Vec<String> {
            self.diagnosis.clone()
        }
    }

    fn run_with(profile: Profile, scope: &Scope, fake: &mut Fake) -> RunResult {
        run(&plan::build(&input(profile, scope)), fake)
    }

    #[test]
    fn a_diff_failure_stops_everything_after_it() {
        let mut fake = Fake::failing(&["diff"]);
        let run = run_with(Profile::PrePush, &Scope::everything(), &mut fake);
        assert!(!run.passed);
        assert_eq!(run.check("diff").status, Status::Fail);
        for later in [
            "format",
            "configure",
            "qmllint",
            "build",
            "tests",
            "cppcheck",
            "clang-tidy",
        ] {
            assert_ne!(
                run.check(later).status,
                Status::Pass,
                "{later} claims PASS after diff failed"
            );
            assert!(
                !fake.started.contains(&later),
                "{later} was started after diff failed"
            );
        }
    }

    #[test]
    fn a_format_failure_stops_the_compile() {
        let mut fake = Fake::failing(&["format"]);
        let run = run_with(Profile::PrePush, &Scope::everything(), &mut fake);
        assert!(!run.passed);
        assert!(!fake.started.contains(&"build"));
        assert!(matches!(
            run.check("build").status,
            Status::NotRun | Status::SkippedDependency
        ));
        assert_ne!(run.check("tests").status, Status::Pass);
    }

    #[test]
    fn every_blocking_gate_fails_the_run() {
        for name in [
            "source-hygiene",
            "drift",
            "cppcheck",
            "clang-tidy",
            "script-tests",
            "rust",
            "network-egress",
        ] {
            let mut fake = Fake::failing(&[name]);
            let run = run_with(Profile::PrePush, &Scope::everything(), &mut fake);
            assert!(!run.passed, "{name} must be a blocking gate");
        }
    }

    #[test]
    fn a_failed_build_never_leaves_a_passing_test_result() {
        let mut fake = Fake::failing(&["build"]);
        let run = run_with(Profile::PrePush, &Scope::everything(), &mut fake);
        assert_eq!(run.check("build").status, Status::Fail);
        assert_eq!(run.check("tests").status, Status::SkippedDependency);
        assert_eq!(run.check("clang-tidy").status, Status::SkippedDependency);
        assert!(!fake.started.contains(&"tests"));
    }

    #[test]
    fn a_failed_configure_blocks_qmllint_build_and_tests() {
        let mut fake = Fake::failing(&["configure"]);
        let run = run_with(Profile::PrePush, &Scope::everything(), &mut fake);
        assert_eq!(run.check("qmllint").status, Status::SkippedDependency);
        assert_eq!(run.check("build").status, Status::SkippedDependency);
        assert_ne!(run.check("tests").status, Status::Pass);
    }

    #[test]
    fn an_out_of_scope_prerequisite_does_not_block() {
        let scope = files(&[".github/workflows/ci.yml"]);
        let run = run_with(Profile::PreCommit, &scope, &mut Fake::failing(&[]));
        assert_eq!(run.check("build").status, Status::Skip);
        assert_eq!(run.check("tests").status, Status::Skip);
        assert!(run.passed);
    }

    #[test]
    fn a_failing_qml_test_carries_its_diagnosis() {
        let mut fake = Fake::failing(&["tests"]);
        fake.diagnosis = vec!["C:/logs/record_controls_qml_tests.txt".into()];
        let run = run_with(Profile::PrePush, &Scope::everything(), &mut fake);
        assert_eq!(run.check("tests").diagnostics.len(), 1);
    }

    #[test]
    fn a_qml_failure_without_a_diagnosis_says_so() {
        let mut fake = Fake::failing(&["tests"]);
        let run = run_with(Profile::PrePush, &Scope::everything(), &mut fake);
        assert!(
            run.check("tests")
                .detail
                .contains("an exit code is not a test report")
        );
    }

    #[test]
    fn a_missing_tool_is_never_pass_and_fails_a_complete_run() {
        let mut fake = Fake::failing(&[]);
        fake.missing = vec!["cppcheck"];
        let scoped = run_with(Profile::PreCommit, &files(&["libs/a.cpp"]), &mut fake);
        assert_eq!(scoped.check("cppcheck").status, Status::ToolMissing);
        assert_eq!(scoped.tool_missing, vec!["cppcheck"]);
        assert!(
            scoped.passed,
            "a scoped run reports the gap and stays usable"
        );

        let mut fake = Fake::failing(&[]);
        fake.missing = vec!["cppcheck"];
        let complete = run_with(Profile::PrePush, &Scope::everything(), &mut fake);
        assert!(
            !complete.passed,
            "a complete run may not pass over a gate that never started"
        );
        assert!(
            fake.started.contains(&"clang-tidy"),
            "a missing tool is not fail-fast"
        );
        assert_eq!(complete.check("clang-tidy").status, Status::Pass);
    }

    #[test]
    fn an_advisory_failure_warns_and_does_not_stop_the_run() {
        let scope = Scope::everything();
        let mut push = input(Profile::CiGuardrails, &scope);
        push.event = crate::plan::Event::parse("push");
        let mut fake = Fake::failing(&["source-hygiene"]);
        let run = run(&plan::build(&push), &mut fake);
        assert_eq!(run.check("source-hygiene").status, Status::Warn);
        assert!(fake.started.contains(&"zizmor"));
        assert!(run.passed);
    }

    #[test]
    fn statuses_are_distinct() {
        let all = [
            Status::Pass,
            Status::Fail,
            Status::Warn,
            Status::Skip,
            Status::SkippedDependency,
            Status::NotRun,
            Status::ToolMissing,
        ];
        let mut names: Vec<_> = all.iter().map(|s| s.as_str()).collect();
        names.sort();
        names.dedup();
        assert_eq!(names.len(), all.len());
    }
}
