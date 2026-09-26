//! Repository facts. Every call names its repository and runs without the hook's
//! git environment (see `process::HOOK_GIT_VARIABLES`).

use std::collections::{BTreeSet, HashMap};
use std::ops::Range;
use std::path::{Path, PathBuf};

use crate::process;

pub struct Git {
    root: PathBuf,
}

impl Git {
    pub fn new(root: &Path) -> Git {
        Git {
            root: root.to_path_buf(),
        }
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
        let mut command = process::command("git");
        command.arg("-C").arg(&self.root).args(args);
        process::query(command).unwrap_or((-1, String::new()))
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
        let mut changed =
            self.diff_line_ranges(&["diff", "--unified=0", "--diff-filter=ACMR", &range]);
        for (file, ranges) in self.diff_line_ranges(&["diff", "--unified=0", "--diff-filter=ACMR"])
        {
            changed.entry(file).or_default().extend(ranges);
        }
        for (file, ranges) in
            self.diff_line_ranges(&["diff", "--cached", "--unified=0", "--diff-filter=ACMR"])
        {
            changed.entry(file).or_default().extend(ranges);
        }
        Ok(changed)
    }

    fn diff_line_ranges(&self, args: &[&str]) -> HashMap<String, Vec<Range<usize>>> {
        // A hunk header with no explicit count means one line; an explicit `,0`
        // means a pure deletion, which touches no line on the new side.
        let hunk = regex::Regex::new(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@").unwrap();
        let mut changed: HashMap<String, Vec<Range<usize>>> = HashMap::new();
        let (_, stdout) = self.run(args);
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
        changed
    }
}
