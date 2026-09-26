//! Runs clang-format over the tracked or staged C++ source in `libs/`, `app/`
//! and `tests/`.
//!
//! `--staged --fix` refuses to autoformat a file that is only partially
//! staged: formatting the whole file and re-staging it would silently
//! discard the intent behind whatever the developer left unstaged. The file
//! list is chunked, because passing every source file to clang-format in one
//! invocation eventually exceeds the command-line length limit. The file
//! list grows with the repository, and a deep checkout path pushes it over
//! sooner.

use std::path::Path;

use anyhow::{Context, bail};

use crate::executor::ToolMissing;
use crate::git::Git;

const SCOPED_DIRS: [&str; 3] = ["libs/", "app/", "tests/"];
const BATCH_SIZE: usize = 100;

pub struct FormatReport {
    /// "staged" or "tracked", for reporting.
    pub scope: &'static str,
    pub files: Vec<String>,
    /// Set only outside fix mode: clang-format --dry-run --Werror found at
    /// least one violation.
    pub violations: bool,
    /// Set only in fix mode, once every batch formatted cleanly.
    pub fixed: bool,
    /// Combined clang-format stdout/stderr across every batch it ran.
    pub output: String,
}

impl FormatReport {
    pub fn ok(&self) -> bool {
        !self.violations
    }
}

pub fn format(repo_root: &Path, staged: bool, fix: bool) -> anyhow::Result<FormatReport> {
    let clang_format = crate::lint::find_tool(&["clang-format"]).ok_or_else(|| {
        ToolMissing("clang-format.exe not found on PATH, VS LLVM, or LLVM install.".into())
    })?;
    run(repo_root, staged, fix, |args, batch| {
        invoke(&clang_format, repo_root, args, batch)
    })
}

/// `format`'s logic with clang-format's invocation injected, so a test can
/// exercise the file selection, overlap refusal and re-staging without a real
/// clang-format binary.
fn run(
    repo_root: &Path,
    staged: bool,
    fix: bool,
    mut invoke: impl FnMut(&[&str], &[String]) -> anyhow::Result<(i32, String)>,
) -> anyhow::Result<FormatReport> {
    let git = Git::new(repo_root);
    let scope = if staged { "staged" } else { "tracked" };
    let files = source_files(&git, repo_root, staged);

    if files.is_empty() {
        return Ok(FormatReport {
            scope,
            files,
            violations: false,
            fixed: false,
            output: String::new(),
        });
    }

    if fix && staged {
        refuse_partially_staged_overlap(&git, &files)?;
    }

    let mut output = String::new();
    let args: &[&str] = if fix {
        &["-i"]
    } else {
        &["--dry-run", "--Werror"]
    };
    let mut code = 0;
    for batch in files.chunks(BATCH_SIZE) {
        let (batch_code, batch_output) = invoke(args, batch)?;
        output.push_str(&batch_output);
        if batch_code != 0 {
            code = batch_code;
            break;
        }
    }

    if fix {
        if code != 0 {
            bail!("clang-format failed (exit {code}).\n{output}");
        }
        if staged {
            restage(&git, &files)?;
        }
        Ok(FormatReport {
            scope,
            files,
            violations: false,
            fixed: true,
            output,
        })
    } else {
        Ok(FormatReport {
            scope,
            files,
            violations: code != 0,
            fixed: false,
            output,
        })
    }
}

fn refuse_partially_staged_overlap(git: &Git, staged_files: &[String]) -> anyhow::Result<()> {
    let unstaged = unstaged_cpp_files(git);
    let overlap: Vec<&String> = staged_files
        .iter()
        .filter(|f| unstaged.iter().any(|u| u.eq_ignore_ascii_case(f)))
        .collect();
    if overlap.is_empty() {
        return Ok(());
    }
    let mut shown: Vec<&str> = overlap.iter().take(8).map(|f| f.as_str()).collect();
    if overlap.len() > 8 {
        shown.push("...");
    }
    bail!(
        "Cannot autoformat staged files that also have unstaged edits: {}. Stage the full file or \
         run `cargo exo-dev check format --fix` manually.",
        shown.join(", ")
    );
}

fn restage(git: &Git, files: &[String]) -> anyhow::Result<()> {
    let mut args = vec!["add", "--"];
    args.extend(files.iter().map(String::as_str));
    let (code, _) = git.run(&args);
    if code != 0 {
        bail!("git add failed after clang-format.");
    }
    Ok(())
}

fn source_files(git: &Git, repo_root: &Path, staged: bool) -> Vec<String> {
    let (_, stdout) = if staged {
        let mut args = vec![
            "diff",
            "--cached",
            "--name-only",
            "--diff-filter=ACMR",
            "--",
        ];
        args.extend(SCOPED_DIRS);
        git.run(&args)
    } else {
        let mut args = vec!["ls-files", "--"];
        args.extend(SCOPED_DIRS);
        git.run(&args)
    };
    stdout
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .filter(|l| is_cpp_source(l))
        .filter(|l| staged || repo_root.join(l).is_file())
        .map(str::to_string)
        .collect()
}

fn unstaged_cpp_files(git: &Git) -> Vec<String> {
    let mut args = vec!["diff", "--name-only", "--"];
    args.extend(SCOPED_DIRS);
    let (_, stdout) = git.run(&args);
    stdout
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .filter(|l| is_cpp_source(l))
        .map(str::to_string)
        .collect()
}

fn is_cpp_source(path: &str) -> bool {
    let lower = path.to_ascii_lowercase();
    lower.ends_with(".cpp") || lower.ends_with(".h")
}

/// Runs clang-format for one batch, capturing stdout and stderr together:
/// `--dry-run --Werror` reports violations on stderr, and a fix-mode parse
/// error goes there too, so `process::query` (which discards stderr) would
/// silently drop the diagnostic the caller needs to act on.
fn invoke(
    tool: &Path,
    repo_root: &Path,
    args: &[&str],
    files: &[String],
) -> anyhow::Result<(i32, String)> {
    let mut command = crate::process::command(&tool.to_string_lossy());
    command.current_dir(repo_root).args(args).args(files);
    let output = command
        .output()
        .with_context(|| format!("could not run {}", tool.display()))?;
    let mut combined = String::from_utf8_lossy(&output.stdout).into_owned();
    combined.push_str(&String::from_utf8_lossy(&output.stderr));
    Ok((output.status.code().unwrap_or(-1), combined))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::{fixture_repo_committed, write_files};
    use std::cell::RefCell;

    fn run_git(dir: &Path, args: &[&str]) {
        let status = std::process::Command::new("git")
            .args(args)
            .current_dir(dir)
            .status()
            .unwrap();
        assert!(status.success(), "git {args:?} failed");
    }

    /// A fake clang-format: records every batch it was called with and
    /// returns a pre-scripted (exit code, output) pair per call, in order.
    struct FakeInvocations {
        calls: RefCell<Vec<(Vec<String>, Vec<String>)>>,
        results: RefCell<Vec<(i32, String)>>,
    }

    impl FakeInvocations {
        fn always_ok() -> FakeInvocations {
            FakeInvocations {
                calls: RefCell::new(Vec::new()),
                results: RefCell::new(vec![(0, String::new())]),
            }
        }

        fn scripted(results: Vec<(i32, String)>) -> FakeInvocations {
            FakeInvocations {
                calls: RefCell::new(Vec::new()),
                results: RefCell::new(results),
            }
        }

        fn invoke(&self, args: &[&str], batch: &[String]) -> anyhow::Result<(i32, String)> {
            self.calls
                .borrow_mut()
                .push((args.iter().map(|a| a.to_string()).collect(), batch.to_vec()));
            let mut results = self.results.borrow_mut();
            Ok(if results.len() > 1 {
                results.remove(0)
            } else {
                results[0].clone()
            })
        }
    }

    #[test]
    fn no_source_files_in_scope_is_reported_as_skipped_not_an_error() {
        let dir = fixture_repo_committed(&[("docs/readme.md", "hi\n")]);
        let fake = FakeInvocations::always_ok();
        let report = run(dir.path(), false, false, |a, b| fake.invoke(a, b)).unwrap();
        assert_eq!(report.files, Vec::<String>::new());
        assert!(!report.violations);
        assert!(fake.calls.borrow().is_empty());
    }

    #[test]
    fn dry_run_reports_violations_without_invoking_a_fix() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x=0;\n")]);
        let fake = FakeInvocations::scripted(vec![(1, "thing.cpp:1: violation\n".into())]);
        let report = run(dir.path(), false, false, |a, b| fake.invoke(a, b)).unwrap();
        assert!(report.violations);
        assert!(!report.fixed);
        let calls = fake.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert_eq!(
            calls[0].0,
            vec!["--dry-run".to_string(), "--Werror".to_string()]
        );
    }

    #[test]
    fn a_clean_dry_run_reports_no_violations() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        let fake = FakeInvocations::always_ok();
        let report = run(dir.path(), false, false, |a, b| fake.invoke(a, b)).unwrap();
        assert!(!report.violations);
        assert!(report.ok());
    }

    #[test]
    fn staged_fix_formats_and_restages_the_staged_files() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        // Staged with deliberately bad formatting, so the re-formatted
        // content differs from what is currently in the index.
        write_files(dir.path(), &[("app/thing.cpp", "int   x=0;\n")]);
        run_git(dir.path(), &["add", "-A"]);

        // Stands in for clang-format -i: rewrites the file on disk, the same
        // way the real tool would.
        let repo_root = dir.path().to_path_buf();
        let invoke = |_args: &[&str], batch: &[String]| -> anyhow::Result<(i32, String)> {
            for file in batch {
                std::fs::write(repo_root.join(file), "int x = 0;\n").unwrap();
            }
            Ok((0, String::new()))
        };

        let report = run(dir.path(), true, true, invoke).unwrap();
        assert!(report.fixed);
        assert_eq!(report.files, vec!["app/thing.cpp".to_string()]);

        // Without restage(), the index would still hold the badly-formatted
        // content that was staged before the fix ran, not what clang-format
        // wrote to disk.
        let indexed = std::process::Command::new("git")
            .args(["show", ":app/thing.cpp"])
            .current_dir(dir.path())
            .output()
            .unwrap();
        assert_eq!(String::from_utf8_lossy(&indexed.stdout), "int x = 0;\n");
    }

    #[test]
    fn a_staged_file_with_further_unstaged_edits_on_top_is_refused() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x=0;\n")]);
        write_files(dir.path(), &[("app/thing.cpp", "int x = 0;\n")]);
        run_git(dir.path(), &["add", "-A"]);
        // A further edit on top of the staged change, left unstaged.
        write_files(dir.path(), &[("app/thing.cpp", "int x = 0; // more\n")]);

        let fake = FakeInvocations::always_ok();
        let result = run(dir.path(), true, true, |a, b| fake.invoke(a, b));
        assert!(result.is_err());
        assert!(
            fake.calls.borrow().is_empty(),
            "clang-format must not run before the refusal"
        );
    }

    #[test]
    fn non_staged_fix_does_not_require_staged_semantics() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x=0;\n")]);
        write_files(dir.path(), &[("app/thing.cpp", "int x = 0;\n")]);
        // Deliberately not staged: a whole-tree --fix formats every tracked
        // file in place without touching the index.
        let fake = FakeInvocations::always_ok();
        let report = run(dir.path(), false, true, |a, b| fake.invoke(a, b)).unwrap();
        assert!(report.fixed);
    }

    #[test]
    fn a_failing_batch_in_fix_mode_is_an_error() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x=0;\n")]);
        let fake = FakeInvocations::scripted(vec![(1, "clang-format: parse error\n".into())]);
        let result = run(dir.path(), false, true, |a, b| fake.invoke(a, b));
        assert!(result.is_err());
    }

    #[test]
    fn only_scoped_directories_are_considered() {
        let dir = fixture_repo_committed(&[
            ("app/thing.cpp", "int x = 0;\n"),
            ("docs/thing.cpp", "int y = 0;\n"),
        ]);
        let fake = FakeInvocations::always_ok();
        let report = run(dir.path(), false, false, |a, b| fake.invoke(a, b)).unwrap();
        assert_eq!(report.files, vec!["app/thing.cpp".to_string()]);
    }

    #[test]
    fn only_cpp_and_h_extensions_are_considered() {
        let dir = fixture_repo_committed(&[
            ("app/thing.cpp", "int x = 0;\n"),
            ("app/thing.h", "void f();\n"),
            ("app/thing.qml", "Item {}\n"),
        ]);
        let fake = FakeInvocations::always_ok();
        let report = run(dir.path(), false, false, |a, b| fake.invoke(a, b)).unwrap();
        let mut files = report.files;
        files.sort();
        assert_eq!(
            files,
            vec!["app/thing.cpp".to_string(), "app/thing.h".to_string()]
        );
    }
}
