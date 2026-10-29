use std::collections::BTreeSet;
use std::path::Path;
use std::time::{Duration, Instant};

use anyhow::{Context as _, bail, ensure};
use serde::Deserialize;
use serde_json::Value;

use super::{GhRunner, MergeOutcome, MergePreview, MergeRequest, REPO, merge, view_json};

pub struct MergeStackRequest {
    /// The native stack number, rather than a member pull request number.
    pub number: u32,
    /// Required to submit the atomic stack merge.
    pub confirm: bool,
}

#[derive(Debug)]
pub enum MergeStackOutcome {
    Preview { members: Vec<MergePreview> },
    Pending { uuid: String },
    Enqueued { message: String },
    Merged { sha: String, numbers: Vec<u32> },
}

#[derive(Deserialize, PartialEq, Eq)]
struct Base {
    #[serde(rename = "ref")]
    reference: String,
}

#[derive(Deserialize, PartialEq, Eq)]
struct Tip {
    #[serde(rename = "ref")]
    reference: String,
    sha: String,
}

#[derive(Deserialize, PartialEq, Eq)]
struct Member {
    number: u32,
    title: String,
    state: String,
    draft: bool,
    head: Tip,
    base: Tip,
}

#[derive(Deserialize, PartialEq, Eq)]
struct Stack {
    number: u32,
    open: bool,
    base: Base,
    pull_requests: Vec<Member>,
}

fn api(gh: &dyn GhRunner, endpoint: &str) -> anyhow::Result<Value> {
    serde_json::from_str(&gh.run(&["api", endpoint])?).context("invalid GitHub API JSON")
}

fn valid_sha(sha: &str) -> bool {
    sha.len() == 40
        && sha.bytes().all(|byte| byte.is_ascii_hexdigit())
        && sha.bytes().any(|byte| byte != b'0')
}

fn snapshot(gh: &dyn GhRunner, number: u32) -> anyhow::Result<Stack> {
    let stack: Stack = serde_json::from_value(api(gh, &format!("repos/{REPO}/stacks/{number}"))?)
        .context("incomplete native stack metadata")?;
    ensure!(
        stack.number == number && stack.open,
        "stack identity changed or stack is closed"
    );
    ensure!(
        stack.base.reference == "next",
        "native development stacks must target next"
    );
    ensure!(
        !stack.base.reference.is_empty() && !stack.pull_requests.is_empty(),
        "stack has no base or members"
    );
    let mut numbers = BTreeSet::new();
    let mut branches = BTreeSet::from([stack.base.reference.as_str()]);
    let mut previous: Option<&Member> = None;
    for member in &stack.pull_requests {
        ensure!(
            member.number > 0 && numbers.insert(member.number),
            "invalid or duplicate stack member number"
        );
        ensure!(
            member.state == "open" && !member.draft,
            "stack member #{} is not open and ready",
            member.number
        );
        ensure!(
            !member.head.reference.is_empty() && branches.insert(member.head.reference.as_str()),
            "invalid or repeated stack head branch"
        );
        ensure!(
            valid_sha(&member.head.sha) && valid_sha(&member.base.sha),
            "stack member #{} has an invalid SHA",
            member.number
        );
        if let Some(previous) = previous {
            ensure!(
                member.base == previous.head,
                "stack member #{} does not follow the preceding head",
                member.number
            );
        } else {
            ensure!(
                member.base.reference == stack.base.reference,
                "bottom member base differs from stack base"
            );
        }
        previous = Some(member);
    }
    Ok(stack)
}

struct StoredView(String);

impl GhRunner for StoredView {
    fn run(&self, args: &[&str]) -> anyhow::Result<String> {
        ensure!(
            args.first() == Some(&"pr") && args.get(1) == Some(&"view"),
            "preview attempted an unexpected command"
        );
        Ok(self.0.clone())
    }
}

fn validate_settings(gh: &dyn GhRunner, commit_counts: &[usize]) -> anyhow::Result<()> {
    let settings = api(gh, &format!("repos/{REPO}"))?;
    ensure!(
        settings["allow_squash_merge"].as_bool() == Some(true),
        "repository does not allow squash merging"
    );
    match settings["squash_merge_commit_title"].as_str() {
        Some("PR_TITLE") => Ok(()),
        Some("COMMIT_OR_PR_TITLE") if commit_counts.iter().all(|count| *count > 1) => Ok(()),
        _ => bail!(
            "repository squash title defaults cannot guarantee every member's validated title (#N)"
        ),
    }
}

/// Validates every member and previews by default. Confirmation submits one
/// SHA-pinned async merge and polls briefly; pending or queued is not merged.
pub fn merge_stack(
    repo_root: &Path,
    gh: &dyn GhRunner,
    request: &MergeStackRequest,
) -> anyhow::Result<MergeStackOutcome> {
    merge_stack_with_poll(repo_root, gh, request, 120, &|| {
        std::thread::sleep(Duration::from_secs(2))
    })
}

fn merge_stack_with_poll(
    repo_root: &Path,
    gh: &dyn GhRunner,
    request: &MergeStackRequest,
    polls: usize,
    wait: &dyn Fn(),
) -> anyhow::Result<MergeStackOutcome> {
    ensure!(
        request.number > 0,
        "a positive native stack number is required"
    );
    let stack = snapshot(gh, request.number)?;
    let mut previews = Vec::new();
    let mut commit_counts = Vec::new();
    for member in &stack.pull_requests {
        let view = view_json(
            gh,
            &member.number.to_string(),
            "number,title,state,isDraft,mergeStateStatus,headRefName,headRefOid,baseRefName,baseRefOid,commits",
        )?;
        ensure!(
            view["number"].as_u64() == Some(u64::from(member.number))
                && view["title"].as_str() == Some(&member.title)
                && view["headRefName"].as_str() == Some(&member.head.reference)
                && view["headRefOid"].as_str() == Some(&member.head.sha)
                && view["baseRefName"].as_str() == Some(&member.base.reference)
                && view["baseRefOid"].as_str() == Some(&member.base.sha)
                && view["state"].as_str() == Some("OPEN")
                && view["isDraft"].as_bool() == Some(false),
            "pull request #{} metadata differs from the stack snapshot",
            member.number
        );
        ensure!(
            view["mergeStateStatus"]
                .as_str()
                .is_some_and(|state| !state.is_empty()),
            "pull request has no merge state"
        );
        let commits = view["commits"]
            .as_array()
            .context("pull request commit metadata is missing")?;
        ensure!(!commits.is_empty(), "pull request commit list is empty");
        commit_counts.push(commits.len());
        let MergeOutcome::Preview(preview) = merge(
            repo_root,
            &StoredView(view.to_string()),
            &MergeRequest {
                number: Some(member.number),
                confirm: false,
                delete_branch: false,
                auto: false,
            },
        )?
        else {
            bail!("member validation unexpectedly merged a pull request")
        };
        ensure!(
            preview.subject == format!("{} (#{})", member.title, member.number),
            "pull request #{} automatic squash subject would differ from the validated subject",
            member.number
        );
        previews.push(preview);
    }
    validate_settings(gh, &commit_counts)?;
    if !request.confirm {
        return Ok(MergeStackOutcome::Preview { members: previews });
    }
    validate_settings(gh, &commit_counts)?;
    ensure!(
        snapshot(gh, request.number)? == stack,
        "stack identity changed before submission; nothing submitted"
    );
    let top = stack
        .pull_requests
        .last()
        .context("stack has no top member")?;
    let endpoint = format!("repos/{REPO}/pulls/{}/merge-async", top.number);
    let sha = format!("sha={}", top.head.sha);
    let mut response: Value = serde_json::from_str(&gh.run(&[
        "api",
        "--method",
        "PUT",
        &endpoint,
        "--raw-field",
        "merge_method=squash",
        "--raw-field",
        "merge_action=default",
        "--raw-field",
        &sha,
        "--field",
        "bypass_rules=false",
    ])?)
    .context("invalid async merge response")?;
    let started = Instant::now();
    let mut uuid = None;
    for poll in 0..=polls {
        let details = response["details"]
            .as_object()
            .context("async merge response has no details")?;
        let message = details
            .get("message")
            .and_then(Value::as_str)
            .filter(|message| !message.is_empty())
            .context("async merge response has no message")?;
        match response["status"].as_str() {
            Some("merged") => {
                let sha = details
                    .get("sha")
                    .and_then(Value::as_str)
                    .filter(|sha| valid_sha(sha))
                    .context("merged response has no valid result SHA")?;
                return Ok(MergeStackOutcome::Merged {
                    sha: sha.to_string(),
                    numbers: stack
                        .pull_requests
                        .iter()
                        .map(|member| member.number)
                        .collect(),
                });
            }
            Some("enqueued") => {
                return Ok(MergeStackOutcome::Enqueued {
                    message: message.to_string(),
                });
            }
            Some("failed") => bail!("atomic stack merge failed: {message}"),
            Some("pending") => {
                ensure!(
                    details.get("merge_method").and_then(Value::as_str) == Some("squash")
                        && details.get("merge_action").and_then(Value::as_str) == Some("default")
                        && details.get("expected_head_sha").and_then(Value::as_str)
                            == Some(&top.head.sha),
                    "pending merge options differ from the requested squash/default/head SHA"
                );
                ensure!(
                    details
                        .get("bypass_rules")
                        .is_none_or(|value| value.as_bool() == Some(false)),
                    "pending merge request bypasses rules or has malformed bypass metadata"
                );
                let current = details
                    .get("uuid")
                    .and_then(Value::as_str)
                    .context("pending merge has no UUID")?;
                ensure!(
                    current.len() == 36
                        && current.chars().enumerate().all(|(index, ch)| {
                            if [8, 13, 18, 23].contains(&index) {
                                ch == '-'
                            } else {
                                ch.is_ascii_hexdigit()
                            }
                        }),
                    "pending merge UUID is malformed"
                );
                ensure!(
                    uuid.as_deref().is_none_or(|previous| previous == current),
                    "pending merge UUID changed"
                );
                uuid = Some(current.to_string());
                if poll == polls || started.elapsed() >= Duration::from_secs(240) {
                    return Ok(MergeStackOutcome::Pending {
                        uuid: current.to_string(),
                    });
                }
                wait();
                response = api(gh, &format!("{endpoint}/{current}"))?;
            }
            _ => bail!("unknown async merge status"),
        }
    }
    unreachable!()
}

pub fn render_merge_stack(outcome: &MergeStackOutcome) -> String {
    match outcome {
        MergeStackOutcome::Preview { members } => {
            let mut out = String::new();
            for member in members {
                out.push_str(&format!("  #{}  {}\n", member.number, member.subject));
            }
            out.push_str("Nothing merged: --confirm was not given.\n");
            out
        }
        MergeStackOutcome::Pending { uuid } => format!(
            "Stack merge still pending (request {uuid}); no completed merge is confirmed.\n"
        ),
        MergeStackOutcome::Enqueued { message } => {
            format!("Stack enqueued, not confirmed merged: {message}\n")
        }
        MergeStackOutcome::Merged { sha, numbers } => format!(
            "Stack pull requests {} merged at {sha}.\n",
            numbers
                .iter()
                .map(|number| format!("#{number}"))
                .collect::<Vec<_>>()
                .join(", ")
        ),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pr::GhRunner;
    use serde_json::{Value, json};
    use std::cell::RefCell;
    use std::collections::{HashMap, VecDeque};

    struct FakeGh {
        calls: RefCell<Vec<Vec<String>>>,
        snapshots: RefCell<VecDeque<Value>>,
        views: HashMap<u32, Value>,
        settings: Value,
        responses: RefCell<VecDeque<Result<Value, String>>>,
    }

    impl FakeGh {
        fn valid() -> Self {
            let lower = json!({"number":491,"title":"fix(ui): validate settings","state":"open","draft":false,
                "head":{"ref":"lower","sha":"a".repeat(40)},"base":{"ref":"next","sha":"c".repeat(40)}});
            let upper = json!({"number":490,"title":"fix(ui): validate controls","state":"open","draft":false,
                "head":{"ref":"upper","sha":"b".repeat(40)},"base":{"ref":"lower","sha":"a".repeat(40)}});
            let views = [&lower, &upper].into_iter().map(|member| (member["number"].as_u64().unwrap() as u32,
                json!({"number":member["number"],"title":member["title"],"state":"OPEN","isDraft":false,
                    "mergeStateStatus":"CLEAN","headRefName":member["head"]["ref"],"headRefOid":member["head"]["sha"],
                    "baseRefName":member["base"]["ref"],"baseRefOid":member["base"]["sha"],"commits":[{},{}]}))).collect();
            Self {
                calls: RefCell::new(Vec::new()),
                snapshots: RefCell::new(VecDeque::from([
                    json!({"number":492,"open":true,"base":{"ref":"next"},"pull_requests":[lower,upper]}),
                ])),
                views,
                settings: json!({"allow_squash_merge":true,"squash_merge_commit_title":"COMMIT_OR_PR_TITLE"}),
                responses: RefCell::new(VecDeque::from([Ok(
                    json!({"status":"merged","details":{"message":"done","sha":"d".repeat(40)}}),
                )])),
            }
        }

        fn mutations(&self) -> Vec<Vec<String>> {
            self.calls
                .borrow()
                .iter()
                .filter(|args| args.iter().any(|arg| arg == "PUT"))
                .cloned()
                .collect()
        }

        fn pending() -> Value {
            json!({"status":"pending","details":{"message":"running","uuid":"12345678-1234-1234-1234-123456789abc",
                "merge_method":"squash","merge_action":"default","expected_head_sha":"b".repeat(40)}})
        }
    }

    impl GhRunner for FakeGh {
        fn run(&self, args: &[&str]) -> anyhow::Result<String> {
            self.calls
                .borrow_mut()
                .push(args.iter().map(|arg| arg.to_string()).collect());
            if args.first() == Some(&"pr") {
                return Ok(self.views[&args[2].parse::<u32>()?].to_string());
            }
            let endpoint = args.iter().find(|arg| arg.starts_with("repos/")).unwrap();
            if endpoint.contains("/stacks/") {
                let mut snapshots = self.snapshots.borrow_mut();
                return Ok(if snapshots.len() > 1 {
                    snapshots.pop_front().unwrap()
                } else {
                    snapshots.front().unwrap().clone()
                }
                .to_string());
            }
            if *endpoint == "repos/Exoridus/exosnap" {
                return Ok(self.settings.to_string());
            }
            self.responses
                .borrow_mut()
                .pop_front()
                .expect("unexpected API request")
                .map(|response| response.to_string())
                .map_err(anyhow::Error::msg)
        }
    }

    fn run(gh: &FakeGh, confirm: bool) -> anyhow::Result<MergeStackOutcome> {
        merge_stack_with_poll(
            std::path::Path::new("."),
            gh,
            &MergeStackRequest {
                number: 492,
                confirm,
            },
            2,
            &|| {},
        )
    }

    #[test]
    fn preview_validates_every_member_without_mutating() {
        let gh = FakeGh::valid();
        let outcome = run(&gh, false).unwrap();
        assert!(
            matches!(outcome, MergeStackOutcome::Preview { ref members } if members.len() == 2)
        );
        assert!(gh.mutations().is_empty());
        let rendered = render_merge_stack(&outcome);
        assert!(rendered.contains("fix(ui): validate settings (#491)"));
        assert!(rendered.contains("Nothing merged"));
    }

    #[test]
    fn invalid_lower_title_draft_and_pr_view_drift_refuse_mutation() {
        for change in ["title", "draft", "view-head"] {
            let mut gh = FakeGh::valid();
            match change {
                "title" => {
                    gh.snapshots.borrow_mut()[0]["pull_requests"][0]["title"] = json!("bad title");
                    gh.views.get_mut(&491).unwrap()["title"] = json!("bad title");
                }
                "draft" => gh.snapshots.borrow_mut()[0]["pull_requests"][0]["draft"] = json!(true),
                _ => gh.views.get_mut(&491).unwrap()["headRefOid"] = json!("e".repeat(40)),
            }
            assert!(run(&gh, true).is_err(), "{change}");
            assert!(gh.mutations().is_empty());
        }
    }

    #[test]
    fn unsafe_lower_squash_title_defaults_refuse_mutation() {
        for setting in ["COMMIT_OR_PR_TITLE", "UNKNOWN"] {
            let mut gh = FakeGh::valid();
            gh.settings["squash_merge_commit_title"] = json!(setting);
            gh.views.get_mut(&491).unwrap()["commits"] = json!([{}]);
            assert!(run(&gh, true).is_err());
            assert!(gh.mutations().is_empty());
        }
    }

    #[test]
    fn lower_automatic_subject_cannot_duplicate_or_drop_a_trailing_number() {
        let mut gh = FakeGh::valid();
        let title = "fix(ui): validate settings (#73)";
        gh.snapshots.borrow_mut()[0]["pull_requests"][0]["title"] = json!(title);
        gh.views.get_mut(&491).unwrap()["title"] = json!(title);
        assert!(run(&gh, true).is_err());
        assert!(gh.mutations().is_empty());
    }

    #[test]
    fn pr_title_default_supports_a_single_commit_lower_member() {
        let mut gh = FakeGh::valid();
        gh.settings["squash_merge_commit_title"] = json!("PR_TITLE");
        gh.views.get_mut(&491).unwrap()["commits"] = json!([{}]);
        assert!(matches!(
            run(&gh, false).unwrap(),
            MergeStackOutcome::Preview { .. }
        ));
    }

    #[test]
    fn polling_uses_the_returned_uuid_and_reports_only_a_confirmed_result() {
        let gh = FakeGh::valid();
        *gh.responses.borrow_mut() = VecDeque::from([
            Ok(FakeGh::pending()),
            Ok(json!({"status":"merged","details":{"message":"done","sha":"d".repeat(40)}})),
        ]);
        assert!(
            matches!(run(&gh,true).unwrap(), MergeStackOutcome::Merged { numbers, .. } if numbers == [491,490])
        );
        assert!(gh.calls.borrow().iter().any(|args| args
            == &[
                "api",
                "repos/Exoridus/exosnap/pulls/490/merge-async/12345678-1234-1234-1234-123456789abc"
            ]));
        let gh = FakeGh::valid();
        *gh.responses.borrow_mut() = VecDeque::from([
            Ok(FakeGh::pending()),
            Ok(json!({"status":"failed","details":{"message":"branch rule rejected merge"}})),
        ]);
        assert!(
            run(&gh, true)
                .unwrap_err()
                .to_string()
                .contains("branch rule rejected merge")
        );
    }

    #[test]
    fn stack_reread_detects_head_drift_before_submission() {
        let gh = FakeGh::valid();
        let mut changed = gh.snapshots.borrow()[0].clone();
        changed["pull_requests"][1]["head"]["sha"] = json!("e".repeat(40));
        gh.snapshots.borrow_mut().push_back(changed);
        assert!(run(&gh, true).is_err());
        assert!(gh.mutations().is_empty());
    }

    #[test]
    fn stack_preview_refuses_a_trunk_other_than_next() {
        let mut gh = FakeGh::valid();
        gh.snapshots.borrow_mut()[0]["base"]["ref"] = json!("main");
        gh.snapshots.borrow_mut()[0]["pull_requests"][0]["base"]["ref"] = json!("main");
        gh.views.get_mut(&491).unwrap()["baseRefName"] = json!("main");
        assert!(run(&gh, false).is_err());
        assert!(gh.mutations().is_empty());
    }

    #[test]
    fn submission_pins_sha_and_defaults_without_ambiguous_commit_overrides() {
        let gh = FakeGh::valid();
        assert!(matches!(
            run(&gh, true).unwrap(),
            MergeStackOutcome::Merged { .. }
        ));
        let calls = gh.mutations();
        assert_eq!(calls.len(), 1);
        for arg in [
            "repos/Exoridus/exosnap/pulls/490/merge-async",
            "merge_method=squash",
            "merge_action=default",
            "bypass_rules=false",
            &format!("sha={}", "b".repeat(40)),
        ] {
            assert!(calls[0].iter().any(|actual| actual == arg), "missing {arg}");
        }
        assert!(
            calls[0]
                .windows(2)
                .any(|args| args == ["--field", "bypass_rules=false"])
        );
        assert!(
            !calls[0]
                .iter()
                .any(|arg| arg.starts_with("commit_title=") || arg.starts_with("commit_message="))
        );
    }

    #[test]
    fn automatic_title_defaults_cover_the_top_member_too() {
        let mut gh = FakeGh::valid();
        gh.views.get_mut(&490).unwrap()["commits"] = json!([{}]);
        assert!(run(&gh, false).is_err());
        assert!(gh.mutations().is_empty());
    }

    #[test]
    fn pending_enqueued_failed_and_api_errors_are_not_merged() {
        let gh = FakeGh::valid();
        *gh.responses.borrow_mut() = VecDeque::from([
            Ok(FakeGh::pending()),
            Ok(FakeGh::pending()),
            Ok(FakeGh::pending()),
        ]);
        let outcome = run(&gh, true).unwrap();
        assert!(matches!(outcome, MergeStackOutcome::Pending { .. }));
        assert!(render_merge_stack(&outcome).contains("still pending"));
        let gh = FakeGh::valid();
        *gh.responses.borrow_mut() = VecDeque::from([Ok(
            json!({"status":"enqueued","details":{"message":"queued"}}),
        )]);
        assert!(matches!(
            run(&gh, true).unwrap(),
            MergeStackOutcome::Enqueued { .. }
        ));
        for response in [
            Ok(json!({"status":"failed","details":{"message":"rules rejected merge"}})),
            Err("HTTP 500".to_string()),
            Ok(json!({"status":"unknown","details":{}})),
        ] {
            let gh = FakeGh::valid();
            *gh.responses.borrow_mut() = VecDeque::from([response]);
            assert!(run(&gh, true).is_err());
        }
    }

    #[test]
    fn mismatched_existing_pending_options_fail_closed() {
        let gh = FakeGh::valid();
        let mut pending = FakeGh::pending();
        pending["details"]["merge_method"] = json!("merge");
        *gh.responses.borrow_mut() = VecDeque::from([Ok(pending)]);
        assert!(run(&gh, true).is_err());
        let gh = FakeGh::valid();
        let mut pending = FakeGh::pending();
        pending["details"]["bypass_rules"] = json!(true);
        *gh.responses.borrow_mut() = VecDeque::from([Ok(pending)]);
        assert!(run(&gh, true).is_err());
    }
}
