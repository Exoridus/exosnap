//! Repository facts. Every call names its repository and runs without the hook's
//! git environment (see `process::HOOK_GIT_VARIABLES`).

use std::collections::BTreeSet;
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
}
