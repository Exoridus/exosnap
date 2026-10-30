//! Repository facts. Hook variables are scrubbed. Staged verification may bind
//! its candidate index only to the canonical source repository on this thread.

use std::cell::RefCell;
use std::collections::{BTreeSet, HashMap};
use std::ops::Range;
use std::path::{Path, PathBuf};

use crate::process;

#[derive(Clone)]
struct BoundIndex {
    root: PathBuf,
    path: PathBuf,
}

thread_local! {
    static STAGED_INDEX: RefCell<Option<BoundIndex>> = const { RefCell::new(None) };
}

struct RestoreIndex(Option<BoundIndex>);

impl Drop for RestoreIndex {
    fn drop(&mut self) {
        STAGED_INDEX.with(|index| *index.borrow_mut() = self.0.take());
    }
}

pub struct Git {
    root: PathBuf,
    index: Option<PathBuf>,
}

impl Git {
    pub fn new(root: &Path) -> Git {
        let index = STAGED_INDEX.with(|index| {
            let index = index.borrow();
            index.as_ref().and_then(|bound| {
                (root.canonicalize().ok().as_ref() == Some(&bound.root)).then(|| bound.path.clone())
            })
        });
        Git {
            root: root.to_path_buf(),
            index,
        }
    }

    /// Captures Git's candidate index before subprocess environment scrubbing.
    /// The binding applies only to this repository and is restored on exit.
    pub fn with_active_index<T>(root: &Path, action: impl FnOnce() -> T) -> anyhow::Result<T> {
        match std::env::var_os("GIT_INDEX_FILE") {
            Some(index) => Self::with_index(root, Path::new(&index), action),
            None => Ok(action()),
        }
    }

    /// Binds an absolute candidate index to the canonical repository for this
    /// thread. Queries in other repositories retain the scrubbed environment.
    pub fn with_index<T>(
        root: &Path,
        index: &Path,
        action: impl FnOnce() -> T,
    ) -> anyhow::Result<T> {
        let bound = BoundIndex {
            root: root.canonicalize()?,
            path: std::path::absolute(index)?,
        };
        let previous = STAGED_INDEX.with(|index| index.replace(Some(bound)));
        let _restore = RestoreIndex(previous);
        Ok(action())
    }

    /// The candidate index bound to this repository, if any. Pass it only to a
    /// child command that reads this same repository.
    pub fn index_file(&self) -> Option<&Path> {
        self.index.as_deref()
    }

    /// The top level of the work tree containing `start`.
    pub fn discover(start: &Path) -> Option<PathBuf> {
        let mut command = process::command("git");
        command
            .arg("-C")
            .arg(start)
            .args(["rev-parse", "--show-toplevel"]);
        let (code, stdout) = process::query(command).ok()?;
        let top = stdout.trim();
        if code != 0 || top.is_empty() {
            return None;
        }
        // git prints forward slashes; the platform form keeps joined paths uniform.
        Some(std::path::absolute(top).unwrap_or_else(|_| PathBuf::from(top)))
    }

    /// Every file tracked at the current index, `git ls-files` order. Errors
    /// when `root` is not (inside) a git repository: a repo boundary failure is
    /// a refusal, never an empty, falsely-clean file list.
    pub fn ls_files(&self) -> anyhow::Result<Vec<String>> {
        let (code, stdout) = self.run(&["ls-files"]);
        anyhow::ensure!(
            code == 0,
            "git ls-files failed in '{}': is this a git repository?",
            self.root.display()
        );
        Ok(stdout
            .lines()
            .map(str::trim)
            .filter(|line| !line.is_empty())
            .map(str::to_string)
            .collect())
    }

    pub fn run(&self, args: &[&str]) -> (i32, String) {
        process::query(self.command(args)).unwrap_or((-1, String::new()))
    }

    fn command(&self, args: &[&str]) -> std::process::Command {
        let mut command = process::command("git");
        command.arg("-C").arg(&self.root).args(args);
        if let Some(index) = &self.index {
            command.env("GIT_INDEX_FILE", index);
        }
        command
    }

    fn first_line(&self, args: &[&str]) -> Option<String> {
        let (code, stdout) = self.run(args);
        let line = stdout.lines().next()?.trim().to_string();
        (code == 0 && !line.is_empty()).then_some(line)
    }

    pub fn head(&self) -> Option<String> {
        self.first_line(&["rev-parse", "HEAD"])
    }

    pub fn branch(&self) -> Option<String> {
        self.first_line(&["rev-parse", "--abbrev-ref", "HEAD"])
    }

    pub fn dirty(&self) -> bool {
        !self.run(&["status", "--porcelain"]).1.trim().is_empty()
    }

    /// The merge base with the first integration branch that has one.
    pub fn default_base(&self) -> Option<String> {
        ["origin/next", "origin/main", "origin/HEAD", "next", "main"]
            .into_iter()
            .find_map(|candidate| self.first_line(&["merge-base", candidate, "HEAD"]))
    }

    /// Deletion-inclusive paths for verification scope. Missing revisions and
    /// failed Git queries cannot establish that a check is unnecessary.
    pub fn scope_files(&self, base: Option<&str>, staged: bool) -> anyhow::Result<Vec<String>> {
        let mut files = BTreeSet::new();
        if !staged {
            let base = base.ok_or_else(|| anyhow::anyhow!("no trustworthy integration base"))?;
            files.extend(self.revision_scope_files(base, "HEAD")?);
            files.extend(self.working_scope_files()?);
        }
        files.extend(self.scope_paths(&[
            "diff",
            "--cached",
            "--name-only",
            "--no-renames",
            "-z",
        ])?);
        Ok(files.into_iter().collect())
    }

    pub fn revision_scope_files(&self, base: &str, head: &str) -> anyhow::Result<Vec<String>> {
        self.scope_paths(&[
            "diff",
            "--name-only",
            "--no-renames",
            "-z",
            base,
            head,
            "--",
        ])
    }

    pub fn working_scope_files(&self) -> anyhow::Result<Vec<String>> {
        let mut files = self.scope_paths(&["diff", "--name-only", "--no-renames", "-z"])?;
        files.extend(self.scope_paths(&["ls-files", "--others", "--exclude-standard", "-z"])?);
        Ok(files)
    }

    fn scope_paths(&self, args: &[&str]) -> anyhow::Result<Vec<String>> {
        let (code, stdout) = self.run(args);
        anyhow::ensure!(
            code == 0,
            "git {} failed while determining verification scope",
            args.join(" ")
        );
        Ok(stdout
            .split('\0')
            .filter(|path| !path.is_empty())
            .map(str::to_string)
            .collect())
    }

    /// The files this invocation is responsible for. Staged is the pre-commit
    /// question ("what am I about to record?"). Otherwise it is what the branch
    /// adds on top of its base plus anything uncommitted, because an uncommitted
    /// edit still decides what must be rebuilt before its tests mean anything.
    pub fn changed_files(&self, base: Option<&str>, staged: bool) -> Vec<String> {
        let mut files = BTreeSet::new();
        let mut add = |args: &[&str]| {
            for line in self.run(args).1.lines() {
                let line = line.trim();
                if !line.is_empty() {
                    files.insert(line.to_string());
                }
            }
        };
        if staged {
            add(&["diff", "--cached", "--name-only", "--diff-filter=ACMR"]);
        } else {
            if let Some(base) = base {
                let range = format!("{base}...HEAD");
                add(&["diff", "--name-only", "--diff-filter=ACMR", &range]);
            }
            add(&["diff", "--name-only", "--diff-filter=ACMR"]);
            add(&["diff", "--cached", "--name-only", "--diff-filter=ACMR"]);
        }
        files.into_iter().collect()
    }

    /// New/changed line numbers per file, as `old_start..old_start+old_count`
    /// ranges from unified-zero-context hunk headers. Layers three sources the
    /// same way `changed_files` does: the range against `base`, the unstaged
    /// worktree diff and the staged diff, so an uncommitted edit is in scope
    /// even when `base` names `HEAD` itself.
    pub fn changed_line_ranges(
        &self,
        base: &str,
    ) -> anyhow::Result<HashMap<String, Vec<Range<usize>>>> {
        let range = format!("{base}...HEAD");
        // An unresolvable base (a typo, or a shallow CI checkout with no
        // merge-base) must be a refusal, not an empty, falsely-clean diff: the
        // caller would otherwise pass every rule on nothing.
        let (code, base_diff) =
            self.diff_line_ranges(&["diff", "--unified=0", "--diff-filter=ACMR", &range]);
        anyhow::ensure!(
            code == 0,
            "git diff {range} failed in '{}': is '{base}' a valid ref reachable from HEAD?",
            self.root.display()
        );
        let mut changed = base_diff;
        let (_, worktree_diff) =
            self.diff_line_ranges(&["diff", "--unified=0", "--diff-filter=ACMR"]);
        for (file, ranges) in worktree_diff {
            changed.entry(file).or_default().extend(ranges);
        }
        let (_, staged_diff) =
            self.diff_line_ranges(&["diff", "--cached", "--unified=0", "--diff-filter=ACMR"]);
        for (file, ranges) in staged_diff {
            changed.entry(file).or_default().extend(ranges);
        }
        Ok(changed)
    }

    /// Returns the invoked git command's exit code alongside the parsed
    /// ranges: a caller scoped to a specific `base` needs to tell "no changes"
    /// apart from "the diff never ran."
    fn diff_line_ranges(&self, args: &[&str]) -> (i32, HashMap<String, Vec<Range<usize>>>) {
        // A hunk header with no explicit count means one line; an explicit `,0`
        // means a pure deletion, which touches no line on the new side.
        let hunk = regex::Regex::new(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@").unwrap();
        let mut changed: HashMap<String, Vec<Range<usize>>> = HashMap::new();
        let (code, stdout) = self.run(args);
        let mut file: Option<String> = None;
        for line in stdout.lines() {
            if let Some(rest) = line.strip_prefix("+++ b/") {
                file = Some(rest.trim().to_string());
                continue;
            }
            let Some(caps) = hunk.captures(line) else {
                continue;
            };
            let Some(file) = &file else { continue };
            let start: usize = caps[1].parse().unwrap_or(1);
            let count: usize = caps.get(2).map_or(1, |m| m.as_str().parse().unwrap_or(1));
            if count > 0 {
                changed
                    .entry(file.clone())
                    .or_default()
                    .push(start..start + count);
            }
        }
        (code, changed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::fixture_repo_committed;

    #[test]
    fn an_unresolvable_base_is_a_hard_error_not_an_empty_diff() {
        let dir = fixture_repo_committed(&[("app/x.cpp", "int x = 0;\n")]);
        let git = Git::new(dir.path());
        let result = git.changed_line_ranges("not-a-real-ref");
        assert!(
            result.is_err(),
            "an unresolvable base must refuse, not silently report zero changed lines"
        );
    }

    #[test]
    fn scope_paths_include_deleted_files_and_fail_on_bad_revisions() {
        let dir = fixture_repo_committed(&[("tools/tool.rs", "fn main() {}\n")]);
        std::fs::remove_file(dir.path().join("tools/tool.rs")).unwrap();
        let git = Git::new(dir.path());
        assert!(
            git.scope_files(Some("HEAD"), false)
                .unwrap()
                .contains(&"tools/tool.rs".into())
        );
        assert!(git.scope_files(Some("missing-ref"), false).is_err());
    }

    #[test]
    fn candidate_index_is_root_bound_and_context_restores_after_unwind() {
        let dir = fixture_repo_committed(&[("original.md", "original\n")]);
        let foreign = fixture_repo_committed(&[("foreign.md", "foreign\n")]);
        let candidate = dir.path().join(".git/candidate-index");
        std::fs::copy(dir.path().join(".git/index"), &candidate).unwrap();
        std::fs::write(dir.path().join("candidate.md"), "candidate\n").unwrap();
        let mut command = process::command("git");
        command
            .arg("-C")
            .arg(dir.path())
            .args(["add", "candidate.md"])
            .env("GIT_INDEX_FILE", &candidate);
        assert_eq!(process::query(command).unwrap().0, 0);
        let inherited = std::env::var_os("GIT_INDEX_FILE");
        let result = std::panic::catch_unwind(|| {
            Git::with_index(dir.path(), &candidate, || {
                assert!(
                    Git::new(dir.path())
                        .ls_files()
                        .unwrap()
                        .contains(&"candidate.md".into())
                );
                assert_eq!(Git::new(foreign.path()).ls_files().unwrap(), ["foreign.md"]);
                let mut command = process::command("git");
                command.arg("-C").arg(foreign.path()).args(["ls-files"]);
                assert_eq!(process::query(command).unwrap().1.trim(), "foreign.md");
                panic!("exercise index context restoration");
            })
            .unwrap();
        });
        assert!(result.is_err());
        assert_eq!(Git::new(dir.path()).ls_files().unwrap(), ["original.md"]);
        assert_eq!(std::env::var_os("GIT_INDEX_FILE"), inherited);
    }
}
