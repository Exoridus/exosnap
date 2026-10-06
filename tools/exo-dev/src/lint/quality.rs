//! Advisory clang-tidy (the broad `.clang-tidy` check set, minus
//! `clang-analyzer-*`) and cppcheck over the project's own sources,
//! with normalized repository-owned diagnostic reports.
//!
//! Distinct from [`crate::lint::clang_tidy`]: that module runs the curated
//! [`crate::lint::canaries::BLOCKING_CHECKS`] set and fails a pull request on
//! a finding. This module's clang-tidy pass is advisory: its findings are
//! reported, never a gate, because the broad check set currently reports
//! findings across the tree. cppcheck is the opposite: it carries
//! `--error-exitcode=1`, so a finding here does fail the calling step.
//!
//! `base` restricts only the clang-tidy pass. Source changes intersect the
//! compilation database. Header changes trigger all translation units so their
//! consumers remain covered. cppcheck always analyses the `libs/` and `app/` trees,
//! regardless of `base`: it is a whole-program-shaped scan cppcheck's own
//! result cache already keeps cheap on an unchanged tree.

use std::collections::HashSet;
use std::io::Write as _;
use std::path::{Path, PathBuf};

use anyhow::Context as _;

use crate::executor::ToolMissing;
use crate::git::Git;
use crate::lint;
use crate::lint::advisory::{self, NormalizedReport, RepositoryPaths, RunMetadata};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Only {
    CppCheck,
    ClangTidy,
    CppCheckUnusedFunction,
}

#[derive(Debug, Default)]
pub struct ClangTidyAdvisory {
    pub scope: String,
    pub analyzed: usize,
    pub batches: usize,
    pub jobs: usize,
    /// Complete output, including warning-only batches that exited successfully.
    pub findings: Vec<String>,
    pub normalized: Box<NormalizedReport>,
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
/// per-user cache under `%LOCALAPPDATA%`). A test passes an injected directory
/// here instead of reading and writing the real one.
pub fn run(
    repo_root: &Path,
    build_dir: Option<&Path>,
    base: Option<&str>,
    only: Option<Only>,
    jobs: usize,
    report_path: Option<&Path>,
    cache_root: Option<&Path>,
) -> anyhow::Result<QualityReport> {
    run_with_tool(
        repo_root,
        build_dir,
        base,
        only,
        jobs,
        report_path,
        cache_root,
        None,
    )
}

/// Explicit tool selection supports compilation databases from newer toolchains.
#[allow(clippy::too_many_arguments)]
pub fn run_with_tool(
    repo_root: &Path,
    build_dir: Option<&Path>,
    base: Option<&str>,
    only: Option<Only>,
    jobs: usize,
    report_path: Option<&Path>,
    cache_root: Option<&Path>,
    clang_tidy: Option<&Path>,
) -> anyhow::Result<QualityReport> {
    let default_path = repo_root.join(if only == Some(Only::CppCheckUnusedFunction) {
        ".workspace/advisory/cppcheck-unused-function.txt"
    } else {
        ".workspace/advisory/clang-tidy.txt"
    });
    let artifact_path =
        (only != Some(Only::CppCheck)).then(|| report_path.unwrap_or(&default_path));
    if let Some(path) = artifact_path {
        advisory::write_status(path, "running", "Measurement has not completed.")?;
        advisory::write_raw(path, "")?;
    }
    let result = run_inner(
        repo_root,
        build_dir,
        base,
        only,
        jobs,
        artifact_path,
        cache_root,
        clang_tidy,
    );
    if let Some(path) = artifact_path {
        match &result {
            Err(error) => advisory::write_status(path, "failed", &format!("{error:#}"))?,
            Ok(report) => {
                if let Some(reason) = &report.clang_tidy_skip_reason {
                    advisory::write_status(path, "out_of_scope", reason)?;
                }
            }
        }
    }
    result
}

#[allow(clippy::too_many_arguments)]
fn run_inner(
    repo_root: &Path,
    build_dir: Option<&Path>,
    base: Option<&str>,
    only: Option<Only>,
    jobs: usize,
    report_path: Option<&Path>,
    cache_root: Option<&Path>,
    clang_tidy: Option<&Path>,
) -> anyhow::Result<QualityReport> {
    let mut missing: Vec<&'static str> = Vec::new();
    let mut clang_tidy_skip_reason = None;

    let clang_tidy = if !matches!(only, Some(Only::CppCheck | Only::CppCheckUnusedFunction)) {
        match clang_tidy_pass(repo_root, build_dir, base, jobs, report_path, clang_tidy)? {
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
        match cppcheck_pass_mode(
            repo_root,
            cache_root,
            only == Some(Only::CppCheckUnusedFunction),
            report_path,
        )? {
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
    clang_tidy: Option<&Path>,
) -> anyhow::Result<ClangTidyOutcome> {
    let started = std::time::Instant::now();
    let started_unix_seconds = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_secs();
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
        anyhow::bail!("no compile_commands.json; run: cmake --preset windows-x64-ninja-debug");
    };

    let Some(tool) = clang_tidy
        .map(Path::to_path_buf)
        .or_else(|| lint::find_tool(&["clang-tidy"]))
    else {
        return Ok(ClangTidyOutcome::ToolMissing);
    };
    if !tool.is_file() {
        return Ok(ClangTidyOutcome::ToolMissing);
    }

    let tracked = project_sources(repo_root)?;
    let paths = RepositoryPaths::new(repo_root, &tracked);
    let units = translation_units(repo_root, &tree, &paths)?;
    let mut sources: Vec<_> = units.keys().cloned().collect();
    let changed_files = base
        .map(|base| touched_files(repo_root, base))
        .transpose()?;
    let scope_desc = match base {
        Some(base) => {
            // Header changes can affect every consumer. A full pass keeps the
            // advisory scope sound without duplicating dependency expansion.
            let touched = changed_files.as_ref().unwrap();
            if !touched.iter().any(|file| file.ends_with(".h")) {
                sources.retain(|s| touched.contains(s));
            }
            format!("changed since {base}")
        }
        None => "every tracked source".to_string(),
    };

    if sources.is_empty() {
        anyhow::ensure!(
            base.is_some(),
            "compilation database contains no tracked C++ translation units"
        );
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
        "--warnings-as-errors=-*".to_string(),
    ];
    let mut fixed_arguments = fixed_arguments;
    // Compiler warnings remain measured findings in this advisory pass. Syntax
    // errors still fail. The production compile command retains /WX or -Werror.
    fixed_arguments.push(if cfg!(windows) {
        "--extra-arg=/clang:-Wno-error".into()
    } else {
        "--extra-arg=-Wno-error".into()
    });
    fixed_arguments[2].push_str(
        ",readability-function-cognitive-complexity,bugprone-easily-swappable-parameters",
    );
    fixed_arguments.push("--config-file=.clang-tidy".into());
    let batches = lint::batch_command_line(&sources, &fixed_arguments, 1, 30000);

    let jobs = if jobs == 0 {
        std::thread::available_parallelism()
            .map(std::num::NonZeroUsize::get)
            .unwrap_or(4)
    } else {
        jobs
    };

    let version = cppcheck_version(&tool)?;
    let git = Git::new(repo_root);
    let (commit_status, commit) = git.run(&["rev-parse", "HEAD"]);
    anyhow::ensure!(commit_status == 0, "cannot identify advisory source commit");
    let (dirty_status, dirty) = git.run(&["status", "--porcelain", "--untracked-files=no"]);
    anyhow::ensure!(
        dirty_status == 0,
        "cannot identify advisory working tree state"
    );
    let default_path = repo_root.join(".workspace/advisory/clang-tidy.txt");
    let path = report_path.unwrap_or(&default_path);
    advisory::write_raw(path, "")?;
    let findings = run_clang_tidy_batches(
        &tool,
        repo_root,
        &fixed_arguments,
        &batches,
        jobs,
        Some(path),
    )?;
    let raw = findings.join("\n");
    // Keep evidence even when parsing finds an infrastructure/compiler failure.
    advisory::write_raw(path, &raw)?;
    let changed_files = changed_files.map(|files| files.into_iter().collect());
    let normalized = advisory::normalize(
        &raw,
        &paths,
        RunMetadata {
            commit: commit.trim().into(),
            working_tree_dirty: !dirty.trim().is_empty(),
            clang_tidy_version: version,
            input_count: sources.len(),
            translation_unit_count: sources.iter().map(|file| units[file]).sum(),
            started_unix_seconds,
            elapsed_seconds: started.elapsed().as_secs_f64(),
            arguments: fixed_arguments,
        },
        changed_files.as_ref(),
    )?;
    normalized.write(path, &raw)?;

    Ok(ClangTidyOutcome::Ran(ClangTidyAdvisory {
        scope: scope_desc,
        analyzed: sources.len(),
        batches: batches.len(),
        jobs,
        findings,
        normalized: Box::new(normalized),
    }))
}

fn translation_units(
    repo_root: &Path,
    tree: &Path,
    paths: &RepositoryPaths,
) -> anyhow::Result<std::collections::BTreeMap<String, usize>> {
    #[derive(serde::Deserialize)]
    struct Entry {
        file: String,
        directory: PathBuf,
    }
    let entries: Vec<Entry> = serde_json::from_slice(&std::fs::read(
        repo_root.join(tree).join("compile_commands.json"),
    )?)
    .context("invalid compilation database")?;
    let mut sources = std::collections::BTreeMap::new();
    for entry in entries {
        let path = Path::new(&entry.file);
        let absolute = if path.is_absolute() {
            path.to_path_buf()
        } else {
            entry.directory.join(path)
        };
        if let Some(file) = paths.relative(&absolute.to_string_lossy())
            && file.ends_with(".cpp")
            && !file.split('/').any(|part| part == "third_party")
        {
            *sources.entry(file).or_default() += 1;
        }
    }
    Ok(sources)
}

/// Tracked C++ sources and headers from libraries, applications, native tools
/// and tests. Generated compilation inputs are excluded by this inventory.
fn project_sources(repo_root: &Path) -> anyhow::Result<Vec<String>> {
    let git = Git::new(repo_root);
    let (code, stdout) = git.run(&[
        "ls-files", "--", "libs/", "app/", "apps/", "tools/", "tests/",
    ]);
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

/// Every path `git diff` names against `base`, the working tree and the index.
/// Failed Git queries cannot establish an empty analysis scope.
fn touched_files(repo_root: &Path, base: &str) -> anyhow::Result<HashSet<String>> {
    let git = Git::new(repo_root);
    let range = format!("{base}...HEAD");
    let mut touched = HashSet::new();
    for args in [
        vec!["diff", "--name-only", range.as_str()],
        vec!["diff", "--name-only"],
        vec!["diff", "--cached", "--name-only"],
    ] {
        let (code, stdout) = git.run(&args);
        anyhow::ensure!(
            code == 0,
            "git {} failed with exit code {code} while resolving advisory scope",
            args.join(" ")
        );
        for line in stdout.lines() {
            let line = line.trim();
            if !line.is_empty() {
                touched.insert(line.replace('\\', "/"));
            }
        }
    }
    Ok(touched)
}

/// Runs clang-tidy over `batches` with up to `jobs` processes in flight at
/// once, returning stdout/stderr regardless of exit status. Findings do not
/// fail this advisory pass, but a nonzero exit is an infrastructure failure:
/// warnings-as-errors are explicitly disabled for advisory measurements.
///
/// A batch that never starts (the tool vanished between discovery and this
/// call, an invalid working directory, ...) is a hard error, not silence:
/// treating a spawn failure as "no findings" would report a clean pass over
/// an analysis that never ran. A gate that never started establishes nothing,
/// and must never be indistinguishable from one that ran and found nothing.
fn run_clang_tidy_batches(
    tool: &Path,
    repo_root: &Path,
    fixed_arguments: &[String],
    batches: &[Vec<String>],
    jobs: usize,
    raw_path: Option<&Path>,
) -> anyhow::Result<Vec<String>> {
    if batches.is_empty() {
        return Ok(Vec::new());
    }
    let jobs = jobs.max(1).min(batches.len());
    let queue: std::sync::Mutex<std::collections::VecDeque<&Vec<String>>> =
        std::sync::Mutex::new(batches.iter().collect());
    let findings: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
    let spawn_error: std::sync::Mutex<Option<String>> = std::sync::Mutex::new(None);
    let raw_file = std::sync::Mutex::new(
        raw_path
            .map(|path| std::fs::OpenOptions::new().append(true).open(path))
            .transpose()?,
    );
    std::thread::scope(|scope| {
        for _ in 0..jobs {
            scope.spawn(|| {
                loop {
                    if spawn_error.lock().unwrap().is_some() {
                        // Another batch already failed to launch. Stop
                        // pulling more work rather than racing more spawn
                        // attempts.
                        break;
                    }
                    let next = queue.lock().unwrap().pop_front();
                    let Some(batch) = next else { break };
                    let mut command = crate::process::command(&tool.to_string_lossy());
                    command.current_dir(repo_root);
                    command.args(fixed_arguments);
                    command.args(batch.as_slice());
                    match command.output() {
                        Ok(output) => {
                            let text =
                                batch_output(&output.stdout, &output.stderr, output.status.code());
                            if !output.status.success() {
                                *spawn_error.lock().unwrap() = Some(format!(
                                    "clang-tidy failed on {} ({}); inspect the raw report",
                                    batch.join(", "),
                                    output.status
                                ));
                            }
                            if let Some(file) = raw_file.lock().unwrap().as_mut() {
                                if let Err(error) = writeln!(file, "{text}") {
                                    *spawn_error.lock().unwrap() =
                                        Some(format!("could not retain raw diagnostics: {error}"));
                                }
                            }
                            let mut collected = findings.lock().unwrap();
                            collected.push(text);
                            if collected.len().is_multiple_of(10)
                                || collected.len() == batches.len()
                            {
                                eprintln!(
                                    "clang-tidy: {}/{} source files completed",
                                    collected.len(),
                                    batches.len()
                                );
                            }
                        }
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
    let mut findings = findings.into_inner().unwrap();
    findings.sort();
    Ok(findings)
}

fn batch_output(stdout: &[u8], stderr: &[u8], exit_code: Option<i32>) -> String {
    let mut combined = String::from_utf8_lossy(stdout).into_owned();
    combined.push('\n');
    combined.push_str(&String::from_utf8_lossy(stderr));
    if exit_code != Some(0) {
        combined.push_str(&format!(
            "\nerror: clang-tidy batch exited with {exit_code:?}\n"
        ));
    }
    combined
}

fn cppcheck_pass_mode(
    repo_root: &Path,
    cache_root: Option<&Path>,
    unused_function: bool,
    report_path: Option<&Path>,
) -> anyhow::Result<Option<CppCheckOutcome>> {
    let Some(tool) = discover_cppcheck() else {
        return Ok(None);
    };
    let mut arguments = cppcheck_arguments();
    if unused_function {
        arguments[0] = "--enable=unusedFunction".into();
        arguments.retain(|arg| arg != "--error-exitcode=1");
        arguments.push("--template={file}:{line}:{column}: {severity}: {message} [{id}]".into());
    }

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

    if unused_function {
        let default_path = repo_root.join(".workspace/advisory/cppcheck-unused-function.txt");
        let path = report_path.unwrap_or(&default_path);
        advisory::write_raw(path, &combined)?;
        anyhow::ensure!(
            output.status.success(),
            "cppcheck advisory tool failed: {} (raw report: {})",
            output.status,
            path.display()
        );
        let count = unused_function_count(&combined)?;
        let summary = serde_json::json!({"schema_version": 1, "tool": "cppcheck", "version": version, "check": "unusedFunction", "raw_diagnostic_count": count, "status": "completed"});
        std::fs::write(
            path.with_extension("json"),
            serde_json::to_vec_pretty(&summary)?,
        )?;
        std::fs::write(
            path.with_extension("md"),
            format!("# cppcheck unusedFunction advisory\n\nTool completed. Findings: {count}.\n"),
        )?;
    }

    Ok(Some(CppCheckOutcome {
        cache_dir,
        ok: output.status.success(),
        output: combined,
    }))
}

fn unused_function_count(output: &str) -> anyhow::Result<usize> {
    let diagnostic = regex::Regex::new(
        r"^.+:\d+:\d+: (style|warning|performance|portability|information): .+ \[([a-zA-Z0-9]+)\]$",
    )?;
    let mut count = 0;
    for line in output.lines() {
        anyhow::ensure!(
            !line.contains(": error:") && !line.contains("[internalError]"),
            "cppcheck could not analyze the source: {line}"
        );
        if line.ends_with("[unusedFunction]") {
            anyhow::ensure!(
                diagnostic.is_match(line),
                "malformed cppcheck finding: {line}"
            );
            count += 1;
        }
    }
    Ok(count)
}

/// cppcheck's own result cache location for one toolchain, salted by both its
/// version and its full argument list so a cppcheck upgrade or a changed
/// check set analyses afresh instead of replaying verdicts reached under
/// different rules. `root` overrides the real per-user cache root. A test
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
pub(crate) fn discover_cppcheck() -> Option<PathBuf> {
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
    let (status, stdout) = crate::process::query(command)
        .with_context(|| format!("could not run {}", tool.display()))?;
    anyhow::ensure!(
        status == 0 && !stdout.trim().is_empty(),
        "{} --version failed",
        tool.display()
    );
    Ok(stdout.trim().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::{fixture_repo_committed, write_files};

    #[test]
    fn cppcheck_analysis_errors_are_not_successful_zero_measurements() {
        assert_eq!(unused_function_count("").unwrap(), 0);
        assert_eq!(
            unused_function_count("C:\\repo\\a.cpp:1:0: style: unused function [unusedFunction]\n")
                .unwrap(),
            1
        );
        for output in [
            "a.cpp:1:2: error: syntax failed [syntaxError]",
            "cppcheck: error: invalid argument",
            "malformed [unusedFunction]",
        ] {
            assert!(unused_function_count(output).is_err(), "{output}");
        }
    }

    #[test]
    fn a_missing_tool_finishes_its_artifact_as_failed() {
        let dir = tempfile::tempdir().unwrap();
        let tree = dir.path().join("build");
        std::fs::create_dir(&tree).unwrap();
        std::fs::write(tree.join("compile_commands.json"), "[]").unwrap();
        let raw = dir.path().join("measurement.txt");
        let error = run_with_tool(
            dir.path(),
            Some(&tree),
            None,
            Some(Only::ClangTidy),
            1,
            Some(&raw),
            None,
            Some(&dir.path().join("missing.exe")),
        )
        .unwrap_err();
        assert!(error.downcast_ref::<ToolMissing>().is_some());
        let artifact: serde_json::Value =
            serde_json::from_slice(&std::fs::read(raw.with_extension("json")).unwrap()).unwrap();
        assert_eq!(artifact["status"], "failed");
    }

    fn repo_with_sources() -> tempfile::TempDir {
        fixture_repo_committed(&[
            ("libs/engine/src/a.cpp", "int a();\n"),
            ("libs/engine/src/a.h", "int a();\n"),
            ("app/quick/main.cpp", "int main() { return 0; }\n"),
            ("apps/updater/main.cpp", "int main() { return 0; }\n"),
            ("tools/probes/probe.cpp", "int probe();\n"),
            ("libs/update/third_party/vendor.cpp", "int v();\n"),
            ("docs/README.md", "not analysed\n"),
        ])
    }

    #[test]
    fn project_sources_includes_updater_and_native_tools() {
        let dir = repo_with_sources();
        let sources = project_sources(dir.path()).unwrap();
        assert_eq!(
            sources,
            vec![
                "app/quick/main.cpp".to_string(),
                "apps/updater/main.cpp".to_string(),
                "libs/engine/src/a.cpp".to_string(),
                "libs/engine/src/a.h".to_string(),
                "libs/update/third_party/vendor.cpp".to_string(),
                "tools/probes/probe.cpp".to_string(),
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
        let touched = touched_files(dir.path(), "HEAD").unwrap();
        assert!(touched.contains("libs/engine/src/a.h"));
        assert!(!touched.contains("libs/engine/src/a.cpp"));
    }

    #[test]
    fn an_invalid_advisory_base_is_not_a_successful_empty_scope() {
        let dir = repo_with_sources();
        let error = touched_files(dir.path(), "definitely-not-a-revision").unwrap_err();
        assert!(error.to_string().contains("git diff"));
    }

    #[test]
    fn advisory_scope_requires_a_git_repository() {
        let dir = tempfile::tempdir().unwrap();
        assert!(touched_files(dir.path(), "HEAD").is_err());
    }

    #[test]
    fn a_missing_compile_database_is_an_infrastructure_failure() {
        let dir = repo_with_sources();
        let error = clang_tidy_pass(dir.path(), None, None, 1, None, None).unwrap_err();
        assert!(error.to_string().contains("no compile_commands.json"));
    }

    #[test]
    fn an_explicit_build_dir_without_a_compile_database_is_a_hard_error() {
        let dir = repo_with_sources();
        let error = clang_tidy_pass(
            dir.path(),
            Some(Path::new("build/missing")),
            None,
            1,
            None,
            None,
        )
        .unwrap_err();
        assert!(error.to_string().contains("compile_commands.json"));
    }

    /// Non-vacuous by construction on two independent axes. A real (if
    /// empty) `compile_commands.json` and real project sources are present,
    /// so if the `only != Some(Only::CppCheck)` guard around the clang-tidy
    /// pass were ever removed, `clang_tidy_pass` would actually attempt to
    /// run clang-tidy against this database and set
    /// `clang_tidy`/`clang_tidy_skip_reason` to something other than "never
    /// touched". And the fixture is deliberately not a git repository:
    /// `project_sources` (which only the clang-tidy pass calls) then fails
    /// with a plain "git ls-files failed" error rather than `ToolMissing`, so
    /// a machine where clang-tidy also happens to be absent cannot make the
    /// buggy path (both tools missing) and the correct path (cppcheck alone
    /// missing) report the same `ToolMissing` message.
    #[test]
    fn only_cppcheck_never_attempts_the_clang_tidy_pass_even_with_a_real_compile_database() {
        let dir = tempfile::tempdir().unwrap();
        write_files(
            dir.path(),
            &[
                ("libs/engine/src/a.cpp", "int a();\n"),
                ("app/quick/main.cpp", "int main() { return 0; }\n"),
            ],
        );
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
                let missing = error.downcast::<ToolMissing>().expect(
                    "the clang-tidy pass must never run here, so the only failure this call \
                     may report is cppcheck missing, never a git error from a pass excluded by \
                     the filter",
                );
                assert_eq!(missing.0, "cppcheck not installed");
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
        let result = run_clang_tidy_batches(bogus_tool, Path::new("."), &[], &batches, 1, None);
        assert!(
            result.is_err(),
            "a batch that never started must not be reported as zero findings"
        );
    }

    #[test]
    fn warning_only_success_is_retained_and_nonzero_exit_is_not_a_measurement() {
        let warning = b"app/a.cpp:4:5: warning: unused [misc-unused-parameters]\n";
        let paths = RepositoryPaths::new(Path::new("C:/repo"), &["app/a.cpp".into()]);
        let raw = batch_output(b"", warning, Some(0));
        let report = advisory::normalize(&raw, &paths, RunMetadata::default(), None).unwrap();
        assert_eq!(report.unique_diagnostic_count, 1);
        for code in [Some(1), Some(3), None] {
            let raw = batch_output(b"", warning, code);
            assert!(advisory::normalize(&raw, &paths, RunMetadata::default(), None).is_err());
        }
    }

    #[test]
    fn compilation_database_selects_real_tracked_translation_units_only() {
        let dir = repo_with_sources();
        let tree = Path::new("build");
        std::fs::create_dir(dir.path().join(tree)).unwrap();
        let entries = [
            "libs/engine/src/a.cpp",
            "libs/engine/src/a.cpp",
            "libs/engine/src/a.h",
            "libs/update/third_party/vendor.cpp",
            "generated.cpp",
        ]
        .into_iter()
        .map(|file| serde_json::json!({"directory": dir.path(), "file": file}))
        .collect::<Vec<_>>();
        std::fs::write(
            dir.path().join(tree).join("compile_commands.json"),
            serde_json::to_vec(&entries).unwrap(),
        )
        .unwrap();
        let paths = RepositoryPaths::new(dir.path(), &project_sources(dir.path()).unwrap());
        assert_eq!(
            translation_units(dir.path(), tree, &paths).unwrap(),
            std::collections::BTreeMap::from([("libs/engine/src/a.cpp".into(), 2)])
        );
    }
}
