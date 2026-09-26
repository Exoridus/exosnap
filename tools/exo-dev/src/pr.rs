//! Opens and merges a pull request with the title grammar `commit_policy` owns.
//!
//! `open` creates the pull request as a draft, reads its stored metadata back
//! and re-validates it before marking it ready. A draft does not start the
//! heavy Windows legs, so a title this module somehow got wrong costs
//! nothing but the draft.
//!
//! `merge` never merges without `confirm`: without it, the constructed
//! subject is only previewed. Nothing checks or refuses anything less than
//! that, on purpose, so a preview cannot itself fail with a refusal that
//! looks like a merge was attempted. The merged subject is built from the
//! pull request's *parsed* title with its number appended exactly once,
//! rather than by trusting whatever the title happened to already end in,
//! so a title that already carried its own number cannot produce a subject
//! that carries it twice.
//!
//! Both shell out to `gh` through [`GhRunner`], injected so a caller can
//! supply a scripted double in a test. `git` goes through [`crate::git::Git`],
//! the same as every other module.

use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::sync::LazyLock;

use anyhow::{Context as _, bail};
use regex::Regex;
use serde_json::Value;

use crate::commit_policy;
use crate::git::Git;
use crate::process;

/// The repository every `gh pr` call targets. Matches the remote this tree is
/// developed against; a fork would need its own value here, same as the
/// scripts this module replaces.
const REPO: &str = "Exoridus/exosnap";

static TRAILING_CITATION: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\(#[0-9]+\)$").unwrap());

/// Runs `gh` and returns its stdout. Injected so `open`/`merge` can be
/// exercised against a scripted response instead of a real GitHub repository.
pub trait GhRunner {
    fn run(&self, args: &[&str]) -> anyhow::Result<String>;
}

/// The production `gh` wrapper. `program` names the executable to invoke,
/// defaulting to `"gh"`; a test can point it at a stub script instead of
/// touching `PATH`, so the argument-building code below is exercised as a
/// real child process rather than only through a fake trait object.
pub struct RealGh {
    program: OsString,
}

impl RealGh {
    pub fn new() -> RealGh {
        RealGh {
            program: "gh".into(),
        }
    }
}

impl Default for RealGh {
    fn default() -> RealGh {
        RealGh::new()
    }
}

impl GhRunner for RealGh {
    /// `args` reaches the child process exactly as given, including an empty
    /// string element: `gh pr merge --body ""` is how a merge body is
    /// cleared, and a wrapper that dropped or rejected such an argument would
    /// make that call impossible.
    fn run(&self, args: &[&str]) -> anyhow::Result<String> {
        let mut command = process::command(&self.program.to_string_lossy());
        command.args(args);
        match command.output() {
            Ok(output) if output.status.success() => {
                Ok(String::from_utf8_lossy(&output.stdout).into_owned())
            }
            Ok(output) => {
                let mut combined = String::from_utf8_lossy(&output.stdout).into_owned();
                combined.push_str(&String::from_utf8_lossy(&output.stderr));
                bail!("gh {} failed: {}", args.join(" "), combined.trim());
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                bail!("gh is not installed or not on PATH.");
            }
            Err(error) => bail!("could not run gh {}: {error}", args.join(" ")),
        }
    }
}

fn git_line(git: &Git, args: &[&str]) -> anyhow::Result<String> {
    let (code, stdout) = git.run(args);
    anyhow::ensure!(code == 0, "git {} failed with exit {code}", args.join(" "));
    Ok(stdout.lines().next().unwrap_or_default().trim().to_string())
}

fn view_json(gh: &dyn GhRunner, selector: &str, fields: &str) -> anyhow::Result<Value> {
    let text = gh.run(&["pr", "view", selector, "--repo", REPO, "--json", fields])?;
    serde_json::from_str(&text).context("could not parse gh pr view output as JSON")
}

fn temp_body_path(body: &str) -> anyhow::Result<PathBuf> {
    let unique = format!(
        "{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos()
    );
    let path = std::env::temp_dir().join(format!("exosnap-pr-body-{unique}.md"));
    std::fs::write(&path, body).with_context(|| format!("could not write {}", path.display()))?;
    Ok(path)
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

pub struct OpenRequest {
    /// The pull request title. Defaults to the subject of the newest commit
    /// on this branch that is not on `base`.
    pub subject: Option<String>,
    /// The pull request description. Ignored when `body_file` is set.
    pub body: Option<String>,
    /// Read the description from this file instead of `body`.
    pub body_file: Option<PathBuf>,
    /// Base branch.
    pub base: String,
    /// Leave the pull request in draft instead of marking it ready.
    pub keep_draft: bool,
    /// Do not push the branch first. Fails if the branch has no upstream.
    pub no_push: bool,
}

#[derive(Debug)]
pub struct OpenOutcome {
    pub number: u32,
    pub title: String,
    pub section: Option<&'static str>,
    pub ready: bool,
}

/// Opens the pull request for the current branch. See the module doc for the
/// draft-then-ready sequence.
pub fn open(
    repo_root: &Path,
    gh: &dyn GhRunner,
    request: &OpenRequest,
) -> anyhow::Result<OpenOutcome> {
    let git = Git::new(repo_root);
    let branch = git_line(&git, &["rev-parse", "--abbrev-ref", "HEAD"])?;
    if branch == request.base || branch == "HEAD" {
        bail!("refusing to open a pull request from '{branch}'; check out a topic branch first");
    }

    let subject = match &request.subject {
        Some(subject) => subject.clone(),
        None => {
            // The newest commit the branch adds, not HEAD unconditionally: on a
            // branch that has already been rebased onto a newer base, HEAD is
            // still the branch's own tip, but on a merge commit it would not be.
            let merge_base = git_line(
                &git,
                &["merge-base", "HEAD", &format!("origin/{}", request.base)],
            )?;
            let range = format!("{merge_base}..HEAD");
            let (code, log) = git.run(&["log", "--format=%s", &range]);
            anyhow::ensure!(code == 0, "git log '{range}' failed");
            log.lines().next().map(str::to_string).with_context(|| {
                format!("this branch adds no commit over origin/{}", request.base)
            })?
        }
    };

    let parsed = commit_policy::parse(&subject, false, None);
    if !parsed.valid {
        bail!(
            "the title '{subject}' -- {}. CONTRIBUTING.md has the rules.",
            parsed.problem.unwrap_or_default()
        );
    }
    if let Some(pr) = parsed.pull_request {
        // It cannot be the own number yet -- there is no pull request -- so
        // this is a citation, and a citation at the END of the title is
        // indistinguishable from the number the squash merge is about to
        // append.
        bail!(
            "the title ends in ' (#{pr})'. The squash merge appends the pull request number; a citation of another pull request belongs inside the summary, not at the end."
        );
    }

    let body = match (&request.body_file, &request.body) {
        (Some(path), _) => std::fs::read_to_string(path)
            .with_context(|| format!("could not read {}", path.display()))?,
        (None, Some(body)) => body.clone(),
        (None, None) => {
            "What changed, why, and what validated it.\n\nReplace this before marking the pull request ready.".to_string()
        }
    };

    if !request.no_push {
        let (code, _) = git.run(&["push", "--set-upstream", "origin", &branch]);
        anyhow::ensure!(
            code == 0,
            "git push --set-upstream origin '{branch}' failed"
        );
    }

    let body_path = temp_body_path(&body)?;
    let body_path_str = body_path.to_string_lossy().into_owned();
    let create_result = gh.run(&[
        "pr",
        "create",
        "--repo",
        REPO,
        "--base",
        &request.base,
        "--head",
        &branch,
        "--title",
        &subject,
        "--body-file",
        &body_path_str,
        "--draft",
    ]);
    let _ = std::fs::remove_file(&body_path);
    create_result?;

    // Read the stored title back rather than trusting what was sent: GitHub is
    // what the squash merge and the changelog will read, and a title that
    // arrived altered (or a pull request that attached to the wrong head) has
    // to be found here, while it is still a draft.
    let view = view_json(gh, &branch, "number,title,isDraft,headRefName,baseRefName")?;
    let number = view["number"]
        .as_u64()
        .context("gh pr view returned no number")? as u32;
    let title = view["title"].as_str().unwrap_or_default().to_string();
    let head_ref = view["headRefName"].as_str().unwrap_or_default();
    let base_ref = view["baseRefName"].as_str().unwrap_or_default();

    let mut problems = Vec::new();
    if title != subject {
        problems.push(format!(
            "stored title '{title}' is not the title that was sent"
        ));
    }
    if head_ref != branch {
        problems.push(format!("head is '{head_ref}', not '{branch}'"));
    }
    if base_ref != request.base {
        problems.push(format!("base is '{base_ref}', not '{}'", request.base));
    }

    let stored = commit_policy::parse(&title, false, Some(number));
    if !stored.valid {
        problems.push(format!(
            "stored title -- {}",
            stored.problem.clone().unwrap_or_default()
        ));
    }

    if !problems.is_empty() {
        bail!(
            "pull request #{number} was created but its metadata is wrong; it is still a draft. Fix it, then re-run with --no-push. {}",
            problems.join("; ")
        );
    }

    let ready = if request.keep_draft {
        false
    } else {
        gh.run(&["pr", "ready", &number.to_string(), "--repo", REPO])?;
        true
    };

    Ok(OpenOutcome {
        number,
        title,
        section: stored.section,
        ready,
    })
}

pub fn render_open(outcome: &OpenOutcome) -> String {
    let mut out = String::new();
    out.push_str(&format!("  #{}  {}\n", outcome.number, outcome.title));
    out.push_str(&format!(
        "  files under {}\n",
        outcome.section.unwrap_or("no changelog section")
    ));
    if outcome.ready {
        out.push_str(&format!(
            "pull request #{} is ready for review\n",
            outcome.number
        ));
    } else {
        out.push_str(&format!("pull request #{} left in draft\n", outcome.number));
    }
    out
}

// ---------------------------------------------------------------------------
// merge
// ---------------------------------------------------------------------------

pub struct MergeRequest {
    /// The pull request to merge. Defaults to the one for the current branch.
    pub number: Option<u32>,
    /// Required to actually merge. Without it, `merge` only previews.
    pub confirm: bool,
    /// Delete the head branch after the merge.
    pub delete_branch: bool,
    /// Enable auto-merge instead of merging now.
    pub auto: bool,
}

#[derive(Debug)]
pub struct MergePreview {
    pub number: u32,
    pub subject: String,
    pub section: Option<&'static str>,
    pub merge_state: String,
}

#[derive(Debug)]
pub struct Merged {
    pub number: u32,
    pub subject: String,
    pub section: Option<&'static str>,
    pub auto: bool,
}

#[derive(Debug)]
pub enum MergeOutcome {
    /// `confirm` was not given: nothing was merged.
    Preview(MergePreview),
    Merged(Merged),
}

/// Squash-merges a pull request with the exact subject the changelog cut
/// reads. See the module doc for why `confirm` gates the merge and not the
/// preview.
pub fn merge(
    repo_root: &Path,
    gh: &dyn GhRunner,
    request: &MergeRequest,
) -> anyhow::Result<MergeOutcome> {
    let selector = match request.number {
        Some(number) if number > 0 => number.to_string(),
        _ => {
            let git = Git::new(repo_root);
            git_line(&git, &["rev-parse", "--abbrev-ref", "HEAD"])?
        }
    };

    let view = view_json(gh, &selector, "number,title,state,isDraft,mergeStateStatus")?;
    let number = view["number"]
        .as_u64()
        .context("gh pr view returned no number")? as u32;
    let title = view["title"].as_str().unwrap_or_default().to_string();
    let state = view["state"].as_str().unwrap_or_default().to_string();
    let is_draft = view["isDraft"].as_bool().unwrap_or(false);
    let merge_state = view["mergeStateStatus"]
        .as_str()
        .unwrap_or_default()
        .to_string();

    if state != "OPEN" {
        bail!("pull request #{number} is {state}, not OPEN");
    }
    if is_draft {
        bail!("pull request #{number} is a draft");
    }

    let parsed_title = commit_policy::parse(&title, false, Some(number));
    if !parsed_title.valid {
        bail!(
            "pull request #{number} title '{title}' -- {}. CONTRIBUTING.md has the rules.",
            parsed_title.problem.clone().unwrap_or_default()
        );
    }

    let subject = commit_policy::format_merge_subject(&parsed_title, number);

    let merged = commit_policy::parse(&subject, true, None);
    if !merged.valid {
        bail!(
            "the constructed subject '{subject}' -- {}",
            merged.problem.clone().unwrap_or_default()
        );
    }
    if merged.pull_request != Some(number) {
        bail!(
            "the constructed subject carries {}, not #{number}",
            merged
                .pull_request
                .map(|n| format!("#{n}"))
                .unwrap_or_else(|| "no number".to_string())
        );
    }
    if merged
        .summary
        .as_deref()
        .is_some_and(|summary| TRAILING_CITATION.is_match(summary))
    {
        bail!("the constructed subject repeats a pull request number: '{subject}'");
    }

    if !request.confirm {
        return Ok(MergeOutcome::Preview(MergePreview {
            number,
            subject,
            section: merged.section,
            merge_state,
        }));
    }

    let number_str = number.to_string();
    let mut merge_args: Vec<String> = vec![
        "pr".into(),
        "merge".into(),
        number_str,
        "--repo".into(),
        REPO.into(),
        "--squash".into(),
        "--subject".into(),
        subject.clone(),
        "--body".into(),
        String::new(),
    ];
    if request.auto {
        merge_args.push("--auto".into());
    }
    if request.delete_branch {
        merge_args.push("--delete-branch".into());
    }
    let merge_args_ref: Vec<&str> = merge_args.iter().map(String::as_str).collect();
    gh.run(&merge_args_ref)?;

    Ok(MergeOutcome::Merged(Merged {
        number,
        subject,
        section: merged.section,
        auto: request.auto,
    }))
}

pub fn render_merge(outcome: &MergeOutcome) -> String {
    match outcome {
        MergeOutcome::Preview(preview) => format!(
            "  #{}  merge state {}\n  subject  {}\n  files under {}\n\nNothing merged: --confirm was not given.\n",
            preview.number,
            preview.merge_state,
            preview.subject,
            preview.section.unwrap_or("no changelog section")
        ),
        MergeOutcome::Merged(merged) => format!(
            "  #{}  subject  {}\n  files under {}\npull request #{} {}\n",
            merged.number,
            merged.subject,
            merged.section.unwrap_or("no changelog section"),
            merged.number,
            if merged.auto {
                "queued for auto-merge"
            } else {
                "merged"
            }
        ),
    }
}

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::fixture_repo_committed;
    use std::cell::RefCell;

    struct FakeGh {
        calls: RefCell<Vec<Vec<String>>>,
        view_json: String,
    }

    impl FakeGh {
        fn new(view_json: &str) -> FakeGh {
            FakeGh {
                calls: RefCell::new(Vec::new()),
                view_json: view_json.to_string(),
            }
        }

        fn calls(&self) -> Vec<Vec<String>> {
            self.calls.borrow().clone()
        }

        fn called(&self, args: &[&str]) -> bool {
            self.calls()
                .iter()
                .any(|call| call.iter().map(String::as_str).eq(args.iter().copied()))
        }
    }

    impl GhRunner for FakeGh {
        fn run(&self, args: &[&str]) -> anyhow::Result<String> {
            self.calls
                .borrow_mut()
                .push(args.iter().map(|s| s.to_string()).collect());
            if args.first() == Some(&"pr") && args.get(1) == Some(&"view") {
                return Ok(self.view_json.clone());
            }
            Ok(String::new())
        }
    }

    fn topic_branch() -> (tempfile::TempDir, String) {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let git = Git::new(dir.path());
        let (code, _) = git.run(&["checkout", "-q", "-b", "topic"]);
        assert_eq!(code, 0);
        (dir, "topic".to_string())
    }

    fn base_open_request(subject: &str, branch: &str) -> String {
        format!(
            r#"{{"number":42,"title":"{subject}","isDraft":true,"headRefName":"{branch}","baseRefName":"next"}}"#
        )
    }

    // -- open ---------------------------------------------------------------

    #[test]
    fn open_refuses_a_title_that_already_ends_in_a_pull_request_number() {
        let (dir, _branch) = topic_branch();
        let gh = FakeGh::new("{}");
        let request = OpenRequest {
            subject: Some("feat(engine): add device enumeration (#5)".to_string()),
            body: None,
            body_file: None,
            base: "next".to_string(),
            keep_draft: false,
            no_push: true,
        };
        let error = open(dir.path(), &gh, &request).unwrap_err();
        assert!(error.to_string().contains("ends in ' (#5)'"), "{error}");
        assert!(gh.calls().is_empty(), "gh should not be called at all");
    }

    #[test]
    fn open_refuses_when_the_branch_equals_the_base() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let git = Git::new(dir.path());
        let branch = git.branch().unwrap();
        let gh = FakeGh::new("{}");
        let request = OpenRequest {
            subject: Some("feat(engine): add device enumeration".to_string()),
            body: None,
            body_file: None,
            base: branch,
            keep_draft: false,
            no_push: true,
        };
        let error = open(dir.path(), &gh, &request).unwrap_err();
        assert!(
            error.to_string().contains("check out a topic branch"),
            "{error}"
        );
    }

    #[test]
    fn open_creates_a_draft_then_marks_it_ready_when_the_stored_title_matches() {
        let (dir, branch) = topic_branch();
        let subject = "feat(engine): add device enumeration";
        let gh = FakeGh::new(&base_open_request(subject, &branch));
        let request = OpenRequest {
            subject: Some(subject.to_string()),
            body: None,
            body_file: None,
            base: "next".to_string(),
            keep_draft: false,
            no_push: true,
        };
        let outcome = open(dir.path(), &gh, &request).unwrap();
        assert_eq!(outcome.number, 42);
        assert_eq!(outcome.title, subject);
        assert!(outcome.ready);
        assert!(gh.called(&["pr", "ready", "42", "--repo", REPO]));
    }

    #[test]
    fn open_leaves_the_pull_request_in_draft_when_keep_draft_is_set() {
        let (dir, branch) = topic_branch();
        let subject = "feat(engine): add device enumeration";
        let gh = FakeGh::new(&base_open_request(subject, &branch));
        let request = OpenRequest {
            subject: Some(subject.to_string()),
            body: None,
            body_file: None,
            base: "next".to_string(),
            keep_draft: true,
            no_push: true,
        };
        let outcome = open(dir.path(), &gh, &request).unwrap();
        assert!(!outcome.ready);
        assert!(
            !gh.calls()
                .iter()
                .any(|c| c.first().map(String::as_str) == Some("pr")
                    && c.get(1).map(String::as_str) == Some("ready"))
        );
    }

    #[test]
    fn open_refuses_when_the_stored_title_does_not_match_and_leaves_it_a_draft() {
        let (dir, branch) = topic_branch();
        let subject = "feat(engine): add device enumeration";
        let gh = FakeGh::new(&base_open_request("feat(engine): something else", &branch));
        let request = OpenRequest {
            subject: Some(subject.to_string()),
            body: None,
            body_file: None,
            base: "next".to_string(),
            keep_draft: false,
            no_push: true,
        };
        let error = open(dir.path(), &gh, &request).unwrap_err();
        assert!(
            error.to_string().contains("is not the title that was sent"),
            "{error}"
        );
        assert!(
            !gh.calls()
                .iter()
                .any(|c| c.get(1).map(String::as_str) == Some("ready")),
            "must not mark it ready once the readback disagrees"
        );
    }

    #[test]
    fn open_pushes_the_branch_before_creating_unless_no_push_is_set() {
        let origin = tempfile::tempdir().unwrap();
        let (dir, branch) = topic_branch();
        let git = Git::new(dir.path());
        assert_eq!(Git::new(origin.path()).run(&["init", "-q", "--bare"]).0, 0);
        assert_eq!(
            git.run(&["remote", "add", "origin", &origin.path().to_string_lossy()])
                .0,
            0
        );

        let subject = "feat(engine): add device enumeration";
        let gh = FakeGh::new(&base_open_request(subject, &branch));
        let request = OpenRequest {
            subject: Some(subject.to_string()),
            body: None,
            body_file: None,
            base: "next".to_string(),
            keep_draft: false,
            no_push: false,
        };
        open(dir.path(), &gh, &request).unwrap();

        let (code, sha) = git.run(&["rev-parse", "HEAD"]);
        assert_eq!(code, 0);
        let (code, remote_sha) = Git::new(origin.path()).run(&["rev-parse", "refs/heads/topic"]);
        assert_eq!(code, 0, "the branch was not pushed to origin");
        assert_eq!(sha.trim(), remote_sha.trim());
    }

    #[test]
    fn open_infers_the_subject_from_the_newest_commit_when_none_is_given() {
        let origin = tempfile::tempdir().unwrap();
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let git = Git::new(dir.path());
        assert_eq!(git.run(&["branch", "-M", "next"]).0, 0);
        assert_eq!(Git::new(origin.path()).run(&["init", "-q", "--bare"]).0, 0);
        assert_eq!(
            git.run(&["remote", "add", "origin", &origin.path().to_string_lossy()])
                .0,
            0
        );
        assert_eq!(git.run(&["push", "origin", "next"]).0, 0);
        assert_eq!(git.run(&["fetch", "origin"]).0, 0);
        assert_eq!(git.run(&["checkout", "-q", "-b", "topic"]).0, 0);
        std::fs::write(dir.path().join("feature.txt"), "x\n").unwrap();
        assert_eq!(git.run(&["add", "-A"]).0, 0);
        assert_eq!(
            git.run(&["commit", "-q", "-m", "feat(ui): add the missing panel",])
                .0,
            0
        );

        let gh = FakeGh::new(&base_open_request(
            "feat(ui): add the missing panel",
            "topic",
        ));
        let request = OpenRequest {
            subject: None,
            body: None,
            body_file: None,
            base: "next".to_string(),
            keep_draft: false,
            no_push: true,
        };
        let outcome = open(dir.path(), &gh, &request).unwrap();
        assert_eq!(outcome.title, "feat(ui): add the missing panel");
        assert!(
            gh.called(&[
                "pr",
                "create",
                "--repo",
                REPO,
                "--base",
                "next",
                "--head",
                "topic",
                "--title",
                "feat(ui): add the missing panel",
                "--body-file",
                gh.calls()
                    .iter()
                    .find(|c| c.first().map(String::as_str) == Some("pr")
                        && c.get(1).map(String::as_str) == Some("create"))
                    .and_then(|c| c.get(11))
                    .unwrap(),
                "--draft",
            ])
        );
    }

    // -- merge ----------------------------------------------------------------

    fn merge_view(title: &str, state: &str, is_draft: bool, merge_state: &str) -> String {
        format!(
            r#"{{"number":400,"title":"{title}","state":"{state}","isDraft":{is_draft},"mergeStateStatus":"{merge_state}"}}"#
        )
    }

    #[test]
    fn merge_without_confirm_previews_and_merges_nothing() {
        let dir = tempfile::tempdir().unwrap();
        let gh = FakeGh::new(&merge_view(
            "feat(engine): add device enumeration",
            "OPEN",
            false,
            "CLEAN",
        ));
        let request = MergeRequest {
            number: Some(400),
            confirm: false,
            delete_branch: false,
            auto: false,
        };
        let outcome = merge(dir.path(), &gh, &request).unwrap();
        match outcome {
            MergeOutcome::Preview(preview) => {
                assert_eq!(preview.number, 400);
                assert_eq!(
                    preview.subject,
                    "feat(engine): add device enumeration (#400)"
                );
            }
            MergeOutcome::Merged(_) => panic!("must not merge without --confirm"),
        }
        assert!(
            !gh.calls()
                .iter()
                .any(|c| c.get(1).map(String::as_str) == Some("merge"))
        );
    }

    #[test]
    fn merge_refuses_a_draft_pull_request() {
        let dir = tempfile::tempdir().unwrap();
        let gh = FakeGh::new(&merge_view(
            "feat(engine): add device enumeration",
            "OPEN",
            true,
            "CLEAN",
        ));
        let request = MergeRequest {
            number: Some(400),
            confirm: true,
            delete_branch: false,
            auto: false,
        };
        let error = merge(dir.path(), &gh, &request).unwrap_err();
        assert!(error.to_string().contains("is a draft"), "{error}");
        assert!(
            !gh.calls()
                .iter()
                .any(|c| c.get(1).map(String::as_str) == Some("merge"))
        );
    }

    #[test]
    fn merge_refuses_a_closed_or_merged_pull_request() {
        let dir = tempfile::tempdir().unwrap();
        let gh = FakeGh::new(&merge_view(
            "feat(engine): add device enumeration",
            "MERGED",
            false,
            "CLEAN",
        ));
        let request = MergeRequest {
            number: Some(400),
            confirm: true,
            delete_branch: false,
            auto: false,
        };
        let error = merge(dir.path(), &gh, &request).unwrap_err();
        assert!(error.to_string().contains("is MERGED, not OPEN"), "{error}");
    }

    #[test]
    fn merge_refuses_a_title_that_already_carries_its_own_pull_request_number() {
        let dir = tempfile::tempdir().unwrap();
        let gh = FakeGh::new(&merge_view(
            "feat(engine): add device enumeration (#400)",
            "OPEN",
            false,
            "CLEAN",
        ));
        let request = MergeRequest {
            number: Some(400),
            confirm: true,
            delete_branch: false,
            auto: false,
        };
        let error = merge(dir.path(), &gh, &request).unwrap_err();
        assert!(
            error.to_string().contains("its own pull request number"),
            "{error}"
        );
        assert!(
            !gh.calls()
                .iter()
                .any(|c| c.get(1).map(String::as_str) == Some("merge"))
        );
    }

    #[test]
    fn merge_confirm_merges_and_passes_delete_branch_and_auto_through() {
        let dir = tempfile::tempdir().unwrap();
        let gh = FakeGh::new(&merge_view(
            "feat(engine): add device enumeration",
            "OPEN",
            false,
            "CLEAN",
        ));
        let request = MergeRequest {
            number: Some(400),
            confirm: true,
            delete_branch: true,
            auto: true,
        };
        let outcome = merge(dir.path(), &gh, &request).unwrap();
        match outcome {
            MergeOutcome::Merged(merged) => {
                assert_eq!(
                    merged.subject,
                    "feat(engine): add device enumeration (#400)"
                );
                assert!(merged.auto);
            }
            MergeOutcome::Preview(_) => panic!("must merge when --confirm is set"),
        }
        assert!(gh.called(&[
            "pr",
            "merge",
            "400",
            "--repo",
            REPO,
            "--squash",
            "--subject",
            "feat(engine): add device enumeration (#400)",
            "--body",
            "",
            "--auto",
            "--delete-branch",
        ]));
    }

    #[test]
    fn merge_infers_the_pull_request_from_the_current_branch_when_number_is_omitted() {
        let dir = fixture_repo_committed(&[("README.md", "Fixture.\n")]);
        let git = Git::new(dir.path());
        assert_eq!(git.run(&["checkout", "-q", "-b", "my-topic"]).0, 0);
        let gh = FakeGh::new(&merge_view(
            "feat(engine): add device enumeration",
            "OPEN",
            false,
            "CLEAN",
        ));
        let request = MergeRequest {
            number: None,
            confirm: false,
            delete_branch: false,
            auto: false,
        };
        merge(dir.path(), &gh, &request).unwrap();
        assert!(gh.calls().iter().any(|c| {
            c.first().map(String::as_str) == Some("pr")
                && c.get(1).map(String::as_str) == Some("view")
                && c.get(2).map(String::as_str) == Some("my-topic")
        }));
    }

    // -- the gh wrapper itself --------------------------------------------

    #[cfg(unix)]
    #[test]
    fn real_gh_passes_an_empty_argument_through_unchanged() {
        use std::os::unix::fs::PermissionsExt;

        let dir = tempfile::tempdir().unwrap();
        let script = dir.path().join("fake-gh.sh");
        std::fs::write(
            &script,
            "#!/bin/sh\nfor a in \"$@\"; do printf '<%s>\\n' \"$a\"; done\n",
        )
        .unwrap();
        let mut perms = std::fs::metadata(&script).unwrap().permissions();
        perms.set_mode(0o755);
        std::fs::set_permissions(&script, perms).unwrap();

        let gh = RealGh {
            program: script.into(),
        };
        let output = gh.run(&["pr", "merge", "--body", ""]).unwrap();
        let lines: Vec<&str> = output.lines().collect();
        assert_eq!(lines, vec!["<pr>", "<merge>", "<--body>", "<>"]);
    }

    // -- the workflow cost contract ----------------------------------------
    //
    // Ported from the homegrown test harness the PowerShell script tests used,
    // read as text rather than as YAML for the same reason that harness gave:
    // asserting the absence of one word does not justify a YAML dependency.

    fn workflow(name: &str) -> String {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../.github/workflows")
            .join(name);
        std::fs::read_to_string(&path)
            .unwrap_or_else(|error| panic!("could not read {}: {error}", path.display()))
    }

    /// The body of a top-level (two-space-indented) workflow job, up to but
    /// not including the next job key. The regex crate has no look-ahead, so
    /// the end of the block is found by a second, independent search over
    /// the remainder rather than as part of one pattern.
    fn job_body(content: &str, job: &str) -> String {
        let normalized = content.replace("\r\n", "\n");
        let header = format!("\n  {job}:\n");
        let start = normalized
            .find(&header)
            .unwrap_or_else(|| panic!("workflow has no job '{job}'"))
            + header.len();
        let rest = &normalized[start..];
        let next_key = Regex::new(r"(?m)^  [a-z][a-z0-9-]*:").unwrap();
        let end = next_key.find(rest).map(|m| m.start()).unwrap_or(rest.len());
        rest[..end].to_string()
    }

    fn pull_request_types(content: &str, name: &str) -> Vec<String> {
        let re =
            Regex::new(r"(?m)^\s*pull_request:\s*\r?\n\s*types:\s*\[(?P<types>[^\]]*)\]").unwrap();
        let caps = re
            .captures(content)
            .unwrap_or_else(|| panic!("{name} declares no pull_request types list"));
        caps["types"]
            .split(',')
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .collect()
    }

    #[test]
    fn heavy_ci_does_not_listen_for_a_title_only_edit() {
        let ci = workflow("ci.yml");
        let types = pull_request_types(&ci, "ci.yml");
        assert!(
            !types.iter().any(|t| t == "edited"),
            "ci.yml still triggers on 'edited': {types:?}"
        );
        for required in ["opened", "synchronize", "reopened"] {
            assert!(
                types.iter().any(|t| t == required),
                "ci.yml no longer triggers on '{required}'"
            );
        }
    }

    #[test]
    fn the_job_the_heavy_legs_hang_off_validates_the_title_itself() {
        let ci = workflow("ci.yml");
        let body = job_body(&ci, "guardrails");
        let body = body.as_str();
        assert!(
            body.contains("cargo exo-dev") && body.contains("--profile ci-guardrails"),
            "guardrails does not run the ci-guardrails profile"
        );
        assert!(
            body.contains("--pr-title"),
            "guardrails does not pass the title"
        );
        assert!(
            body.contains("--pr-number"),
            "guardrails does not pass the pull request number"
        );
        assert!(
            body.contains("github.event.pull_request.title"),
            "guardrails reads no title"
        );
    }

    #[test]
    fn the_metadata_workflow_listens_for_the_edit_and_checks_the_title() {
        let pr_policy = workflow("pr-policy.yml");
        let types = pull_request_types(&pr_policy, "pr-policy.yml");
        assert!(
            types.iter().any(|t| t == "edited"),
            "pr-policy.yml does not trigger on 'edited': {types:?}"
        );
        assert!(
            pr_policy.contains("cargo exo-dev")
                && pr_policy.contains("--profile pr-policy")
                && pr_policy.contains("--pr-title"),
            "pr-policy.yml does not run the commit policy check against the title"
        );
        assert!(
            pr_policy.contains("--pr-number"),
            "pr-policy.yml does not pass the pull request number"
        );
    }

    #[test]
    fn the_metadata_verdict_is_its_own_required_context() {
        let pr_policy = workflow("pr-policy.yml");
        assert!(
            Regex::new(r"(?m)^\s*name:\s*pr-policy-required\s*$")
                .unwrap()
                .is_match(&pr_policy),
            "pr-policy.yml declares no pr-policy-required aggregate job"
        );
        assert!(
            !Regex::new(r"(?m)^\s*name:\s*ci-required\s*$")
                .unwrap()
                .is_match(&pr_policy),
            "pr-policy.yml reports into ci-required"
        );
    }

    #[test]
    fn the_expensive_legs_wait_for_the_cheap_guardrails() {
        let ci = workflow("ci.yml");
        let needs_guardrails = Regex::new(r"(?m)^\s*needs:.*guardrails").unwrap();
        for job in ["build-test-debug", "build-test-release"] {
            let body = job_body(&ci, job);
            assert!(
                needs_guardrails.is_match(&body),
                "'{job}' does not declare needs on guardrails"
            );
        }
    }
}
