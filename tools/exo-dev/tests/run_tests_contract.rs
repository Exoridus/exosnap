//! The `exo-dev test` runner against real ctest.
//!
//! Most cases point the runner at a throwaway build tree made of a
//! hand-written CTestTestfile.cmake, so the real ctest answers the real
//! question and nothing is stubbed but the host: the locks are recorded
//! rather than taken, so which lock is held when can be read back, and the
//! cases run on any platform. The cases about the build, the freshness and
//! `reusable` need a tree a build system answers for, so those configure a
//! real CMake project with no languages enabled.
//!
//! Cases that only mean something with real named mutexes live at the end,
//! Windows only, each in a lock namespace of its own.

use std::path::{Path, PathBuf};

use exo_dev::host_lock::{self, LockKind};
use exo_dev::test::catalog::Phase;
use exo_dev::test::host::{Checkpoint, Console};
use exo_dev::test::runner::{self, Options, RunResult};
use exo_dev::test_support::test_runner::{
    RecordingHost, cmake_path, configured_tree, env_dump_script, failing, hand_written_tree,
    labelled_tree, options, passing, read_env_dump, script, scripted, source_repo,
};
use serde_json::Value;

struct Outcome {
    result: RunResult,
    output: String,
    receipt: Option<Value>,
}

fn run_with(options: &Options, host: &RecordingHost) -> Outcome {
    let mut console = Console::captured();
    let result = runner::run(options, host, &mut console);
    let receipt = std::fs::read_to_string(&result.receipt_path)
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok());
    Outcome {
        result,
        output: console.text().to_string(),
        receipt,
    }
}

fn run(options: &Options) -> Outcome {
    run_with(options, &RecordingHost::new())
}

impl Outcome {
    fn code(&self) -> i32 {
        self.result.exit_code
    }

    fn receipt(&self) -> &Value {
        self.receipt
            .as_ref()
            .unwrap_or_else(|| panic!("no receipt was written:\n{}", self.output))
    }

    fn reasons(&self) -> Vec<String> {
        self.receipt()["invalid_reasons"]
            .as_array()
            .unwrap()
            .iter()
            .map(|r| r.as_str().unwrap().to_string())
            .collect()
    }
}

fn strings(value: &Value) -> Vec<String> {
    value
        .as_array()
        .unwrap_or_else(|| panic!("{value} is not an array"))
        .iter()
        .map(|v| v.as_str().unwrap().to_string())
        .collect()
}

fn run_ctest_names(build: &Path, extra: &[&str]) -> Vec<String> {
    let mut command = exo_dev::process::command("ctest");
    command
        .arg("--test-dir")
        .arg(build)
        .args(["-C", "Debug", "--show-only=json-v1"])
        .args(extra);
    let (_, stdout) = exo_dev::process::query(command).unwrap();
    exo_dev::test::catalog::parse_catalog(&stdout)
        .unwrap()
        .into_iter()
        .map(|t| t.name)
        .collect()
}

// --- Selection and labels ---------------------------------------------------

#[test]
fn a_tree_with_a_test_that_declares_no_phase_is_refused_and_the_test_is_named() {
    let repo = source_repo();
    let tree = hand_written_tree(&format!(
        "{}{}",
        passing("fixture.hardware", "live;phase.gpu"),
        passing("fixture.unlabelled", ""),
    ));
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    assert!(outcome.output.contains("no phase"), "{}", outcome.output);
    assert!(outcome.output.contains("fixture.unlabelled"));
    assert!(
        strings(&outcome.receipt()["phase_violations"]["missing"])
            .contains(&"fixture.unlabelled".to_string())
    );
}

#[test]
fn a_test_with_two_phases_is_refused_although_the_totals_add_up() {
    let repo = source_repo();
    let tree = hand_written_tree(&format!(
        "{}{}{}",
        passing("fixture.no_phase", ""),
        passing("fixture.two_phases", "phase.hermetic;phase.gpu"),
        passing("fixture.one_phase", "phase.hermetic"),
    ));
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    let violations = &outcome.receipt()["phase_violations"];
    assert!(strings(&violations["missing"]).contains(&"fixture.no_phase".to_string()));
    assert!(
        strings(&violations["multiple"])
            .iter()
            .any(|m| m.starts_with("fixture.two_phases"))
    );
}

#[test]
fn a_phase_label_outside_the_vocabulary_is_refused() {
    let repo = source_repo();
    let tree = hand_written_tree(&passing("fixture.typo", "phase.hermitic"));
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    assert!(
        strings(&outcome.receipt()["phase_violations"]["unknown"])
            .iter()
            .any(|u| u.contains("fixture.typo"))
    );
}

#[test]
fn a_phase_selects_exactly_the_tests_that_declare_it() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&Options {
        phase: Some(Phase::Gpu),
        ..options(repo.path(), tree.path())
    });
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    assert_eq!(outcome.receipt()["phase"], "gpu");
    assert_eq!(outcome.receipt()["tests_selected"], 1);
}

#[test]
fn excluding_live_keeps_the_live_verify_suites() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&Options {
        exclude_label: "live".into(),
        ..options(repo.path(), tree.path())
    });
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    let receipt = outcome.receipt();
    assert_eq!(receipt["exclude_pattern"], "^live$");
    assert_eq!(receipt["tests_selected"], 2);
    assert_eq!(receipt["tests_passed"], 2);
}

#[test]
fn the_unanchored_pattern_this_guards_against_still_drops_live_verify() {
    // The premise. If ctest ever stopped treating -LE as a regex, the
    // anchoring would be testing nothing and this case would notice.
    let tree = labelled_tree();
    let unanchored = run_ctest_names(tree.path(), &["-LE", "live"]);
    assert!(
        !unanchored.contains(&"fixture.script_suite".to_string()),
        "ctest -LE live no longer drops live_verify; the anchoring has lost its premise"
    );
    let anchored = run_ctest_names(tree.path(), &["-LE", "^live$"]);
    assert!(anchored.contains(&"fixture.script_suite".to_string()));
}

#[test]
fn excluding_live_verify_still_excludes_exactly_that_label() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&Options {
        exclude_label: "live_verify".into(),
        ..options(repo.path(), tree.path())
    });
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    assert_eq!(outcome.receipt()["tests_selected"], 2);
}

#[test]
fn a_caller_supplied_regex_is_passed_through_unchanged() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&Options {
        exclude_label: "^(live|live_verify)$".into(),
        ..options(repo.path(), tree.path())
    });
    assert_eq!(outcome.receipt()["exclude_pattern"], "^(live|live_verify)$");
    assert_eq!(outcome.receipt()["tests_selected"], 1);
}

#[test]
fn a_selection_that_matches_nothing_fails_instead_of_passing() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&Options {
        filter: "no.such.test".into(),
        ..options(repo.path(), tree.path())
    });
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    assert!(
        outcome
            .reasons()
            .contains(&"the selection matches no test that would run".to_string())
    );
}

#[test]
fn an_empty_build_tree_fails_instead_of_passing() {
    let repo = source_repo();
    let tree = hand_written_tree("");
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    assert!(
        outcome
            .reasons()
            .contains(&"the build tree registers no tests".to_string())
    );
}

// --- Census -----------------------------------------------------------------

#[test]
fn a_disabled_test_is_accounted_for_as_disabled_not_as_a_missing_result() {
    let repo = source_repo();
    let tree = hand_written_tree(&format!(
        "{}{}set_tests_properties(fixture.disabled PROPERTIES DISABLED TRUE)\n",
        passing("fixture.runs", "phase.hermetic"),
        passing("fixture.disabled", "phase.hermetic"),
    ));
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    let receipt = outcome.receipt();
    assert_eq!(receipt["census_mismatch"], false);
    assert_eq!(receipt["tests_selected"], 2);
    assert_eq!(receipt["tests_disabled"], 1);
    assert_eq!(receipt["tests_expected"], 1);
    assert_eq!(receipt["tests_accounted"], 1);
}

#[test]
fn a_run_whose_output_could_not_be_recorded_is_not_reported_as_a_pass() {
    // The log path is occupied by a directory: the cheapest way to make the
    // recording fail.
    let repo = source_repo();
    let tree = labelled_tree();
    std::fs::create_dir_all(tree.path().join("Testing/last-run.log")).unwrap();
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    assert_eq!(outcome.receipt()["reusable"], false);
    assert!(!outcome.reasons().is_empty());
}

/// A host that rewrites the suite's log after ctest wrote it and before the
/// run reads it, as a ctest that dropped or invented tests would have.
fn rewriting_log(log: PathBuf, text: &'static str) -> RecordingHost {
    RecordingHost::new().on_checkpoint(move |point, _| {
        if point == Checkpoint::AfterSuiteBeforeVerdict {
            std::fs::write(&log, text)?;
        }
        Ok(())
    })
}

#[test]
fn a_summary_that_accounts_for_a_different_number_of_tests_is_a_census_mismatch() {
    let repo = source_repo();
    let tree = labelled_tree();
    let host = rewriting_log(
        tree.path().join("Testing/last-run.log"),
        "100% tests passed out of 5\n",
    );
    let outcome = run_with(&options(repo.path(), tree.path()), &host);
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    assert_eq!(outcome.receipt()["census_mismatch"], true);
    assert!(
        outcome
            .reasons()
            .contains(&"census mismatch: expected 3 accounted tests, got 5".to_string())
    );
}

#[test]
fn a_suite_log_without_a_summary_is_not_a_verdict() {
    let repo = source_repo();
    let tree = labelled_tree();
    let host = rewriting_log(tree.path().join("Testing/last-run.log"), "");
    let outcome = run_with(&options(repo.path(), tree.path()), &host);
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    assert_eq!(outcome.receipt()["census_mismatch"], true);
    assert!(
        outcome
            .reasons()
            .contains(&"ctest printed no parseable summary".to_string())
    );
}

#[test]
fn a_receipt_that_cannot_be_published_turns_a_pass_into_an_invalid_run() {
    let repo = source_repo();
    let tree = labelled_tree();
    // A non-empty directory where the receipt goes cannot be replaced by it.
    std::fs::create_dir_all(tree.path().join("Testing/last-run-receipt.json/occupied")).unwrap();
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    assert!(!outcome.result.receipt_published);
    assert!(outcome.output.contains("Could not publish the run receipt"));
}

// --- Build, freshness and the reusable verdict ------------------------------

#[test]
fn a_failed_build_stops_before_the_suite_runs_and_says_so_in_the_receipt() {
    // A hand-written tree has no build system, so the build fails the way a
    // broken compile does.
    let repo = source_repo();
    let tree = labelled_tree();
    let host = RecordingHost::new();
    let outcome = run_with(
        &Options {
            no_build: false,
            ..options(repo.path(), tree.path())
        },
        &host,
    );
    assert_ne!(outcome.code(), 0);
    let receipt = outcome.receipt();
    assert_eq!(receipt["build_status"], "failed");
    assert_eq!(receipt["build_exit_code"], outcome.code());
    assert_eq!(receipt["reusable"], false);
    assert!(receipt["ctest_exit_code"].is_null(), "the suite ran anyway");
    assert!(!host.timeline().contains(&"acquire device".to_string()));
}

#[test]
fn a_failing_run_replaces_an_older_successful_receipt() {
    let repo = source_repo();
    let (_root, build) = configured_tree("");
    let built = Options {
        no_build: false,
        allow_stale: false,
        ..options(repo.path(), &build)
    };
    let good = run(&built);
    assert_eq!(good.code(), 0, "{}", good.output);
    assert_eq!(
        good.receipt()["reusable"],
        true,
        "a clean, freshly built, undrifted run was not reusable"
    );

    let bad = run(&Options {
        filter: "no.such.test".into(),
        ..built
    });
    assert_ne!(bad.code(), 0);
    assert_ne!(bad.receipt()["run_id"], good.receipt()["run_id"]);
    assert_eq!(bad.receipt()["reusable"], false);
}

#[test]
fn a_tree_whose_freshness_cannot_be_proven_is_refused_without_a_build() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&Options {
        allow_stale: false,
        ..options(repo.path(), tree.path())
    });
    assert_eq!(outcome.code(), 3, "{}", outcome.output);
    assert_eq!(outcome.receipt()["freshness"], "unknown");
    assert!(outcome.receipt()["ctest_exit_code"].is_null());
}

#[test]
fn allow_stale_reports_a_result_and_says_which_binaries_it_describes() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    assert!(
        outcome.output.contains("OLD binaries"),
        "{}",
        outcome.output
    );
    assert_eq!(outcome.receipt()["reusable"], false);
}

#[test]
fn a_ninja_tree_with_nothing_left_to_build_is_fresh_without_a_build() {
    let repo = source_repo();
    let (_root, build) = configured_tree("");
    let built = Options {
        no_build: false,
        allow_stale: false,
        ..options(repo.path(), &build)
    };
    assert_eq!(run(&built).code(), 0);
    let outcome = run(&Options {
        no_build: true,
        ..built
    });
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    assert_eq!(outcome.receipt()["freshness"], "fresh");
    assert_eq!(outcome.receipt()["build_status"], "skipped");
    assert_eq!(outcome.receipt()["reusable"], true);
}

#[test]
fn a_ninja_tree_with_pending_work_is_stale_and_refused_without_a_build() {
    let repo = source_repo();
    // A custom target with no outputs is out of date on every build.
    let (_root, build) =
        configured_tree("add_custom_target(always ALL COMMAND \"${CMAKE_COMMAND}\" -E true)");
    let outcome = run(&Options {
        allow_stale: false,
        ..options(repo.path(), &build)
    });
    assert_eq!(outcome.code(), 3, "{}", outcome.output);
    assert_eq!(outcome.receipt()["freshness"], "stale");
    assert!(outcome.output.contains("STALE"));
    assert!(
        outcome
            .reasons()
            .iter()
            .any(|r| r.starts_with("build tree is stale"))
    );
}

// --- Source identity --------------------------------------------------------

#[test]
fn the_receipt_records_the_source_identity_at_both_ends_of_the_run() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&options(repo.path(), tree.path()));
    let head = exo_dev::git::Git::new(repo.path()).head().unwrap();
    let receipt = outcome.receipt();
    assert_eq!(receipt["source_before"]["head"], head.as_str());
    assert_eq!(receipt["source_before"]["ok"], true);
    assert_eq!(receipt["source_after"]["ok"], true);
    assert_eq!(
        receipt["source_before"]["fingerprint"],
        receipt["source_after"]["fingerprint"]
    );
    assert_eq!(receipt["source_drift"], false);
    assert_eq!(receipt["tests_registered"], 3);
    assert_eq!(receipt["census_mismatch"], false);
}

#[test]
fn a_working_tree_that_changes_during_the_run_invalidates_the_result() {
    let repo = source_repo();
    let scripts = tempfile::tempdir().unwrap();
    let appeared = repo.path().join("appeared-during-the-run.txt");
    let mutator = script(
        scripts.path(),
        "mutate.cmake",
        "file(WRITE \"${TARGET}\" \"new source\")\n",
    );
    let tree = hand_written_tree(&scripted(
        "fixture.mutates_the_tree",
        "phase.hermetic",
        &mutator,
        &[("TARGET", &appeared)],
    ));
    let outcome = run(&options(repo.path(), tree.path()));
    assert!(appeared.is_file(), "the fixture test did not run");
    assert_eq!(outcome.receipt()["source_drift"], true);
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    assert_eq!(outcome.receipt()["reusable"], false);
    assert!(
        outcome
            .reasons()
            .iter()
            .any(|r| r.contains("changed while"))
    );
}

#[test]
fn a_source_that_cannot_be_identified_is_not_a_valid_run() {
    let not_a_repo = tempfile::tempdir().unwrap();
    std::fs::write(not_a_repo.path().join(".qt-version"), "6.11.2\n").unwrap();
    let tree = labelled_tree();
    let outcome = run(&options(not_a_repo.path(), tree.path()));
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    assert_eq!(outcome.receipt()["source_before"]["ok"], false);
    assert!(outcome.receipt()["source_after"].is_null());
    assert!(
        outcome
            .reasons()
            .iter()
            .any(|r| r.starts_with("source identity unavailable"))
    );
}

// --- Locks ------------------------------------------------------------------

#[test]
fn the_locks_are_taken_tree_build_device_and_each_only_for_its_span() {
    let repo = source_repo();
    let (_root, build) = configured_tree("");
    let host = RecordingHost::new();
    let outcome = run_with(
        &Options {
            no_build: false,
            allow_stale: false,
            ..options(repo.path(), &build)
        },
        &host,
    );
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    // The MSVC import happens under the tree lock and before the build lock,
    // or not at all when this shell already has a compiler.
    let mut timeline = host.timeline();
    if let Some(index) = timeline.iter().position(|e| e == "msvc") {
        assert_eq!(index, 1, "{timeline:?}");
        timeline.remove(index);
    }
    assert_eq!(
        timeline,
        [
            "acquire tree",
            "acquire build",
            "BeforeBuild",
            "AfterBuild",
            "release build",
            "acquire device",
            "BeforeSuite",
            "AfterSuiteBeforeVerdict",
            "release device",
            "BeforeReceipt",
            "release tree",
        ]
    );
}

#[test]
fn the_build_runs_without_the_device_lock_and_the_suite_without_the_build_lock() {
    // Read from the children themselves: each dumps the environment it was
    // started with, and a held lock is visible there as its inherit mark.
    let repo = source_repo();
    let dumps = tempfile::tempdir().unwrap();
    let dump = env_dump_script(dumps.path());
    let build_env = dumps.path().join("build.env");
    let suite_env = dumps.path().join("suite.env");
    let (_root, build) = configured_tree(&format!(
        "{}\n\
         add_test(NAME fixture.dump COMMAND \"${{CMAKE_COMMAND}}\" \"-DOUT={}\" -P \"{}\")\n\
         set_tests_properties(fixture.dump PROPERTIES LABELS \"phase.hermetic\")",
        dump_at_build(&build_env, &dump),
        cmake_path(&suite_env),
        cmake_path(&dump),
    ));
    let host = RecordingHost::new();
    let outcome = run_with(
        &Options {
            no_build: false,
            allow_stale: false,
            ..options(repo.path(), &build)
        },
        &host,
    );
    assert_eq!(outcome.code(), 0, "{}", outcome.output);

    let tree_mark = host_lock::inherit_variable(LockKind::Tree, Some(&build)).to_uppercase();
    let build_mark = "EXOSNAP_HOST_LOCK_BUILD";
    let device_mark = "EXOSNAP_HOST_LOCK_DEVICE";
    let upper = |map: std::collections::BTreeMap<String, String>| -> Vec<String> {
        map.into_keys().map(|k| k.to_uppercase()).collect()
    };
    let during_build = upper(read_env_dump(&build_env));
    assert!(
        during_build.contains(&tree_mark),
        "the build ran outside the tree hold"
    );
    assert!(during_build.contains(&build_mark.to_string()));
    assert!(
        !during_build.contains(&device_mark.to_string()),
        "the device lock was held during the build"
    );
    let during_suite = upper(read_env_dump(&suite_env));
    assert!(
        during_suite.contains(&tree_mark),
        "the suite ran outside the tree hold"
    );
    assert!(during_suite.contains(&device_mark.to_string()));
    assert!(
        !during_suite.contains(&build_mark.to_string()),
        "the build lock was held during the suite"
    );
}

/// A target of the default build that writes the build's environment to
/// `out`.
fn dump_at_build(out: &Path, dump: &Path) -> String {
    format!(
        "add_custom_target(dump ALL COMMAND \"${{CMAKE_COMMAND}}\" \"-DOUT={}\" -P \"{}\" VERBATIM)",
        cmake_path(out),
        cmake_path(dump)
    )
}

#[test]
fn a_second_run_does_not_touch_a_build_tree_another_run_is_holding() {
    let repo = source_repo();
    let tree = labelled_tree();
    let mut host = RecordingHost::new();
    host.contended = vec![(LockKind::Tree, Some(tree.path().to_path_buf()))];
    let outcome = run_with(
        &Options {
            no_build: false,
            ..options(repo.path(), tree.path())
        },
        &host,
    );
    assert_eq!(outcome.code(), 4);
    assert!(
        outcome.output.contains("held by another run"),
        "{}",
        outcome.output
    );
    assert!(!outcome.output.contains("Building all targets"));
    assert!(outcome.receipt.is_none());
    assert!(outcome.result.receipt.is_none());
    assert_eq!(host.timeline(), ["refuse tree"]);
}

#[test]
fn an_unrelated_build_tree_is_not_blocked_by_a_hold_on_another() {
    let repo = source_repo();
    let held = labelled_tree();
    let free = labelled_tree();
    let mut host = RecordingHost::new();
    host.contended = vec![(LockKind::Tree, Some(held.path().to_path_buf()))];
    let outcome = run_with(&options(repo.path(), free.path()), &host);
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
}

#[test]
fn the_tree_lock_is_held_while_the_suite_runs_and_released_afterwards() {
    let repo = source_repo();
    let tree = labelled_tree();
    let host = RecordingHost::new().on_checkpoint(|point, host| {
        if point == Checkpoint::BeforeSuite {
            anyhow::ensure!(
                host.held() == [LockKind::Tree, LockKind::Device],
                "held during the suite: {:?}",
                host.held()
            );
        }
        Ok(())
    });
    let outcome = run_with(&options(repo.path(), tree.path()), &host);
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    assert!(host.held().is_empty(), "a lock outlived the run");
}

#[test]
fn the_tree_lock_is_still_held_while_the_receipt_is_being_written() {
    let repo = source_repo();
    let tree = labelled_tree();
    let receipt_path = tree.path().join("Testing/last-run-receipt.json");
    let expected_receipt = receipt_path.clone();
    let host = RecordingHost::new().on_checkpoint(move |point, host| {
        if point == Checkpoint::BeforeReceipt {
            anyhow::ensure!(
                !expected_receipt.exists(),
                "the checkpoint is after the publish, so this case proves nothing"
            );
            anyhow::ensure!(
                host.held() == [LockKind::Tree],
                "held while publishing: {:?}",
                host.held()
            );
        }
        Ok(())
    });
    let outcome = run_with(&options(repo.path(), tree.path()), &host);
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    assert!(receipt_path.is_file());
    assert!(host.held().is_empty());
}

#[test]
fn a_run_that_never_got_the_tree_lock_leaves_the_holders_receipt_alone() {
    let repo = source_repo();
    let tree = labelled_tree();
    let receipt_path = tree.path().join("Testing/last-run-receipt.json");
    std::fs::create_dir_all(receipt_path.parent().unwrap()).unwrap();
    let sentinel = r#"{"exit_code":0,"reusable":true,"owner":"the run that holds the tree"}"#;
    std::fs::write(&receipt_path, sentinel).unwrap();
    let mut host = RecordingHost::new();
    host.contended = vec![(LockKind::Tree, None)];
    let outcome = run_with(&options(repo.path(), tree.path()), &host);
    assert_eq!(outcome.code(), 4);
    assert_eq!(std::fs::read_to_string(&receipt_path).unwrap(), sentinel);
    assert!(outcome.output.contains("No receipt was written"));
}

#[test]
fn a_lock_that_cannot_be_taken_mid_run_aborts_with_a_receipt() {
    let repo = source_repo();
    let tree = labelled_tree();
    let mut host = RecordingHost::new();
    host.contended = vec![(LockKind::Device, None)];
    let outcome = run_with(&options(repo.path(), tree.path()), &host);
    assert_eq!(outcome.code(), 4);
    assert!(
        outcome
            .reasons()
            .iter()
            .any(|r| r.starts_with("run aborted"))
    );
    assert!(host.held().is_empty());
}

#[test]
fn a_delegated_child_runs_under_the_parent_hold() {
    // Without the inherit marks a test that starts another run on this tree
    // or this device would wait out the deadline on its own parent.
    let repo = source_repo();
    let dumps = tempfile::tempdir().unwrap();
    let dump = env_dump_script(dumps.path());
    let seen = dumps.path().join("child.env");
    let tree = hand_written_tree(&scripted(
        "fixture.delegates",
        "phase.hermetic",
        &dump,
        &[("OUT", &seen)],
    ));
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    let env = read_env_dump(&seen);
    let key = |name: String| {
        if cfg!(windows) {
            name.to_uppercase()
        } else {
            name
        }
    };
    assert!(env.contains_key(&key(host_lock::inherit_variable(
        LockKind::Tree,
        Some(tree.path())
    ))));
    assert!(env.contains_key(&key("EXOSNAP_HOST_LOCK_DEVICE".into())));
}

#[test]
fn the_locks_are_released_after_a_build_failure_and_after_a_test_failure() {
    let repo = source_repo();
    let tree = labelled_tree();
    let host = RecordingHost::new();
    let built = run_with(
        &Options {
            no_build: false,
            ..options(repo.path(), tree.path())
        },
        &host,
    );
    assert_ne!(built.code(), 0);
    assert!(
        host.held().is_empty(),
        "a failed build kept {:?}",
        host.held()
    );

    let failing_tree = hand_written_tree(&failing("fixture.fails", "phase.hermetic"));
    let host = RecordingHost::new();
    let outcome = run_with(&options(repo.path(), failing_tree.path()), &host);
    assert_ne!(outcome.code(), 0);
    assert!(
        host.held().is_empty(),
        "a failed suite kept {:?}",
        host.held()
    );
}

// --- Evidence ---------------------------------------------------------------

/// A test that writes `app.log` into the throwaway configuration directory,
/// the way the QML and cursor-audit suites write their logs, and passes or
/// fails as told.
fn log_writing_tree(scripts: &Path, fail: bool) -> tempfile::TempDir {
    let body = format!(
        "string(RANDOM LENGTH 16 nonce)\n\
         file(WRITE \"$ENV{{EXOSNAP_CONFIG_DIR}}/app.log\" \"diagnostic ${{nonce}}\")\n\
         file(WRITE \"${{WHERE}}\" \"$ENV{{EXOSNAP_CONFIG_DIR}}\")\n{}",
        if fail {
            "message(FATAL_ERROR \"failing on purpose\")\n"
        } else {
            ""
        }
    );
    let writer = script(scripts, "writes-a-log.cmake", &body);
    hand_written_tree(&scripted(
        "fixture.writes_a_log",
        "phase.hermetic",
        &writer,
        &[("WHERE", &scripts.join("config-dir.txt"))],
    ))
}

fn config_dir_seen(scripts: &Path) -> PathBuf {
    PathBuf::from(std::fs::read_to_string(scripts.join("config-dir.txt")).unwrap())
}

#[test]
fn a_run_that_aborts_after_the_suite_still_secures_what_the_tests_wrote() {
    // The fixture test passes on purpose: the rescue must be driven by the
    // missing verdict alone, not by a failing one.
    let repo = source_repo();
    let scripts = tempfile::tempdir().unwrap();
    let tree = log_writing_tree(scripts.path(), false);
    let mut host = RecordingHost::new();
    host.fault_at = Some(Checkpoint::AfterSuiteBeforeVerdict);
    let outcome = run_with(&options(repo.path(), tree.path()), &host);
    assert_eq!(outcome.code(), 4, "{}", outcome.output);
    let receipt = outcome.receipt();
    assert_eq!(receipt["rescue_status"], "rescued");
    let rescued = PathBuf::from(receipt["rescued_config_dir"].as_str().unwrap());
    assert!(rescued.join("app.log").is_file());
    assert!(receipt["ctest_exit_code"].is_null());
    assert!(outcome.reasons().iter().any(|r| r.contains("aborted")));
}

#[test]
fn what_a_failing_test_wrote_is_kept_under_a_path_of_its_own_per_run() {
    let repo = source_repo();
    let scripts = tempfile::tempdir().unwrap();
    let tree = log_writing_tree(scripts.path(), true);

    let first = run(&options(repo.path(), tree.path()));
    assert_ne!(first.code(), 0);
    assert_eq!(first.receipt()["rescue_status"], "rescued");
    let first_rescue = PathBuf::from(first.receipt()["rescued_config_dir"].as_str().unwrap());
    let first_content = std::fs::read_to_string(first_rescue.join("app.log")).unwrap();
    assert!(
        !config_dir_seen(scripts.path()).exists(),
        "the throwaway config dir survived a rescued run"
    );

    let second = run(&options(repo.path(), tree.path()));
    let second_rescue = PathBuf::from(second.receipt()["rescued_config_dir"].as_str().unwrap());
    assert_ne!(second_rescue, first_rescue);
    assert_eq!(
        std::fs::read_to_string(first_rescue.join("app.log")).unwrap(),
        first_content,
        "the second run overwrote the first run's evidence"
    );
}

#[test]
fn a_rescue_that_cannot_complete_keeps_the_original_and_reports_the_failure() {
    let repo = source_repo();
    let scripts = tempfile::tempdir().unwrap();
    let tree = log_writing_tree(scripts.path(), true);
    std::fs::create_dir_all(tree.path().join("Testing")).unwrap();
    std::fs::write(
        tree.path().join("Testing/last-run-config-dir"),
        "not a directory",
    )
    .unwrap();
    let outcome = run(&options(repo.path(), tree.path()));
    let receipt = outcome.receipt();
    assert_eq!(receipt["rescue_status"], "failed");
    let kept = PathBuf::from(receipt["rescued_config_dir"].as_str().unwrap());
    assert_eq!(kept, config_dir_seen(scripts.path()));
    assert!(kept.join("app.log").is_file(), "the only copy was deleted");
    assert!(outcome.output.contains("original directory is kept"));
    assert!(outcome.reasons().iter().any(|r| r.contains("evidence")));
    std::fs::remove_dir_all(kept).unwrap();
}

#[test]
fn a_passing_run_removes_its_throwaway_config_dir() {
    let repo = source_repo();
    let scripts = tempfile::tempdir().unwrap();
    let tree = log_writing_tree(scripts.path(), false);
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    assert_eq!(outcome.receipt()["rescue_status"], "not-needed");
    assert!(!config_dir_seen(scripts.path()).exists());
}

#[test]
fn a_failing_test_is_named_in_the_summary_even_though_it_carries_labels() {
    let repo = source_repo();
    let tree = hand_written_tree(&failing("fixture.labelled_failure", "phase.hermetic;quick"));
    let outcome = run(&options(repo.path(), tree.path()));
    assert_ne!(outcome.code(), 0);
    assert!(
        outcome.output.contains("Failed test binaries"),
        "{}",
        outcome.output
    );
    assert!(outcome.output.contains("  fixture.labelled_failure"));
    assert_eq!(outcome.result.failed_tests, ["fixture.labelled_failure"]);
    // The line evidence::failed_ctest_names follows to the full log.
    assert!(
        outcome
            .output
            .contains(&format!("Full log: {}", outcome.result.log.display()))
    );
}

#[test]
fn a_failing_gtest_case_is_shown_with_its_assertion() {
    let repo = source_repo();
    let scripts = tempfile::tempdir().unwrap();
    let gtest = script(
        scripts.path(),
        "gtest.cmake",
        "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E echo \"[ RUN      ] Suite.Case\")\n\
         execute_process(COMMAND \"${CMAKE_COMMAND}\" -E echo \"value.cpp(12): error: Expected equality\")\n\
         execute_process(COMMAND \"${CMAKE_COMMAND}\" -E echo \"[  FAILED  ] Suite.Case (0 ms)\")\n\
         message(FATAL_ERROR \"failing on purpose\")\n",
    );
    let tree = hand_written_tree(&scripted("fixture.gtest", "phase.hermetic", &gtest, &[]));
    let outcome = run(&options(repo.path(), tree.path()));
    assert_ne!(outcome.code(), 0);
    assert!(
        outcome.output.contains("Failing gtest cases:"),
        "{}",
        outcome.output
    );
    assert!(outcome.output.contains("--- Suite.Case"));
    assert!(
        outcome
            .output
            .contains("  value.cpp(12): error: Expected equality")
    );
}

// --- Environment ------------------------------------------------------------

fn suite_environment(
    host: &RecordingHost,
) -> (Outcome, std::collections::BTreeMap<String, String>) {
    let repo = source_repo();
    let dumps = tempfile::tempdir().unwrap();
    let dump = env_dump_script(dumps.path());
    let seen = dumps.path().join("suite.env");
    let tree = hand_written_tree(&scripted(
        "fixture.environment",
        "phase.hermetic",
        &dump,
        &[("OUT", &seen)],
    ));
    let outcome = run_with(&options(repo.path(), tree.path()), host);
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    let env = read_env_dump(&seen);
    (outcome, env)
}

fn get<'a>(env: &'a std::collections::BTreeMap<String, String>, name: &str) -> Option<&'a str> {
    let key = if cfg!(windows) {
        name.to_uppercase()
    } else {
        name.to_string()
    };
    env.get(&key).map(String::as_str)
}

#[test]
fn the_suite_runs_offscreen_in_a_throwaway_config_dir_with_this_executable_as_its_tool() {
    let (_, env) = suite_environment(&RecordingHost::new());
    assert_eq!(get(&env, "QT_QPA_PLATFORM"), Some("offscreen"));
    let config_dir = get(&env, "EXOSNAP_CONFIG_DIR").expect("no EXOSNAP_CONFIG_DIR");
    assert!(config_dir.contains("exosnap_runtests_"), "{config_dir}");
    assert!(
        !Path::new(config_dir).exists(),
        "the throwaway config dir outlived the run"
    );
    assert_eq!(
        get(&env, runner::TOOL_EXE_VARIABLE).map(PathBuf::from),
        Some(std::env::current_exe().unwrap())
    );
}

#[test]
fn only_the_suite_gets_the_tool_executable() {
    let repo = source_repo();
    let dumps = tempfile::tempdir().unwrap();
    let dump = env_dump_script(dumps.path());
    let build_env = dumps.path().join("build.env");
    let (_root, build) = configured_tree(&dump_at_build(&build_env, &dump));
    let outcome = run(&Options {
        no_build: false,
        allow_stale: false,
        ..options(repo.path(), &build)
    });
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    assert_eq!(
        get(&read_env_dump(&build_env), runner::TOOL_EXE_VARIABLE),
        None
    );
}

#[test]
fn the_canonical_qt_install_is_put_in_front_of_the_suite() {
    let qt = tempfile::tempdir().unwrap();
    let root = qt.path().join("6.11.2").join("msvc2022_64");
    std::fs::create_dir_all(root.join("bin")).unwrap();
    std::fs::create_dir_all(root.join("plugins")).unwrap();
    let mut host = RecordingHost::new();
    host.vars
        .insert("EXOSNAP_QT_ROOT".into(), qt.path().as_os_str().to_owned());
    let (outcome, env) = suite_environment(&host);
    let path = get(&env, "PATH").unwrap();
    assert!(
        path.starts_with(&root.join("bin").display().to_string()),
        "{path}"
    );
    assert_eq!(
        get(&env, "QT_PLUGIN_PATH").map(PathBuf::from),
        Some(root.join("plugins"))
    );
    assert!(!outcome.output.contains("was not found"));
}

#[test]
fn a_missing_qt_install_is_a_warning_not_an_error() {
    let (outcome, env) = suite_environment(&RecordingHost::new());
    assert!(
        outcome
            .output
            .contains("Qt 6.11.2 was not found under the expected install root")
    );
    assert_eq!(
        get(&env, "QT_PLUGIN_PATH").map(str::to_string),
        std::env::var("QT_PLUGIN_PATH").ok()
    );
}

#[test]
fn a_repository_without_a_qt_version_does_not_start() {
    let repo = exo_dev::test_support::fixture_repo_committed(&[("README.md", "x\n")]);
    let tree = labelled_tree();
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 4);
    assert!(
        outcome.output.contains("did not start"),
        "{}",
        outcome.output
    );
    assert!(outcome.output.contains(".qt-version is missing"));
    assert!(outcome.receipt.is_none());
}

#[test]
fn a_ninja_tree_gets_the_msvc_environment_for_its_build() {
    let repo = source_repo();
    let dumps = tempfile::tempdir().unwrap();
    let dump = env_dump_script(dumps.path());
    let build_env = dumps.path().join("build.env");
    let (_root, build) = configured_tree(&dump_at_build(&build_env, &dump));
    let mut host = RecordingHost::new();
    host.msvc = Some(vec![("EXO_DEV_FAKE_VCVARS".into(), "imported".into())]);
    let outcome = run_with(
        &Options {
            no_build: false,
            allow_stale: false,
            ..options(repo.path(), &build)
        },
        &host,
    );
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    let compiler_on_path =
        exo_dev::msvc::find_compiler(&std::env::var("PATH").unwrap_or_default()).is_some();
    if compiler_on_path {
        // A developer shell: its own compiler environment is never replaced.
        assert!(!host.timeline().contains(&"msvc".to_string()));
    } else {
        assert_eq!(
            get(&read_env_dump(&build_env), "EXO_DEV_FAKE_VCVARS"),
            Some("imported")
        );
    }
}

#[test]
fn a_compiler_the_caller_already_provides_is_not_imported_again() {
    // The verify path: an earlier step imported the MSVC environment and
    // hands it over, and the run builds with that rather than importing again.
    let repo = source_repo();
    let compiler = tempfile::tempdir().unwrap();
    std::fs::write(compiler.path().join("cl.exe"), "").unwrap();
    let mut path = compiler.path().as_os_str().to_owned();
    path.push(if cfg!(windows) { ";" } else { ":" });
    path.push(std::env::var_os("PATH").unwrap_or_default());
    let (_root, build) = configured_tree("");
    let host = RecordingHost::new();
    let outcome = run_with(
        &Options {
            no_build: false,
            allow_stale: false,
            child_env: vec![("PATH".into(), path)],
            ..options(repo.path(), &build)
        },
        &host,
    );
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    assert!(!host.timeline().contains(&"msvc".to_string()));
}

#[test]
fn an_msvc_import_that_fails_aborts_the_run_with_a_receipt() {
    let repo = source_repo();
    let (_root, build) = configured_tree("");
    let mut host = RecordingHost::new();
    host.msvc_error = Some("No Visual Studio installation with the x64 C++ toolset".into());
    let outcome = run_with(
        &Options {
            no_build: false,
            ..options(repo.path(), &build)
        },
        &host,
    );
    assert_eq!(outcome.code(), 4);
    assert!(
        outcome
            .reasons()
            .iter()
            .any(|r| r.contains("No Visual Studio installation"))
    );
    assert_eq!(outcome.receipt()["build_status"], "not-started");
}

#[test]
fn the_process_environment_is_left_exactly_as_it_was() {
    // Every variable a run gives its children is set on their commands, never
    // on this process: a caller that survives the run keeps its environment,
    // and a variable that was unset stays unset rather than becoming empty.
    let watched = [
        "PATH",
        "QT_QPA_PLATFORM",
        "QT_PLUGIN_PATH",
        "EXOSNAP_CONFIG_DIR",
        "EXOSNAP_TEST_TOOL_EXE",
        "EXOSNAP_HOST_LOCK_DEVICE",
        "EXOSNAP_HOST_LOCK_BUILD",
    ];
    let before: Vec<_> = watched.iter().map(std::env::var_os).collect();
    let qt = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(qt.path().join("6.11.2/msvc2022_64/plugins")).unwrap();
    let mut host = RecordingHost::new();
    host.vars
        .insert("EXOSNAP_QT_ROOT".into(), qt.path().as_os_str().to_owned());
    suite_environment(&host);
    let after: Vec<_> = watched.iter().map(std::env::var_os).collect();
    assert_eq!(before, after);
}

// --- Exit codes -------------------------------------------------------------

#[test]
fn exit_0_is_a_valid_passing_run() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 0, "{}", outcome.output);
    assert_eq!(outcome.receipt()["exit_code"], 0);
    assert_eq!(outcome.receipt()["ctest_exit_code"], 0);
}

#[test]
fn exit_2_is_a_build_directory_that_does_not_exist() {
    let repo = source_repo();
    let missing = repo.path().join("no-such-build");
    let host = RecordingHost::new();
    let outcome = run_with(&options(repo.path(), &missing), &host);
    assert_eq!(outcome.code(), 2);
    assert!(outcome.output.contains("does not exist"));
    assert!(outcome.receipt.is_none());
    assert!(host.timeline().is_empty(), "a lock was taken for no tree");
}

#[test]
fn exit_3_is_a_tree_not_proven_to_match_the_source() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&Options {
        allow_stale: false,
        ..options(repo.path(), tree.path())
    });
    assert_eq!(outcome.code(), 3);
    assert_eq!(outcome.receipt()["exit_code"], 3);
    assert_eq!(outcome.receipt()["reusable"], false);
}

#[test]
fn exit_4_is_a_run_that_is_not_a_valid_verification() {
    let repo = source_repo();
    let tree = hand_written_tree(&passing("fixture.no_phase", ""));
    let outcome = run(&options(repo.path(), tree.path()));
    assert_eq!(outcome.code(), 4);
    assert_eq!(outcome.receipt()["exit_code"], 4);
}

#[test]
fn any_other_code_is_the_suites_own() {
    let repo = source_repo();
    let tree = hand_written_tree(&failing("fixture.fails", "phase.hermetic"));
    let outcome = run(&options(repo.path(), tree.path()));
    let ctest = outcome.receipt()["ctest_exit_code"].as_i64().unwrap();
    assert_ne!(ctest, 0);
    assert_eq!(i64::from(outcome.code()), ctest);
    assert_eq!(outcome.receipt()["exit_code"], ctest);
    assert_eq!(outcome.receipt()["tests_failed"], 1);
}

#[test]
fn the_receipt_has_exactly_the_documented_fields_in_order() {
    let repo = source_repo();
    let tree = labelled_tree();
    let outcome = run(&options(repo.path(), tree.path()));
    let fields: Vec<&str> = outcome
        .receipt()
        .as_object()
        .unwrap()
        .keys()
        .map(String::as_str)
        .collect();
    assert_eq!(
        fields,
        [
            "run_id",
            "started_utc",
            "finished_utc",
            "build_dir",
            "generator",
            "config",
            "jobs",
            "freshness",
            "freshness_detail",
            "allow_stale",
            "no_build",
            "build_status",
            "build_exit_code",
            "ctest_args",
            "exclude_label",
            "exclude_pattern",
            "phase",
            "filter",
            "tests_registered",
            "tests_disabled",
            "tests_selected",
            "tests_accounted",
            "tests_expected",
            "tests_passed",
            "tests_failed",
            "census_mismatch",
            "phase_violations",
            "ctest_exit_code",
            "source_before",
            "source_after",
            "source_drift",
            "log",
            "rescued_config_dir",
            "rescue_status",
            "rescue_detail",
            "invalid_reasons",
            "exit_code",
            "reusable",
        ]
    );
    let source: Vec<&str> = outcome.receipt()["source_before"]
        .as_object()
        .unwrap()
        .keys()
        .map(String::as_str)
        .collect();
    assert_eq!(
        source,
        [
            "ok",
            "reason",
            "head",
            "dirty",
            "fingerprint",
            "untracked_files",
            "untracked_bytes"
        ]
    );
    assert_eq!(
        strings(&outcome.receipt()["ctest_args"])[..2],
        ["--test-dir".to_string(), tree.path().display().to_string()]
    );
}

// --- The binary -------------------------------------------------------------

#[test]
fn the_binary_refuses_a_phase_the_vocabulary_does_not_know() {
    let output = std::process::Command::new(env!("CARGO_BIN_EXE_exo-dev"))
        .args(["test", "--phase", "gpu-ish", "--build-dir", "."])
        .output()
        .unwrap();
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("gpu-ish"));
}

#[test]
fn the_binary_refuses_allow_stale_without_no_build() {
    let output = std::process::Command::new(env!("CARGO_BIN_EXE_exo-dev"))
        .args(["test", "--allow-stale"])
        .output()
        .unwrap();
    assert!(!output.status.success());
}

#[test]
fn the_binary_exits_2_for_a_build_directory_that_does_not_exist() {
    let missing = tempfile::tempdir().unwrap().path().join("gone");
    let output = std::process::Command::new(env!("CARGO_BIN_EXE_exo-dev"))
        .arg("test")
        .arg("--build-dir")
        .arg(&missing)
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&output.stdout).contains("does not exist"));
}

// --- Real named mutexes -----------------------------------------------------

#[cfg(windows)]
mod windows {
    use std::time::Duration;

    use exo_dev::host_lock::{LockKind, LockSpace};

    use super::*;

    fn private_space() -> LockSpace {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        LockSpace {
            namespace: Some(format!("exo-dev-contract-{}-{nanos}", std::process::id())),
            timeout: Some(Duration::from_secs(2)),
        }
    }

    /// Whether another thread can take `kind` right now. A mutex is recursive
    /// for the thread that owns it, so the question is never asked on the
    /// thread running the run.
    fn free_elsewhere(space: &LockSpace, kind: LockKind, tree: Option<&Path>) -> bool {
        let space = LockSpace {
            timeout: Some(Duration::from_millis(50)),
            ..space.clone()
        };
        let tree = tree.map(Path::to_path_buf);
        std::thread::spawn(move || space.acquire(kind, tree.as_deref(), "probe").is_ok())
            .join()
            .unwrap()
    }

    fn real_host(space: &LockSpace) -> RecordingHost {
        let mut host = RecordingHost::new();
        host.real_locks = Some(space.clone());
        host
    }

    #[test]
    fn a_second_run_times_out_on_a_tree_another_holder_has() {
        let space = private_space();
        let repo = source_repo();
        let tree = labelled_tree();
        let path = tree.path().to_path_buf();
        let (hold, release) = std::sync::mpsc::channel::<()>();
        let (held, wait_held) = std::sync::mpsc::channel::<()>();
        let holder = std::thread::spawn({
            let space = space.clone();
            move || {
                let _lock = space
                    .acquire(LockKind::Tree, Some(&path), "foreign")
                    .unwrap();
                held.send(()).unwrap();
                let _ = release.recv();
            }
        });
        wait_held.recv().unwrap();
        let outcome = run_with(
            &Options {
                no_build: false,
                ..options(repo.path(), tree.path())
            },
            &real_host(&space),
        );
        hold.send(()).unwrap();
        holder.join().unwrap();
        assert_eq!(outcome.code(), 4);
        assert!(
            outcome.output.contains("held by another run"),
            "{}",
            outcome.output
        );
        assert!(!outcome.output.contains("Building all targets"));
    }

    #[test]
    fn an_unrelated_tree_is_not_blocked_by_a_real_hold_on_another() {
        let space = private_space();
        let repo = source_repo();
        let held_tree = labelled_tree();
        let free_tree = labelled_tree();
        let _hold = space
            .acquire(LockKind::Tree, Some(held_tree.path()), "foreign")
            .unwrap();
        // Same thread holds the other tree: a different mutex, so recursion
        // plays no part in the answer.
        let outcome = run_with(&options(repo.path(), free_tree.path()), &real_host(&space));
        assert_eq!(outcome.code(), 0, "{}", outcome.output);
    }

    #[test]
    fn the_real_tree_lock_spans_the_suite_and_the_receipt_and_is_released() {
        let space = private_space();
        let repo = source_repo();
        let tree = labelled_tree();
        let path = tree.path().to_path_buf();
        let probe_space = space.clone();
        let host = real_host(&space).on_checkpoint(move |point, _| {
            if matches!(point, Checkpoint::BeforeSuite | Checkpoint::BeforeReceipt) {
                anyhow::ensure!(
                    !free_elsewhere(&probe_space, LockKind::Tree, Some(&path)),
                    "the tree was free for another run at {point:?}"
                );
            }
            if point == Checkpoint::BeforeSuite {
                anyhow::ensure!(
                    !free_elsewhere(&probe_space, LockKind::Device, None),
                    "the device was free for another run during the suite"
                );
            }
            if point == Checkpoint::BeforeReceipt {
                anyhow::ensure!(
                    free_elsewhere(&probe_space, LockKind::Device, None),
                    "the device was still held after the suite"
                );
            }
            Ok(())
        });
        let outcome = run_with(&options(repo.path(), tree.path()), &host);
        assert_eq!(outcome.code(), 0, "{}", outcome.output);
        assert!(free_elsewhere(&space, LockKind::Tree, Some(tree.path())));
        assert!(free_elsewhere(&space, LockKind::Device, None));
    }

    #[test]
    fn the_real_locks_are_released_after_a_failed_build_and_a_failed_suite() {
        let space = private_space();
        let repo = source_repo();
        let tree = labelled_tree();
        let built = run_with(
            &Options {
                no_build: false,
                ..options(repo.path(), tree.path())
            },
            &real_host(&space),
        );
        assert_ne!(built.code(), 0);
        assert!(free_elsewhere(&space, LockKind::Tree, Some(tree.path())));
        assert!(free_elsewhere(&space, LockKind::Build, None));

        let failing_tree = hand_written_tree(&failing("fixture.fails", "phase.hermetic"));
        let outcome = run_with(
            &options(repo.path(), failing_tree.path()),
            &real_host(&space),
        );
        assert_ne!(outcome.code(), 0);
        assert!(free_elsewhere(
            &space,
            LockKind::Tree,
            Some(failing_tree.path())
        ));
        assert!(free_elsewhere(&space, LockKind::Device, None));
    }

    /// Run by the delegation case as a child of ctest: takes the tree lock the
    /// parent run holds and reports whether it ran under the parent's hold.
    #[test]
    #[ignore = "a child probe, started by the delegation case"]
    fn delegated_child_probe() {
        let tree = std::env::var_os("EXO_DEV_PROBE_TREE").expect("no EXO_DEV_PROBE_TREE");
        let space = LockSpace {
            namespace: std::env::var("EXOSNAP_HOST_LOCK_NAMESPACE").ok(),
            timeout: Some(Duration::from_secs(3)),
        };
        let inherited =
            exo_dev::host_lock::inherit_variable(LockKind::Tree, Some(Path::new(&tree)));
        assert!(
            std::env::var_os(&inherited).is_some(),
            "no inherit mark {inherited}"
        );
        space
            .acquire(LockKind::Tree, Some(Path::new(&tree)), "delegated child")
            .expect("the child waited on its own parent's tree lock");
        space
            .acquire(LockKind::Device, None, "delegated child")
            .expect("the child waited on its own parent's device lock");
    }

    #[test]
    fn a_delegated_child_takes_the_real_locks_under_the_parent_hold() {
        let space = private_space();
        let repo = source_repo();
        let tree = tempfile::tempdir().unwrap();
        let exe = cmake_path(&std::env::current_exe().unwrap());
        // The probe reads the tree from the environment. ctest's ENVIRONMENT
        // property sets it for that test alone.
        std::fs::write(
            tree.path().join("CTestTestfile.cmake"),
            format!(
                "add_test(fixture.delegates \"{exe}\" \"windows::delegated_child_probe\" \"--exact\" \"--ignored\")\n\
                 set_tests_properties(fixture.delegates PROPERTIES LABELS \"phase.hermetic\" ENVIRONMENT \"EXO_DEV_PROBE_TREE={}\")\n",
                cmake_path(tree.path())
            ),
        )
        .unwrap();
        let outcome = run_with(&options(repo.path(), tree.path()), &real_host(&space));
        assert_eq!(outcome.code(), 0, "{}", outcome.output);
    }
}
