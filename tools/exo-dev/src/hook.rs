//! Git hook entry points. The hook scripts only locate exo-dev; what runs, in what
//! order and what a failure stops is decided here and in the profiles.

use std::io::BufRead;

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

/// Whether a pre-push ref list updates any branch. Tag-only pushes and deletions
/// are not verified.
pub fn pushes_a_branch(input: impl BufRead) -> bool {
    input.lines().map_while(Result::ok).any(|line| {
        let fields: Vec<&str> = line.split_whitespace().collect();
        let [local_ref, local_sha, remote_ref, remote_sha] = fields[..] else {
            return false;
        };
        (local_ref.starts_with("refs/heads/") || remote_ref.starts_with("refs/heads/"))
            && local_sha != ZERO_SHA
            && local_sha != remote_sha
    })
}

#[cfg(test)]
mod tests {
    use super::*;

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
}
