//! Uncached early feedback over direct changed translation units.

use super::*;
use anyhow::Context as _;

#[derive(Debug)]
pub struct QuickReport {
    pub analyzed: usize,
    pub jobs: usize,
    pub skip_reason: Option<String>,
    pub unconfigured: Vec<String>,
}

impl QuickReport {
    pub fn summary(&self) -> String {
        let verdict = match &self.skip_reason {
            Some(reason) => format!(
                "SKIP ({reason}); {} direct translation unit(s) checked",
                self.analyzed
            ),
            None => format!(
                "clean ({} direct translation unit(s), {} worker(s), canonical checks)",
                self.analyzed, self.jobs
            ),
        };
        format!("clang-tidy preflight: {verdict}; full blocking analysis remains required.\n")
    }
}

fn quick_checks() -> String {
    // Analyzer selection also affects frontend warning handling; preserve the full gate.
    format!("-*,{}", BLOCKING_CHECKS.join(","))
}

fn changed_sources(repo_root: &Path, build_dir: &Path, changed: &[String]) -> Vec<String> {
    let root = normalize_root(repo_root);
    let build = normalize_root(build_dir);
    let physical_build = normalize_directory(build_dir);
    changed
        .iter()
        .map(|path| repo_root.join(path))
        .filter(|path| path.is_file())
        .map(|path| normalize_root(&path))
        .filter(|path| {
            is_project_source(path, &root, &build)
                && is_project_source(path, &root, &physical_build)
        })
        .collect::<BTreeSet<_>>()
        .into_iter()
        .collect()
}

fn covered_sources(changed: &[String], available: &[String]) -> (Vec<String>, Vec<String>) {
    let mut selected = BTreeSet::new();
    let mut missing = Vec::new();
    for source in changed {
        if let Some(configured) = available
            .iter()
            .find(|entry| entry.eq_ignore_ascii_case(source))
        {
            selected.insert(configured.clone());
        } else {
            missing.push(source.clone());
        }
    }
    (selected.into_iter().collect(), missing)
}

fn require_quick_verdict(
    source: &str,
    output: &str,
    code: i32,
    root: &str,
    build: &str,
) -> anyhow::Result<()> {
    require_analysis(source, output, code)?;
    let violations = extract_violations(&[(source.to_string(), output.to_string())], root, build);
    anyhow::ensure!(
        violations.is_empty(),
        "clang-tidy preflight blocking violations:\n  {}",
        violations.into_iter().collect::<Vec<_>>().join("\n  ")
    );
    Ok(())
}

/// Checks only direct changed sources using an existing build's compiler commands.
/// Missing configuration is explicitly incomplete; this never replaces the full gate.
#[allow(clippy::too_many_arguments)]
pub fn run_quick(
    repo_root: &Path,
    build_dir: &Path,
    changed: &[String],
    jobs: usize,
    require_version: Option<&str>,
    clang_tidy_path: Option<&Path>,
    environment: &[(std::ffi::OsString, std::ffi::OsString)],
) -> anyhow::Result<QuickReport> {
    let mut report = QuickReport {
        analyzed: 0,
        jobs: 0,
        skip_reason: None,
        unconfigured: Vec::new(),
    };
    let changed = changed_sources(repo_root, build_dir, changed);
    if changed.is_empty() {
        report.skip_reason =
            Some("no direct C/C++ source changes; headers require the full gate".into());
        return Ok(report);
    }
    let database = build_dir.join("compile_commands.json");
    let text = match std::fs::read_to_string(&database) {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            report.skip_reason = Some(format!(
                "no existing compilation database at {}; configure and build the Ninja preset for local preflight coverage",
                database.display()
            ));
            return Ok(report);
        }
        Err(error) => {
            return Err(error).with_context(|| format!("could not read {}", database.display()));
        }
    };
    let entries = parse_compile_db(&text)?;
    let root = normalize_root(repo_root);
    let build = normalize_root(build_dir);
    let available = project_sources(&entries, &root, &build);
    let (selected, missing) = covered_sources(&changed, &available);
    if !missing.is_empty() {
        report.skip_reason = Some(format!(
            "source(s) absent from the configured compilation database: {}; configure and build a preset including the affected targets, or rely on the required CI configuration for intentionally excluded sources",
            missing.join(", ")
        ));
        report.unconfigured = missing;
    }
    if selected.is_empty() {
        return Ok(report);
    }
    let tool = discover_tool(&entries, clang_tidy_path)?;
    if let Some(expected) = require_version {
        let banner = clang_tidy_version_banner(&tool)?;
        let version = parse_version(&banner).unwrap_or_else(|| "unknown".into());
        let expected = expected.trim();
        anyhow::ensure!(
            version == expected || version.starts_with(&format!("{expected}.")),
            "clang-tidy {version} does not satisfy the required version '{expected}' ({})",
            tool.display()
        );
    }
    let jobs = if jobs == 0 { 2 } else { jobs };
    let flags = invocation_flags(&quick_checks(), HEADER_FILTER);
    let observed = dependency_manifest::Observed::new(false);
    analyze_continuously(
        &selected,
        jobs,
        1,
        |_, source| {
            let (output, code, timing) =
                invoke_clang_tidy(&tool, build_dir, &flags, source, environment);
            observed.strip((source.to_string(), output, code, timing))
        },
        |batch| {
            for (source, output, code, _) in batch {
                require_quick_verdict(source, output, *code, &root, &build)?;
            }
            Ok(())
        },
    )?;
    report.analyzed = selected.len();
    report.jobs = jobs.min(selected.len());
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quick_checks_preserve_the_canonical_frontend_and_checks() {
        let checks = quick_checks();
        assert_eq!(checks, format!("-*,{}", BLOCKING_CHECKS.join(",")));
        assert!(checks.contains("clang-analyzer"));
        assert!(checks.contains("misc-include-cleaner"));
    }

    #[test]
    fn quick_scope_ignores_new_headers_and_retains_only_direct_owned_sources() {
        let fixture = tempfile::tempdir().unwrap();
        let root = fixture.path();
        let build = root.join("build");
        for file in [
            "libs/a.cpp",
            "libs/new.h",
            "libs/unmodified.cpp",
            "build/generated.cpp",
            "third_party/vendor.cpp",
        ] {
            let path = root.join(file);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, "").unwrap();
        }
        let changed = [
            "libs/a.cpp",
            "libs/new.h",
            "build/generated.cpp",
            "third_party/vendor.cpp",
            "libs/deleted.cpp",
        ]
        .map(str::to_string);
        assert_eq!(
            changed_sources(root, &build, &changed),
            vec![normalize_root(&root.join("libs/a.cpp"))]
        );
        let (selected, missing) = covered_sources(
            &["C:/Repo/LIBS/a.cpp".into(), "C:/repo/libs/new.cpp".into()],
            &["C:/repo/libs/a.cpp".into()],
        );
        assert_eq!(selected, ["C:/repo/libs/a.cpp"]);
        assert_eq!(missing, ["C:/repo/libs/new.cpp"]);
    }

    #[test]
    fn headers_and_missing_database_skip_before_tool_or_cache_discovery() {
        let fixture = tempfile::tempdir().unwrap();
        let root = fixture.path();
        std::fs::write(root.join("a.h"), "").unwrap();
        std::fs::write(root.join("a.cpp"), "").unwrap();
        let build = root.join("build");
        let missing_tool = root.join("no-clang-tidy");
        for changed in [vec!["a.h".into()], vec!["a.cpp".into()]] {
            let report =
                run_quick(root, &build, &changed, 0, None, Some(&missing_tool), &[]).unwrap();
            assert_eq!(report.analyzed, 0);
            assert!(report.skip_reason.is_some());
            assert!(report.summary().contains("SKIP"));
            assert!(
                report
                    .summary()
                    .contains("full blocking analysis remains required")
            );
            assert!(!build.exists());
        }
    }

    #[test]
    fn absent_compile_commands_are_incomplete_instead_of_clean() {
        let fixture = tempfile::tempdir().unwrap();
        let root = fixture.path();
        std::fs::write(root.join("new.cpp"), "").unwrap();
        let build = root.join("build");
        std::fs::create_dir(&build).unwrap();
        std::fs::write(build.join("compile_commands.json"), "[]").unwrap();
        let report = run_quick(root, &build, &["new.cpp".into()], 0, None, None, &[]).unwrap();
        assert_eq!(report.analyzed, 0);
        assert_eq!(report.unconfigured, [normalize_root(&root.join("new.cpp"))]);
        assert!(report.summary().contains("SKIP"));
        assert!(!report.summary().contains("clean"));
    }

    #[test]
    fn quick_verdict_keeps_the_canonical_include_cleaner_scope_and_parse_failures() {
        let output =
            "C:/repo/libs/engine/a.cpp:1:2: warning: missing provider [misc-include-cleaner]";
        let error =
            require_quick_verdict("a.cpp", output, 0, "C:/repo", "C:/repo/build").unwrap_err();
        assert!(
            error
                .to_string()
                .contains("libs/engine/a.cpp:1:2 [misc-include-cleaner] missing provider")
        );
        require_quick_verdict(
            "a.cpp",
            &output.replace("libs/engine/", "app/"),
            0,
            "C:/repo",
            "C:/repo/build",
        )
        .unwrap();
        assert!(
            require_quick_verdict(
                "a.cpp",
                "error: cannot parse",
                1,
                "C:/repo",
                "C:/repo/build"
            )
            .is_err()
        );
    }

    #[test]
    fn first_blocking_quick_result_stops_queued_work_and_drains_the_running_unit() {
        let pending = (0..20)
            .map(|index| format!("{index}.cpp"))
            .collect::<Vec<_>>();
        let started = AtomicUsize::new(0);
        let finished = AtomicUsize::new(0);
        let failure_seen = std::sync::atomic::AtomicBool::new(false);
        let result = analyze_continuously(
            &pending,
            1,
            1,
            |index, source| {
                started.fetch_add(1, Ordering::SeqCst);
                if index > 0 {
                    let deadline = Instant::now() + std::time::Duration::from_secs(10);
                    while !failure_seen.load(Ordering::SeqCst) {
                        assert!(Instant::now() < deadline);
                        std::thread::yield_now();
                    }
                    std::thread::sleep(std::time::Duration::from_millis(50));
                }
                finished.fetch_add(1, Ordering::SeqCst);
                (source.to_string(), "C:/repo/libs/engine/a.cpp:2:3: warning: missing provider [misc-include-cleaner]".into(), 0,
                UnitTiming { unit: None, source: source.to_string(), wall_seconds: 0.0, cpu_seconds: None, peak_working_set_bytes: None })
            },
            |batch| {
                let (source, output, code, _) = &batch[0];
                let verdict =
                    require_quick_verdict(source, output, *code, "C:/repo", "C:/repo/build");
                failure_seen.store(true, Ordering::SeqCst);
                verdict
            },
        );
        assert!(result.unwrap_err().to_string().contains("missing provider"));
        assert!(started.load(Ordering::SeqCst) <= 2);
        assert_eq!(
            started.load(Ordering::SeqCst),
            finished.load(Ordering::SeqCst)
        );
    }
}
