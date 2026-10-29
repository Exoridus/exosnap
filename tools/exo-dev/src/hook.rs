//! Git hook entry points. The hook scripts only locate exo-dev; what runs, in what
//! order and what a failure stops is decided here and in the profiles.

use std::io::BufRead;

use crate::{git::Git, scope::Scope};

#[cfg(test)]
const ZERO_SHA: &str = "0000000000000000000000000000000000000000";

/// Why a commit on this branch is refused, or `None` when it is allowed.
///
/// Direct commits to either protected branch bypass pull request review, and a
/// local branch with its own commits diverges after a squash merge upstream.
pub fn protected_branch_refusal(branch: &str, allow: Option<&str>) -> Option<Vec<String>> {
    if !matches!(branch, "main" | "next") || allow == Some("1") {
        return None;
    }
    Some(vec![
        format!("pre-commit: refusing to commit on '{branch}'."),
        "  Work on a branch:      git switch -c <name> origin/next".into(),
        "  Move what is staged:   git stash && git switch -c <name> origin/next && git stash pop"
            .into(),
        "  Intentional exception: ALLOW_PROTECTED_COMMIT=1 git commit ...".into(),
    ])
}

#[derive(Debug)]
pub struct BranchUpdate {
    pub local_sha: String,
    pub remote_sha: String,
}

/// Branch updates from Git's pre-push input. Tags, deletions and unchanged refs
/// do not need verification. Malformed or unreadable input refuses the push.
pub fn branch_updates(input: impl BufRead) -> anyhow::Result<Vec<BranchUpdate>> {
    let mut updates = Vec::new();
    for line in input.lines() {
        let line = line?;
        let fields: Vec<&str> = line.split_whitespace().collect();
        let [local_ref, local_sha, remote_ref, remote_sha] = fields[..] else {
            anyhow::bail!("malformed pre-push ref input");
        };
        for sha in [local_sha, remote_sha] {
            anyhow::ensure!(
                matches!(sha.len(), 40 | 64) && sha.bytes().all(|byte| byte.is_ascii_hexdigit()),
                "invalid SHA in pre-push ref input"
            );
        }
        if (local_ref.starts_with("refs/heads/") || remote_ref.starts_with("refs/heads/"))
            && !local_sha.bytes().all(|byte| byte == b'0')
            && local_sha != remote_sha
        {
            updates.push(BranchUpdate {
                local_sha: local_sha.into(),
                remote_sha: remote_sha.into(),
            });
        }
    }
    Ok(updates)
}

/// Refuses partially staged files and working tooling absent from the index,
/// because the gate executes tooling and reads source from the working tree.
pub fn ensure_staged_inputs_match(git: &Git) -> anyhow::Result<()> {
    let staged = git.scope_files(None, true)?;
    for file in git.working_scope_files()? {
        anyhow::ensure!(
            !staged.contains(&file) && !Scope::of(std::slice::from_ref(&file)).requires_rust,
            "staged verification cannot use working inputs that differ from the index: {file}; stage or restore the overlapping or tooling changes first"
        );
    }
    Ok(())
}

/// Certifies only the checked-out source. Unknown diffs widen the Rust gate,
/// while another pushed source tree needs its own checkout and invocation.
pub fn pre_push_scope(git: &Git, updates: &[BranchUpdate]) -> anyhow::Result<Scope> {
    let head = git
        .head()
        .ok_or_else(|| anyhow::anyhow!("cannot resolve checked-out HEAD"))?;
    for update in updates {
        anyhow::ensure!(
            update.local_sha == head,
            "pushed SHA {} differs from checked-out HEAD {head}; switch to that branch or its worktree and push it separately",
            update.local_sha
        );
    }
    let Some(base) = git.default_base() else {
        return Ok(Scope::of(&[]));
    };
    let Ok(mut files) = git.scope_files(Some(&base), false) else {
        return Ok(Scope::of(&[]));
    };
    for update in updates {
        if update.remote_sha.bytes().all(|byte| byte == b'0') {
            continue;
        }
        let Ok(remote_files) = git.revision_scope_files(&update.remote_sha, &update.local_sha)
        else {
            return Ok(Scope::of(&[]));
        };
        files.extend(remote_files);
    }
    files.sort();
    files.dedup();
    Ok(Scope::of(&files))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pushes_a_branch(input: impl BufRead) -> bool {
        !branch_updates(input).unwrap().is_empty()
    }

    #[test]
    fn commits_on_protected_branches_are_refused_unless_explicitly_allowed() {
        assert!(protected_branch_refusal("main", None).is_some());
        assert!(protected_branch_refusal("next", Some("0")).is_some());
        assert!(protected_branch_refusal("next", Some("1")).is_none());
        assert!(protected_branch_refusal("tooling/x", None).is_none());
    }

    #[test]
    fn only_branch_updates_trigger_the_push_gate() {
        let a = "a".repeat(40);
        let b = "b".repeat(40);
        let update = format!("refs/heads/x {a} refs/heads/x {b}\n");
        assert!(pushes_a_branch(update.as_bytes()));

        let unchanged = format!("refs/heads/x {a} refs/heads/x {a}\n");
        assert!(!pushes_a_branch(unchanged.as_bytes()));

        let delete = format!("(delete) {ZERO_SHA} refs/heads/x {a}\n");
        assert!(!pushes_a_branch(delete.as_bytes()));

        let tag = format!("refs/tags/v1 {a} refs/tags/v1 {ZERO_SHA}\n");
        assert!(!pushes_a_branch(tag.as_bytes()));

        assert!(!pushes_a_branch(&b""[..]));
    }

    #[test]
    fn malformed_or_unreadable_push_input_is_refused() {
        assert!(branch_updates(&b"malformed\n"[..]).is_err());
        assert!(branch_updates(&b"refs/heads/x invalid refs/heads/x invalid\n"[..]).is_err());
        let unreadable = std::io::BufReader::new(std::io::Read::take(std::io::repeat(0xff), 1));
        assert!(branch_updates(unreadable).is_err());
    }

    #[test]
    fn multiple_refs_keep_every_branch_update_and_skip_non_updates() {
        let a = "a".repeat(40);
        let b = "b".repeat(40);
        let input = format!(
            "refs/heads/a {a} refs/heads/a {b}\nrefs/tags/v1 {a} refs/tags/v1 {ZERO_SHA}\n(delete) {ZERO_SHA} refs/heads/deleted {b}\nrefs/heads/same {a} refs/heads/same {a}\nrefs/heads/b {a} refs/heads/b {ZERO_SHA}\n"
        );
        let updates = branch_updates(input.as_bytes()).unwrap();
        assert_eq!(updates.len(), 2);
        assert_eq!(updates[0].remote_sha, b);
        assert_eq!(updates[1].remote_sha, ZERO_SHA);
    }

    #[test]
    fn every_pushed_head_must_match_the_verified_source() {
        let dir = crate::test_support::fixture_repo_committed(&[("README.md", "docs\n")]);
        let git = crate::git::Git::new(dir.path());
        let head = git.head().unwrap();
        let input = format!(
            "refs/heads/a {head} refs/heads/a {ZERO_SHA}\nrefs/heads/b {} refs/heads/b {ZERO_SHA}\n",
            "b".repeat(40)
        );
        let updates = branch_updates(input.as_bytes()).unwrap();
        assert_eq!(updates.len(), 2);
        assert!(
            pre_push_scope(&git, &updates)
                .unwrap_err()
                .to_string()
                .contains("checked-out HEAD")
        );
    }

    fn commit(root: &std::path::Path) {
        for args in [&["add", "-A"][..], &["commit", "-q", "-m", "fixture"]] {
            assert_eq!(Git::new(root).run(args).0, 0);
        }
    }

    #[test]
    fn branch_scope_includes_deleted_rust_remote_changes_and_working_tooling() {
        let dir = crate::test_support::fixture_repo_committed(&[
            ("tools/tool.rs", "fn main() {}\n"),
            ("README.md", "docs\n"),
        ]);
        let git = Git::new(dir.path());
        let remote = git.head().unwrap();
        std::fs::remove_file(dir.path().join("tools/tool.rs")).unwrap();
        commit(dir.path());
        assert_eq!(
            git.run(&["update-ref", "refs/remotes/origin/next", "HEAD"])
                .0,
            0
        );
        let updates = branch_updates(
            format!(
                "refs/heads/a {} refs/heads/a {remote}\n",
                git.head().unwrap()
            )
            .as_bytes(),
        )
        .unwrap();
        let scope = pre_push_scope(&git, &updates).unwrap();
        assert!(scope.requires_rust);
        assert!(scope.changed_files.contains(&"tools/tool.rs".into()));
        let new_ref = branch_updates(
            format!(
                "refs/heads/a {} refs/heads/a {ZERO_SHA}\n",
                git.head().unwrap()
            )
            .as_bytes(),
        )
        .unwrap();
        crate::test_support::write_files(dir.path(), &[("docs/change.md", "docs\n")]);
        assert!(!pre_push_scope(&git, &new_ref).unwrap().requires_rust);
        crate::test_support::write_files(dir.path(), &[(".cargo/config.toml", "[alias]\n")]);
        assert!(pre_push_scope(&git, &new_ref).unwrap().requires_rust);
    }

    #[test]
    fn absent_integration_base_or_remote_object_escalates_rust() {
        let dir = crate::test_support::fixture_repo_committed(&[("README.md", "docs\n")]);
        let git = Git::new(dir.path());
        let head = git.head().unwrap();
        let updates = branch_updates(
            format!("refs/heads/a {head} refs/heads/a {}\n", "b".repeat(40)).as_bytes(),
        )
        .unwrap();
        assert!(pre_push_scope(&git, &updates).unwrap().requires_rust);
        assert_eq!(
            git.run(&["update-ref", "refs/remotes/origin/next", "HEAD"])
                .0,
            0
        );
        assert!(pre_push_scope(&git, &updates).unwrap().requires_rust);
    }
}
