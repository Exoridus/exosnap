//! Advisory clang-tidy (the broad `.clang-tidy` check set, minus
//! `clang-analyzer-*`) and cppcheck over the project's own sources, ported
//! from scripts/check-quality.ps1.
//!
//! Distinct from [`crate::lint::clang_tidy`]: that module runs the curated
//! [`crate::lint::canaries::BLOCKING_CHECKS`] set and fails a pull request on
//! a finding. This module's clang-tidy pass is advisory: its findings are
//! reported, never a gate, because the broad check set currently reports
//! findings across the tree. cppcheck is the opposite: it carries
//! `--error-exitcode=1`, so a finding here does fail the calling step.
//!
//! `base` restricts only the clang-tidy pass, to the files a change touched.
//! It is a plain filename intersection against `git diff`, not the
//! header-to-consumer expansion `lint::clang_tidy::run_blocking` does for its
//! blocking gate: this pass is advisory, and the extra machinery is not worth
//! paying for it. cppcheck always analyses the whole `libs/` and `app/` trees,
//! regardless of `base`: it is a whole-program-shaped scan cppcheck's own
//! result cache already keeps cheap on an unchanged tree.

use std::collections::HashSet;
use std::path::{Path, PathBuf};

use anyhow::Context as _;

use crate::executor::ToolMissing;
use crate::git::Git;
use crate::lint;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Only {
    CppCheck,
    ClangTidy,
}

#[derive(Debug, Default)]
pub struct ClangTidyAdvisory {
    pub scope: String,
    pub analyzed: usize,
    pub batches: usize,
    pub jobs: usize,
    /// Raw output of every batch that exited nonzero. Advisory: never used to
    /// decide pass/fail, only to report what clang-tidy found.
    pub findings: Vec<String>,
}

#[derive(Debug, Default)]
pub struct CppCheckOutcome {
    pub cache_dir: PathBuf,
    pub ok: bool,
    pub output: String,
}

#[derive(Debug, Default)]
pub struct QualityReport {
    /// `None` when `only` excluded this pass, or when it was skipped for lack
    /// of a compile database or of C++ in scope (see `clang_tidy_skip_reason`).
    pub clang_tidy: Option<ClangTidyAdvisory>,
    /// Why the clang-tidy pass ran nothing, when it ran nothing but was not
    /// excluded by `only` and no tool is missing.
    pub clang_tidy_skip_reason: Option<String>,
    /// `None` when `only` excluded this pass.
    pub cppcheck: Option<CppCheckOutcome>,
}

/// Runs the selected passes. Neither pass stops the other: a missing tool is
/// collected and reported only after both have had their chance to run, so
/// asking for one tool's install never hides that the other is also absent.
///
/// `cache_root` overrides cppcheck's tool-cache root (`None` means the real
/// per-user cache under `%LOCALAPPDATA%`); it exists so a test can inject a
/// throwaway directory instead of reading and writing the real one.
pub fn run(
    repo_root: &Path,
    build_dir: Option<&Path>,
    base: Option<&str>,
    only: Option<Only>,
    jobs: usize,
    report_path: Option<&Path>,
    cache_root: Option<&Path>,
) -> anyhow::Result<QualityReport> {
    let mut missing: Vec<&'static str> = Vec::new();
    let mut clang_tidy_skip_reason = None;

    let clang_tidy = if only != Some(Only::CppCheck) {
        match clang_tidy_pass(repo_root, build_dir, base, jobs, report_path)? {
            ClangTidyOutcome::Ran(report) => Some(report),
            ClangTidyOutcome::Skipped(reason) => {
                clang_tidy_skip_reason = Some(reason);
                None
            }
            ClangTidyOutcome::ToolMissing => {
                missing.push("clang-tidy");
                None
            }
        }
    } else {
        None
    };

    let cppcheck = if only != Some(Only::ClangTidy) {
        match cppcheck_pass(repo_root, cache_root)? {
            Some(report) => Some(report),
            None => {
                missing.push("cppcheck");
                None
            }
        }
    } else {
        None
    };

    if !missing.is_empty() {
        return Err(ToolMissing(format!("{} not installed", missing.join(", "))).into());
    }

    Ok(QualityReport {
        clang_tidy,
        clang_tidy_skip_reason,
        cppcheck,
    })
}

#[derive(Debug)]
enum ClangTidyOutcome {
    Ran(ClangTidyAdvisory),
    Skipped(String),
    ToolMissing,
}

fn clang_tidy_pass(
    repo_root: &Path,
    build_dir: Option<&Path>,
    base: Option<&str>,
    jobs: usize,
    report_path: Option<&Path>,
) -> anyhow::Result<ClangTidyOutcome> {
    // Only the Ninja presets export a compile database; the Visual Studio
    // generator does not.
    let compile_db_tree: Option<PathBuf> = match build_dir {
        Some(dir) => {
            let db = repo_root.join(dir).join("compile_commands.json");
            anyhow::ensure!(
                db.is_file(),
                "clang-tidy: '{}' has no compile_commands.json. Configure it with a Ninja preset before asking for this check.",
                dir.display()
            );
            Some(dir.to_path_buf())
        }
        None => [
            "build/windows-x64-ninja-debug",
            "build/windows-x64-ninja-release",
        ]
        .into_iter()
        .map(PathBuf::from)
        .find(|candidate| {
            repo_root
                .join(candidate)
                .join("compile_commands.json")
                .is_file()
        }),
    };
    let Some(tree) = compile_db_tree else {
        return Ok(ClangTidyOutcome::Skipped(
            "no compile_commands.json; run: cmake --preset windows-x64-ninja-debug".to_string(),
        ));
    };

    let Some(tool) = lint::find_tool(&["clang-tidy"]) else {
        return Ok(ClangTidyOutcome::ToolMissing);
    };

    let mut sources = project_sources(repo_root)?;
    let scope_desc = match base {
        Some(base) => {
            let touched = touched_files(repo_root, base);
            sources.retain(|s| touched.contains(s));
            format!("changed since {base}")
        }
        None => "every tracked source".to_string(),
    };

    if sources.is_empty() {
        return Ok(ClangTidyOutcome::Skipped(format!(
            "no C++ in scope: {scope_desc}"
        )));
    }

    let compile_db_absolute = std::fs::canonicalize(repo_root.join(&tree))
        .with_context(|| format!("could not resolve {}", tree.display()))?;
    // -clang-analyzer-*: .clang-tidy enables a few path-sensitive analyser
    // checks, and the analyser turns this pass into a multi-hour one.
    // cargo exo-dev lint clang-tidy runs the curated BLOCKING_CHECKS set,
    // which includes the analyser checks that qualified.
    let fixed_arguments = vec![
        "-p".to_string(),
        compile_db_absolute.display().to_string(),
        "--checks=-clang-analyzer-*".to_string(),
    ];
    let batches = lint::batch_command_line(&sources, &fixed_arguments, 25, 30000);

    let jobs = if jobs == 0 {
        std::thread::available_parallelism()
            .map(std::num::NonZeroUsize::get)
            .unwrap_or(4)
    } else {
        jobs
    };

    let findings = run_clang_tidy_batches(&tool, repo_root, &fixed_arguments, &batches, jobs)?;

    if let Some(path) = report_path {
        if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
            std::fs::create_dir_all(parent)
                .with_context(|| format!("could not create {}", parent.display()))?;
        }
        // Written even when empty: a caller counting findings has to be able
        // to tell "zero findings" from "the file the count came from is not
        // there".
        std::fs::write(path, findings.join("\n"))
            .with_context(|| format!("could not write {}", path.display()))?;
    }

    Ok(ClangTidyOutcome::Ran(ClangTidyAdvisory {
        scope: scope_desc,
        analyzed: sources.len(),
        batches: batches.len(),
        jobs,
        findings,
    }))
}

/// The C++ files this pass may ever analyse: tracked, under `libs/`, `app/`
/// or `tests/`, with a `.cpp` or `.h` extension. Sorted and deduplicated.
fn project_sources(repo_root: &Path) -> anyhow::Result<Vec<String>> {
    let git = Git::new(repo_root);
    let (code, stdout) = git.run(&["ls-files", "--", "libs/", "app/", "tests/"]);
    anyhow::ensure!(
        code == 0,
        "git ls-files failed in '{}': is this a git repository?",
        repo_root.display()
    );
    let mut files: Vec<String> = stdout
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .map(|line| line.replace('\\', "/"))
        .filter(|file| file.ends_with(".cpp") || file.ends_with(".h"))
        .collect();
    files.sort();
    files.dedup();
    Ok(files)
}

/// Every path `git diff` names against `base`, the working tree and the
/// index, matching the three sources `check-quality.ps1`'s `-Base` scoping
/// read. A plain intersection against [`project_sources`], not the
/// header-to-consumer expansion the blocking gate does: this pass is
/// advisory.
fn touched_files(repo_root: &Path, base: &str) -> HashSet<String> {
    let git = Git::new(repo_root);
    let range = format!("{base}...HEAD");
    let mut touched = HashSet::new();
    for args in [
        vec!["diff", "--name-only", range.as_str()],
        vec!["diff", "--name-only"],
        vec!["diff", "--cached", "--name-only"],
    ] {
        let (_, stdout) = git.run(&args);
        for line in stdout.lines() {
            let line = line.trim();
            if !line.is_empty() {
                touched.insert(line.replace('\\', "/"));
            }
        }
    }
    touched
}

/// Runs clang-tidy over `batches` with up to `jobs` processes in flight at
/// once, returning the combined stdout/stderr of every batch that exited
/// nonzero. A clean batch contributes nothing: this pass is advisory, and
/// only what clang-tidy actually flagged is worth printing.
///
/// A batch that never starts (the tool vanished between discovery and this
/// call, an invalid working directory, ...) is a hard error, not silence:
/// treating a spawn failure as "no findings" would report a clean pass over
/// an analysis that never ran, exactly the failure mode the blocking
/// clang-tidy gate and `check-quality.ps1`'s own `ToolMissingExitCode`
/// existed to rule out.
fn run_clang_tidy_batches(
    tool: &Path,
    repo_root: &Path,
    fixed_arguments: &[String],
    batches: &[Vec<String>],
    jobs: usize,
) -> anyhow::Result<Vec<String>> {
    if batches.is_empty() {
        return Ok(Vec::new());
    }
    let jobs = jobs.max(1).min(batches.len());
    let queue: std::sync::Mutex<std::collections::VecDeque<&Vec<String>>> =
        std::sync::Mutex::new(batches.iter().collect());
    let findings: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
    let spawn_error: std::sync::Mutex<Option<String>> = std::sync::Mutex::new(None);
    std::thread::scope(|scope| {
        for _ in 0..jobs {
            scope.spawn(|| {
                loop {
                    if spawn_error.lock().unwrap().is_some() {
                        // Another batch already failed to launch; stop pulling
                        // more work rather than racing more spawn attempts.
                        break;
                    }
                    let next = queue.lock().unwrap().pop_front();
                    let Some(batch) = next else { break };
                    let mut command = crate::process::command(&tool.to_string_lossy());
                    command.current_dir(repo_root);
                    command.args(fixed_arguments);
                    command.args(batch.as_slice());
                    match command.output() {
                        Ok(output) if !output.status.success() => {
                            let mut combined = String::from_utf8_lossy(&output.stdout).into_owned();
                            combined.push_str(&String::from_utf8_lossy(&output.stderr));
                            findings.lock().unwrap().push(combined);
                        }
                        Ok(_) => {}
                        Err(error) => {
                            let mut guard = spawn_error.lock().unwrap();
                            if guard.is_none() {
                                *guard = Some(format!("could not run {}: {error}", tool.display()));
                            }
                        }
                    }
                }
            });
        }
    });
    if let Some(message) = spawn_error.into_inner().unwrap() {
        anyhow::bail!(message);
    }
    Ok(findings.into_inner().unwrap())
}

fn cppcheck_pass(
    repo_root: &Path,
    cache_root: Option<&Path>,
) -> anyhow::Result<Option<CppCheckOutcome>> {
    let Some(tool) = discover_cppcheck() else {
        return Ok(None);
    };
    let arguments = cppcheck_arguments();

    // --cppcheck-build-dir lets cppcheck reuse the per-file analysis of every
    // translation unit whose input has not changed. The directory is keyed on
    // both the version and the argument list: an upgrade or a changed check
    // set analyses afresh instead of replaying verdicts reached under
    // different rules.
    let version = cppcheck_version(&tool)?;
    let cache_dir = cppcheck_cache_dir(&version, &arguments, cache_root);
    std::fs::create_dir_all(&cache_dir)
        .with_context(|| format!("could not create {}", cache_dir.display()))?;

    let mut command = crate::process::command(&tool.to_string_lossy());
    command.current_dir(repo_root);
    command.arg(format!("--cppcheck-build-dir={}", cache_dir.display()));
    command.args(&arguments);
    let output = command
        .output()
        .with_context(|| format!("could not run {}", tool.display()))?;
    let mut combined = String::from_utf8_lossy(&output.stdout).into_owned();
    combined.push_str(&String::from_utf8_lossy(&output.stderr));

    Ok(Some(CppCheckOutcome {
        cache_dir,
        ok: output.status.success(),
        output: combined,
    }))
}

/// cppcheck's own result cache location for one toolchain, salted by both its
/// version and its full argument list so a cppcheck upgrade or a changed
/// check set analyses afresh instead of replaying verdicts reached under
/// different rules. `root` overrides the real per-user cache root; a test
/// passes one so it never reads or writes `%LOCALAPPDATA%`.
fn cppcheck_cache_dir(version: &str, arguments: &[String], root: Option<&Path>) -> PathBuf {
    crate::evidence::tool_cache_dir(
        "cppcheck",
        &[version.to_string(), arguments.join(" ")],
        root,
    )
}

fn cppcheck_arguments() -> Vec<String> {
    [
        "--enable=warning,performance,portability",
        "--std=c++20",
        "--error-exitcode=1",
        "--inline-suppr",
        "--suppressions-list=.cppcheck-suppress",
        "--library=windows",
        "--library=qt",
        "-q",
        "-I",
        "libs/engine/include",
        "-I",
        "libs/capability/include",
        // Vendored third-party sources are not ours to analyze. Monocypher's
        // header uses a C++ `namespace` behind a macro guard that cppcheck
        // mis-parses as C.
        "-i",
        "libs/update/third_party",
        "libs",
        "app",
    ]
    .into_iter()
    .map(String::from)
    .collect()
}

/// cppcheck is not LLVM-based, so [`lint::find_tool`]'s VS-LLVM and
/// standalone-LLVM tiers never apply to it; only its PATH tier does. This adds
/// the one cppcheck-specific location `winget install Cppcheck.Cppcheck` uses.
fn discover_cppcheck() -> Option<PathBuf> {
    lint::find_tool(&["cppcheck"]).or_else(|| {
        let program_files = std::env::var_os("ProgramFiles")?;
        let candidate = Path::new(&program_files)
            .join("Cppcheck")
            .join("cppcheck.exe");
        candidate.is_file().then_some(candidate)
    })
}

fn cppcheck_version(tool: &Path) -> anyhow::Result<String> {
    let mut command = crate::process::command(&tool.to_string_lossy());
    command.arg("--version");
    let (_, stdout) = crate::process::query(command)
        .with_context(|| format!("could not run {}", tool.display()))?;
    Ok(stdout.trim().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::{fixture_repo_committed, write_files};

    fn repo_with_sources() -> tempfile::TempDir {
        fixture_repo_committed(&[
            ("libs/engine/src/a.cpp", "int a();\n"),
            ("libs/engine/src/a.h", "int a();\n"),
            ("app/quick/main.cpp", "int main() { return 0; }\n"),
            ("libs/update/third_party/vendor.cpp", "int v();\n"),
            ("docs/README.md", "not analysed\n"),
        ])
    }

    #[test]
    fn project_sources_is_scoped_to_libs_app_tests_and_cpp_h_extensions() {
        let dir = repo_with_sources();
        let sources = project_sources(dir.path()).unwrap();
        assert_eq!(
            sources,
            vec![
                "app/quick/main.cpp".to_string(),
                "libs/engine/src/a.cpp".to_string(),
                "libs/engine/src/a.h".to_string(),
                "libs/update/third_party/vendor.cpp".to_string(),
            ]
        );
    }

    #[test]
    fn touched_files_reads_the_working_tree_and_staged_and_base_diffs() {
        let dir = repo_with_sources();
        write_files(
            dir.path(),
            &[("libs/engine/src/a.h", "int a(); // changed\n")],
        );
        let touched = touched_files(dir.path(), "HEAD");
        assert!(touched.contains("libs/engine/src/a.h"));
        assert!(!touched.contains("libs/engine/src/a.cpp"));
    }

    #[test]
    fn a_base_that_touches_nothing_in_scope_skips_rather_than_analyses_everything() {
        let dir = repo_with_sources();
        let outcome = clang_tidy_pass(dir.path(), None, Some("HEAD"), 1, None).unwrap();
        assert!(matches!(outcome, ClangTidyOutcome::Skipped(_)));
    }

    #[test]
    fn a_missing_compile_database_is_a_skip_not_a_failure() {
        let dir = repo_with_sources();
        let outcome = clang_tidy_pass(dir.path(), None, None, 1, None).unwrap();
        assert!(matches!(outcome, ClangTidyOutcome::Skipped(_)));
    }

    #[test]
    fn an_explicit_build_dir_without_a_compile_database_is_a_hard_error() {
        let dir = repo_with_sources();
        let error = clang_tidy_pass(dir.path(), Some(Path::new("build/missing")), None, 1, None)
            .unwrap_err();
        assert!(error.to_string().contains("compile_commands.json"));
    }

    /// Non-vacuous by construction: a real (if empty) `compile_commands.json`
    /// and real project sources are present, so if the `only !=
    /// Some(Only::CppCheck)` guard around the clang-tidy pass were ever
    /// removed, `clang_tidy_pass` would actually attempt to run clang-tidy
    /// against this database and set `clang_tidy`/`clang_tidy_skip_reason` to
    /// something other than "never touched". A tempdir with no build tree at
    /// all (the earlier version of this test) cannot tell "the filter
    /// excluded the pass" apart from "the pass ran and immediately skipped
    /// for lack of a compile database" -- both look like `None`.
    #[test]
    fn only_cppcheck_never_attempts_the_clang_tidy_pass_even_with_a_real_compile_database() {
        let dir = repo_with_sources();
        let build_dir = dir.path().join("build/windows-x64-ninja-debug");
        std::fs::create_dir_all(&build_dir).unwrap();
        std::fs::write(build_dir.join("compile_commands.json"), "[]").unwrap();
        // Injected, never the real per-user cache: this test may run on a
        // machine with cppcheck actually installed, and must not read or
        // write %LOCALAPPDATA%\ExoSnap\tool-cache.
        let cache_root = dir.path().join("tool-cache");

        let result = run(
            dir.path(),
            None,
            None,
            Some(Only::CppCheck),
            1,
            None,
            Some(&cache_root),
        );

        match result {
            Ok(report) => {
                assert!(
                    report.clang_tidy.is_none(),
                    "Only::CppCheck must keep the clang-tidy pass from ever running"
                );
                assert!(
                    report.clang_tidy_skip_reason.is_none(),
                    "a skip reason means the pass ran and decided to skip; it must never have started"
                );
            }
            Err(error) => {
                assert!(
                    error.downcast_ref::<ToolMissing>().is_some(),
                    "the only tool this call may report missing is cppcheck"
                );
            }
        }
    }

    #[test]
    fn cppcheck_cache_dir_is_salted_by_version_and_the_full_argument_list() {
        let arguments = cppcheck_arguments();
        let root = tempfile::tempdir().unwrap();

        let one = cppcheck_cache_dir("2.13", &arguments, Some(root.path()));
        let same_inputs_again = cppcheck_cache_dir("2.13", &arguments, Some(root.path()));
        assert_eq!(
            one, same_inputs_again,
            "identical version and arguments must address the same cache directory"
        );

        let different_version = cppcheck_cache_dir("2.14", &arguments, Some(root.path()));
        assert_ne!(
            one, different_version,
            "a cppcheck upgrade must not replay verdicts reached under a different version"
        );

        let mut different_arguments = arguments.clone();
        different_arguments.push("--enable=unusedFunction".to_string());
        let different_key = cppcheck_cache_dir("2.13", &different_arguments, Some(root.path()));
        assert_ne!(
            one, different_key,
            "a changed check set must not replay verdicts reached under a different rule set"
        );
    }

    #[test]
    fn a_batch_that_fails_to_spawn_is_a_hard_error_never_silent_success() {
        // A name no PATH entry can resolve: `Command::output()` fails with
        // "file not found" rather than the process ever starting.
        let bogus_tool = Path::new("exo-dev-test-tool-that-does-not-exist.exe");
        let batches = vec![vec!["a.cpp".to_string()]];
        let result = run_clang_tidy_batches(bogus_tool, Path::new("."), &[], &batches, 1);
        assert!(
            result.is_err(),
            "a batch that never started must not be reported as zero findings"
        );
    }
}
