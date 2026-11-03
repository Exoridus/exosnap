use std::io::Write;
use std::path::{Path, PathBuf};

use anyhow::{Context as _, ensure};
use serde::Deserialize;
use serde_json::{Value, json};

use super::{GhRunner, REPO, git_line, valid_sha};
use crate::git::Git;
use crate::host_lock::{self, LockKind};

pub struct UpdateRequest {
    pub number: u32,
    pub confirm: bool,
    pub expected_head: Option<String>,
    pub expected_base: Option<String>,
    /// Allows an acceptance update while a PR is draft or checks are active or unknown.
    pub acceptance_update: bool,
}

#[derive(Debug, Deserialize)]
pub struct UpdateSnapshot {
    pub number: u32,
    pub head: String,
    pub base: String,
    pub base_ref: String,
    pub behind: u64,
}

#[derive(Debug)]
pub enum UpdateOutcome {
    Preview {
        snapshot: UpdateSnapshot,
    },
    UpToDate {
        snapshot: UpdateSnapshot,
    },
    Accepted {
        snapshot: UpdateSnapshot,
        already_pending: bool,
    },
}

#[derive(Deserialize, PartialEq, Eq)]
struct Repository {
    full_name: String,
}

#[derive(Deserialize, PartialEq, Eq)]
struct Tip {
    #[serde(rename = "ref")]
    reference: String,
    sha: String,
    repo: Repository,
}

#[derive(Deserialize, PartialEq, Eq)]
struct Pull {
    number: u32,
    state: String,
    draft: bool,
    mergeable: Option<bool>,
    head: Tip,
    base: Tip,
}

fn api(gh: &dyn GhRunner, endpoint: &str) -> anyhow::Result<Value> {
    serde_json::from_str(&gh.run(&["api", endpoint])?).context("invalid GitHub API JSON")
}

fn current_ref(gh: &dyn GhRunner, reference: &str) -> anyhow::Result<String> {
    let encoded: String = reference
        .bytes()
        .map(|byte| {
            if byte.is_ascii_alphanumeric() || b"-._~/".contains(&byte) {
                char::from(byte).to_string()
            } else {
                format!("%{byte:02X}")
            }
        })
        .collect();
    let value = api(gh, &format!("repos/{REPO}/git/ref/heads/{encoded}"))?;
    ensure!(
        value["ref"].as_str() == Some(&format!("refs/heads/{reference}"))
            && value["object"]["type"].as_str() == Some("commit"),
        "branch reference identity is unavailable"
    );
    value["object"]["sha"]
        .as_str()
        .filter(|sha| valid_sha(sha))
        .map(str::to_string)
        .context("branch reference has no valid SHA")
}

fn snapshot(gh: &dyn GhRunner, number: u32) -> anyhow::Result<Pull> {
    let mut pull: Pull = serde_json::from_value(api(gh, &format!("repos/{REPO}/pulls/{number}"))?)
        .context("incomplete pull request identity")?;
    ensure!(
        pull.number == number && pull.state == "open",
        "pull request is not open or its identity changed"
    );
    ensure!(
        pull.head.repo.full_name == REPO && pull.base.repo.full_name == REPO,
        "branch updates require a same-repository pull request"
    );
    ensure!(
        !pull.head.reference.is_empty()
            && !pull.base.reference.is_empty()
            && pull.head.reference != pull.base.reference
            && valid_sha(&pull.head.sha),
        "invalid pull request branch identity"
    );
    ensure!(
        current_ref(gh, &pull.head.reference)? == pull.head.sha,
        "pull request identity changed: head metadata is stale"
    );
    // PR base metadata can lag a branch push; only the branch ref is current.
    pull.base.sha = current_ref(gh, &pull.base.reference)?;
    Ok(pull)
}

fn require_idle_ci(gh: &dyn GhRunner, head: &str) -> anyhow::Result<()> {
    let checks = api(
        gh,
        &format!("repos/{REPO}/commits/{head}/check-runs?per_page=100"),
    )?;
    let runs = checks["check_runs"]
        .as_array()
        .context("CI check census is unknown")?;
    ensure!(
        checks["total_count"].as_u64() == Some(runs.len() as u64)
            && runs
                .iter()
                .all(|run| run["status"].as_str() == Some("completed")),
        "CI checks are active or unknown; an explicit acceptance update is required"
    );
    let statuses = api(
        gh,
        &format!("repos/{REPO}/commits/{head}/status?per_page=100"),
    )?;
    let contexts = statuses["statuses"]
        .as_array()
        .context("CI status census is unknown")?;
    ensure!(
        statuses["total_count"].as_u64() == Some(contexts.len() as u64)
            && contexts.iter().all(|context| matches!(
                context["state"].as_str(),
                Some("success" | "failure" | "error")
            ))
            && (!runs.is_empty() || !contexts.is_empty()),
        "CI statuses are active or unknown; an explicit acceptance update is required"
    );
    Ok(())
}

fn shared_storage(repo_root: &Path, number: u32) -> anyhow::Result<(PathBuf, PathBuf)> {
    let common = PathBuf::from(git_line(
        &Git::new(repo_root),
        &["rev-parse", "--path-format=absolute", "--git-common-dir"],
    )?);
    let root = common
        .parent()
        .context("Git common directory has no parent")?;
    Ok((
        common.join("pr-update").join(number.to_string()),
        root.join(".workspace/pr-updates/Exoridus-exosnap")
            .join(number.to_string()),
    ))
}

/// Merges the current actual base into one PR head, once per observed head.
/// Acceptance is asynchronous. Inspect the new refs and requalify the new head.
pub fn update_branch(
    repo_root: &Path,
    gh: &dyn GhRunner,
    request: &UpdateRequest,
) -> anyhow::Result<UpdateOutcome> {
    ensure!(
        request.number > 0,
        "a positive pull request number is required"
    );
    if request.confirm {
        ensure!(
            request.expected_head.as_deref().is_some_and(valid_sha)
                && request.expected_base.as_deref().is_some_and(valid_sha),
            "confirmation requires --expected-head and --expected-base with full SHAs"
        );
    }
    let storage = request
        .confirm
        .then(|| shared_storage(repo_root, request.number))
        .transpose()?;
    let _lock = storage
        .as_ref()
        .map(|(key, _)| host_lock::acquire(LockKind::Tree, Some(key), "pr update-branch"))
        .transpose()?;
    let pull = snapshot(gh, request.number)?;
    if request.confirm {
        ensure!(
            request.expected_head.as_deref() == Some(&pull.head.sha)
                && request.expected_base.as_deref() == Some(&pull.base.sha),
            "pull request head or actual base differs from the caller snapshot"
        );
    }
    let record_paths = storage.as_ref().map(|(_, directory)| {
        (
            directory.join(format!("{}.json", pull.head.sha)),
            directory.join(format!("{}.intent.json", pull.head.sha)),
        )
    });
    if let Some((receipt_path, intent_path)) = &record_paths {
        if receipt_path.try_exists()? {
            let stored: Value = serde_json::from_str(&std::fs::read_to_string(receipt_path)?)?;
            ensure!(
                stored["repo"].as_str() == Some(REPO),
                "accepted update receipt identity is invalid; inspect live refs before retrying"
            );
            let original: UpdateSnapshot = serde_json::from_value(stored)?;
            ensure!(
                original.number == pull.number
                    && original.head == pull.head.sha
                    && valid_sha(&original.base)
                    && !original.base_ref.is_empty(),
                "accepted update receipt identity is invalid"
            );
            return Ok(UpdateOutcome::Accepted {
                snapshot: original,
                already_pending: true,
            });
        }
        ensure!(
            !intent_path.try_exists()?,
            "update outcome is uncertain for this head; inspect {} and live refs before a diagnosed resolution",
            intent_path.display()
        );
    }
    ensure!(
        !pull.draft || request.acceptance_update,
        "draft pull requests require --acceptance-update"
    );
    let compare = api(
        gh,
        &format!("repos/{REPO}/compare/{}...{}", pull.base.sha, pull.head.sha),
    )?;
    let behind = compare["behind_by"]
        .as_u64()
        .context("base comparison is unavailable")?;
    let summary = UpdateSnapshot {
        number: request.number,
        head: pull.head.sha.clone(),
        base: pull.base.sha.clone(),
        base_ref: pull.base.reference.clone(),
        behind,
    };
    if behind == 0 {
        return Ok(UpdateOutcome::UpToDate { snapshot: summary });
    }
    ensure!(
        pull.mergeable == Some(true),
        "pull request conflicts or mergeability is unknown"
    );
    if !request.confirm {
        return Ok(UpdateOutcome::Preview { snapshot: summary });
    }
    let directory = &storage.as_ref().unwrap().1;
    let (receipt_path, intent_path) = record_paths.as_ref().unwrap();
    let receipt = json!({"repo":REPO,"number":pull.number,"head":pull.head.sha,"base":pull.base.sha,"base_ref":pull.base.reference,"behind":behind});
    if !request.acceptance_update {
        require_idle_ci(gh, &pull.head.sha)?;
    }
    std::fs::create_dir_all(directory)?;
    ensure!(
        snapshot(gh, request.number)? == pull,
        "pull request identity changed before update; nothing submitted"
    );
    let temporary = intent_path.with_extension(format!("{}.tmp", std::process::id()));
    let mut intent = std::fs::File::create(&temporary)?;
    writeln!(intent, "{receipt}")?;
    intent.sync_all()?;
    drop(intent);
    std::fs::rename(&temporary, intent_path)
        .context("could not record update intent; nothing submitted")?;
    let response = gh
        .run(&[
            "api",
            "--method",
            "PUT",
            &format!("repos/{REPO}/pulls/{}/update-branch", request.number),
            "--include",
            "--raw-field",
            &format!("expected_head_sha={}", pull.head.sha),
        ])
        .context("branch update outcome is uncertain; no retry was sent, inspect live refs and the intent record")?;
    ensure!(
        response
            .lines()
            .next()
            .and_then(|line| line.split_whitespace().nth(1))
            == Some("202"),
        "branch update outcome is uncertain: acceptance was not confirmed; inspect live refs and the intent record"
    );
    std::fs::rename(intent_path, receipt_path).context(
        "update accepted but receipt transition failed; do not repeat this head, inspect live refs",
    )?;
    Ok(UpdateOutcome::Accepted {
        snapshot: summary,
        already_pending: false,
    })
}

pub fn render_update(outcome: &UpdateOutcome) -> String {
    let (snapshot, result) = match outcome {
        UpdateOutcome::Preview { snapshot } => (snapshot, "Preview only; nothing submitted."),
        UpdateOutcome::UpToDate { snapshot } => {
            (snapshot, "Already up to date; nothing submitted.")
        }
        UpdateOutcome::Accepted {
            snapshot,
            already_pending: false,
        } => (
            snapshot,
            "Update accepted; completion is not verified. Requalify the resulting head.",
        ),
        UpdateOutcome::Accepted {
            snapshot,
            already_pending: true,
        } => (
            snapshot,
            "An update for this head was already accepted against the base shown; no repeat submitted. Inspect live refs.",
        ),
    };
    format!(
        "  #{}  head {}\n  base {} ({})  behind {}\n{result}\n",
        snapshot.number, snapshot.head, snapshot.base_ref, snapshot.base, snapshot.behind
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::fixture_repo_committed;
    use serde_json::{Value, json};
    use std::cell::RefCell;
    use std::collections::VecDeque;

    struct FakeGh {
        pulls: RefCell<VecDeque<Value>>,
        calls: RefCell<Vec<Vec<String>>>,
        behind: u64,
        checks: Value,
        statuses: Value,
        response: String,
        fail_update: bool,
        panic_update: bool,
        head_refs: RefCell<VecDeque<String>>,
        base_refs: RefCell<VecDeque<String>>,
    }

    fn pull() -> Value {
        json!({"number":400,"state":"open","draft":false,"mergeable":true,
            "head":{"ref":"topic","sha":"a".repeat(40),"repo":{"full_name":"Exoridus/exosnap"}},
            "base":{"ref":"next","sha":"b".repeat(40),"repo":{"full_name":"Exoridus/exosnap"}}})
    }

    impl FakeGh {
        fn new() -> Self {
            Self {
                pulls: RefCell::new(VecDeque::from([pull()])),
                calls: RefCell::new(Vec::new()),
                behind: 1,
                checks: json!({"total_count":1,"check_runs":[{"status":"completed"}]}),
                statuses: json!({"total_count":0,"statuses":[]}),
                response:
                    "HTTP/2.0 202 Accepted\r\n\r\n{\"message\":\"Updating pull request branch.\"}"
                        .into(),
                fail_update: false,
                panic_update: false,
                head_refs: RefCell::new(VecDeque::from(["a".repeat(40)])),
                base_refs: RefCell::new(VecDeque::from(["b".repeat(40)])),
            }
        }

        fn puts(&self) -> Vec<Vec<String>> {
            self.calls
                .borrow()
                .iter()
                .filter(|args| args.iter().any(|arg| arg == "PUT"))
                .cloned()
                .collect()
        }
    }

    impl GhRunner for FakeGh {
        fn run(&self, args: &[&str]) -> anyhow::Result<String> {
            self.calls
                .borrow_mut()
                .push(args.iter().map(|arg| (*arg).into()).collect());
            if args.contains(&"PUT") {
                assert!(!self.panic_update, "simulated worker loss after PUT");
                anyhow::ensure!(!self.fail_update, "HTTP 422: head changed");
                return Ok(self.response.clone());
            }
            let endpoint = args[1];
            if let Some(reference) = endpoint.split("/git/ref/heads/").nth(1) {
                let mut references = if reference == "topic" {
                    self.head_refs.borrow_mut()
                } else {
                    self.base_refs.borrow_mut()
                };
                let sha = if references.len() > 1 {
                    references.pop_front().unwrap()
                } else {
                    references.front().unwrap().clone()
                };
                return Ok(json!({"ref":format!("refs/heads/{reference}"),"object":{"type":"commit","sha":sha}}).to_string());
            }
            if endpoint.ends_with("/pulls/400") {
                let mut pulls = self.pulls.borrow_mut();
                return Ok(if pulls.len() > 1 {
                    pulls.pop_front().unwrap()
                } else {
                    pulls.front().unwrap().clone()
                }
                .to_string());
            }
            if endpoint.contains("/compare/") {
                return Ok(json!({"behind_by":self.behind}).to_string());
            }
            if endpoint.contains("/check-runs") {
                return Ok(self.checks.to_string());
            }
            if endpoint.ends_with("/status?per_page=100") {
                return Ok(self.statuses.to_string());
            }
            panic!("unexpected gh call: {args:?}");
        }
    }

    fn request() -> UpdateRequest {
        UpdateRequest {
            number: 400,
            confirm: true,
            expected_head: Some("a".repeat(40)),
            expected_base: Some("b".repeat(40)),
            acceptance_update: false,
        }
    }

    #[test]
    fn preview_never_mutates_or_writes_a_receipt() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let gh = FakeGh::new();
        let request = UpdateRequest {
            confirm: false,
            expected_head: None,
            expected_base: None,
            ..request()
        };
        assert!(matches!(
            update_branch(dir.path(), &gh, &request).unwrap(),
            UpdateOutcome::Preview { .. }
        ));
        assert!(gh.puts().is_empty());
        assert!(!dir.path().join(".workspace/pr-updates").exists());
    }

    #[test]
    fn up_to_date_is_a_noop() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let mut gh = FakeGh::new();
        gh.behind = 0;
        assert!(matches!(
            update_branch(dir.path(), &gh, &request()).unwrap(),
            UpdateOutcome::UpToDate { .. }
        ));
        assert!(gh.puts().is_empty());
    }

    #[test]
    fn behind_pr_updates_once_with_an_expected_head_and_suppresses_repeats() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let gh = FakeGh::new();
        assert!(matches!(
            update_branch(dir.path(), &gh, &request()).unwrap(),
            UpdateOutcome::Accepted {
                already_pending: false,
                ..
            }
        ));
        assert!(matches!(
            update_branch(dir.path(), &gh, &request()).unwrap(),
            UpdateOutcome::Accepted {
                already_pending: true,
                ..
            }
        ));
        let puts = gh.puts();
        assert_eq!(puts.len(), 1);
        assert!(puts[0].contains(&format!("expected_head_sha={}", "a".repeat(40))));
        assert!(
            !puts[0]
                .iter()
                .any(|arg| arg.contains("rebase") || arg.contains("base="))
        );
    }

    #[test]
    fn a_newer_base_cannot_resubmit_while_the_accepted_head_is_unchanged() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let gh = FakeGh::new();
        update_branch(dir.path(), &gh, &request()).unwrap();
        gh.base_refs.borrow_mut()[0] = "c".repeat(40);
        assert!(update_branch(dir.path(), &gh, &request()).is_err());
        assert_eq!(gh.puts().len(), 1);
        let request = UpdateRequest {
            expected_base: Some("c".repeat(40)),
            ..request()
        };
        let outcome = update_branch(dir.path(), &gh, &request).unwrap();
        assert!(
            matches!(outcome, UpdateOutcome::Accepted { snapshot, already_pending:true } if snapshot.base == "b".repeat(40))
        );
        assert_eq!(gh.puts().len(), 1);
    }

    #[test]
    fn a_new_actual_head_reopens_update_eligibility() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let gh = FakeGh::new();
        update_branch(dir.path(), &gh, &request()).unwrap();
        gh.pulls.borrow_mut()[0]["head"]["sha"] = "c".repeat(40).into();
        gh.head_refs.borrow_mut()[0] = "c".repeat(40);
        let request = UpdateRequest {
            expected_head: Some("c".repeat(40)),
            ..request()
        };
        assert!(matches!(
            update_branch(dir.path(), &gh, &request).unwrap(),
            UpdateOutcome::Accepted {
                already_pending: false,
                ..
            }
        ));
        assert_eq!(gh.puts().len(), 2);
    }

    #[test]
    fn pending_request_keeps_its_original_identity_when_new_base_mergeability_is_unknown() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let gh = FakeGh::new();
        update_branch(dir.path(), &gh, &request()).unwrap();
        gh.base_refs.borrow_mut()[0] = "c".repeat(40);
        gh.pulls.borrow_mut()[0]["mergeable"] = Value::Null;
        let request = UpdateRequest {
            expected_base: Some("c".repeat(40)),
            ..request()
        };
        assert!(
            matches!(update_branch(dir.path(), &gh, &request).unwrap(), UpdateOutcome::Accepted { snapshot, already_pending:true } if snapshot.base == "b".repeat(40))
        );
        assert_eq!(gh.puts().len(), 1);
    }

    #[test]
    fn lost_response_or_worker_loss_blocks_a_second_put_for_the_same_head() {
        for panic in [false, true] {
            let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
            let mut gh = FakeGh::new();
            gh.fail_update = !panic;
            gh.panic_update = panic;
            let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                update_branch(dir.path(), &gh, &request())
            }));
            assert!(outcome.is_err() || outcome.unwrap().is_err());
            gh.fail_update = false;
            gh.panic_update = false;
            gh.base_refs.borrow_mut()[0] = "c".repeat(40);
            let request = UpdateRequest {
                expected_base: Some("c".repeat(40)),
                ..request()
            };
            let error = update_branch(dir.path(), &gh, &request).unwrap_err();
            assert!(error.to_string().contains("uncertain"), "{error}");
            assert_eq!(gh.puts().len(), 1);
        }
    }

    #[test]
    fn concurrent_local_calls_submit_only_one_accepted_tuple() {
        use std::sync::{
            Arc, Barrier,
            atomic::{AtomicUsize, Ordering},
        };
        struct CountingGh {
            gh: FakeGh,
            puts: Arc<AtomicUsize>,
        }
        impl GhRunner for CountingGh {
            fn run(&self, args: &[&str]) -> anyhow::Result<String> {
                if args.contains(&"PUT") {
                    self.puts.fetch_add(1, Ordering::SeqCst);
                }
                self.gh.run(args)
            }
        }
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let barrier = Arc::new(Barrier::new(2));
        let puts = Arc::new(AtomicUsize::new(0));
        let threads: Vec<_> = (0..2)
            .map(|_| {
                let path = dir.path().to_path_buf();
                let barrier = barrier.clone();
                let puts = puts.clone();
                std::thread::spawn(move || {
                    let gh = CountingGh {
                        gh: FakeGh::new(),
                        puts,
                    };
                    barrier.wait();
                    update_branch(&path, &gh, &request()).unwrap()
                })
            })
            .collect();
        let outcomes: Vec<_> = threads
            .into_iter()
            .map(|thread| thread.join().unwrap())
            .collect();
        assert_eq!(puts.load(Ordering::SeqCst), 1);
        assert_eq!(
            outcomes
                .iter()
                .filter(|outcome| matches!(
                    outcome,
                    UpdateOutcome::Accepted {
                        already_pending: true,
                        ..
                    }
                ))
                .count(),
            1
        );
    }

    #[test]
    fn linked_worktrees_share_the_update_lock_and_receipts() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let linked = dir.path().join("linked");
        let git = Git::new(dir.path());
        assert_eq!(
            git.run(&["worktree", "add", "--detach", linked.to_str().unwrap()])
                .0,
            0
        );
        assert_eq!(
            shared_storage(dir.path(), 400).unwrap(),
            shared_storage(&linked, 400).unwrap()
        );
    }

    #[test]
    fn confirmation_requires_matching_caller_head_and_base() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        for (head, base) in [
            (None, Some("b".repeat(40))),
            (Some("a".repeat(40)), None),
            (Some("c".repeat(40)), Some("b".repeat(40))),
            (Some("a".repeat(40)), Some("c".repeat(40))),
        ] {
            let gh = FakeGh::new();
            let request = UpdateRequest {
                expected_head: head,
                expected_base: base,
                ..request()
            };
            assert!(update_branch(dir.path(), &gh, &request).is_err());
            assert!(gh.puts().is_empty());
        }
    }

    #[test]
    fn fresh_head_base_and_dependency_identity_drift_refuses_update() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        for field in ["ref"] {
            for tip in ["head", "base"] {
                let gh = FakeGh::new();
                let mut changed = pull();
                changed[tip][field] = "c".repeat(40).into();
                gh.pulls.borrow_mut().push_back(changed);
                let error = update_branch(dir.path(), &gh, &request()).unwrap_err();
                assert!(error.to_string().contains("identity changed"), "{error}");
                assert!(gh.puts().is_empty());
            }
        }
        for tip in ["head", "base"] {
            let gh = FakeGh::new();
            let references = if tip == "head" {
                &gh.head_refs
            } else {
                &gh.base_refs
            };
            references.borrow_mut().push_back("c".repeat(40));
            assert!(update_branch(dir.path(), &gh, &request()).is_err());
            assert!(gh.puts().is_empty());
        }
    }

    #[test]
    fn actual_base_ref_replaces_cached_pr_base_but_stale_head_is_refused() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let gh = FakeGh::new();
        gh.pulls.borrow_mut()[0]["base"]["sha"] = "c".repeat(40).into();
        update_branch(dir.path(), &gh, &request()).unwrap();
        assert_eq!(gh.puts().len(), 1);

        let gh = FakeGh::new();
        gh.head_refs.borrow_mut()[0] = "c".repeat(40);
        assert!(update_branch(dir.path(), &gh, &request()).is_err());
        assert!(gh.puts().is_empty());
    }

    #[test]
    fn conflict_unknown_mergeability_fork_and_closed_pr_refuse_update() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        for case in 0..5 {
            let gh = FakeGh::new();
            let mut changed = pull();
            match case {
                0 => changed["mergeable"] = false.into(),
                1 => changed["mergeable"] = Value::Null,
                2 => changed["head"]["repo"]["full_name"] = "someone/fork".into(),
                3 => changed["state"] = "closed".into(),
                _ => changed["head"]["sha"] = "invalid".into(),
            }
            *gh.pulls.borrow_mut() = VecDeque::from([changed]);
            assert!(update_branch(dir.path(), &gh, &request()).is_err());
            assert!(gh.puts().is_empty());
        }
    }

    #[test]
    fn drafts_require_an_explicit_acceptance_update() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let gh = FakeGh::new();
        gh.pulls.borrow_mut()[0]["draft"] = true.into();
        assert!(update_branch(dir.path(), &gh, &request()).is_err());
        let request = UpdateRequest {
            acceptance_update: true,
            ..request()
        };
        assert!(matches!(
            update_branch(dir.path(), &gh, &request).unwrap(),
            UpdateOutcome::Accepted { .. }
        ));
    }

    #[test]
    fn dependent_pr_compares_and_updates_its_actual_base() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let gh = FakeGh::new();
        gh.pulls.borrow_mut()[0]["base"]["ref"] = "feat/parent".into();
        update_branch(dir.path(), &gh, &request()).unwrap();
        assert!(gh.calls.borrow().iter().any(|args| args.iter().any(|arg| {
            arg.ends_with(&format!("/compare/{}...{}", "b".repeat(40), "a".repeat(40)))
        })));
        assert!(
            gh.calls
                .borrow()
                .iter()
                .all(|args| !args.iter().any(|arg| arg == "next"))
        );
    }

    #[test]
    fn active_or_unknown_ci_needs_an_explicit_acceptance_update() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        for checks in [
            json!({"total_count":1,"check_runs":[{"status":"in_progress"}]}),
            json!({"total_count":2,"check_runs":[]}),
            json!({}),
        ] {
            let mut gh = FakeGh::new();
            gh.checks = checks;
            assert!(update_branch(dir.path(), &gh, &request()).is_err());
            assert!(gh.puts().is_empty());
        }
        let mut gh = FakeGh::new();
        gh.statuses = json!({"total_count":1,"statuses":[{"state":"pending"}]});
        assert!(update_branch(dir.path(), &gh, &request()).is_err());
        let request = UpdateRequest {
            acceptance_update: true,
            ..request()
        };
        update_branch(dir.path(), &gh, &request).unwrap();
    }

    #[test]
    fn api_error_and_nonaccepted_response_never_retry_or_claim_acceptance() {
        for fail in [true, false] {
            let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
            let mut gh = FakeGh::new();
            gh.fail_update = fail;
            gh.response = "HTTP/2.0 200 OK\r\n\r\n{}".into();
            assert!(update_branch(dir.path(), &gh, &request()).is_err());
            assert_eq!(gh.puts().len(), 1);
        }
    }
}
