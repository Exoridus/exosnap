use std::path::Path;

use anyhow::{Context, Result, bail, ensure};

use crate::host_lock::{self, LockKind};
use crate::process::{OutputMode, StepExit, StepRunner};

/// Builds Qt's statistics target and retains compiler details while printing its summary.
pub fn report(
    root: &Path,
    build_dir: &Path,
    config: &str,
    jobs: usize,
    report_path: Option<&Path>,
) -> Result<()> {
    ensure!(jobs > 0, "--jobs must be greater than zero");
    let build_dir = root.join(build_dir);
    let cache = std::fs::read_to_string(build_dir.join("CMakeCache.txt"))
        .context("AOT statistics require an existing configured Qt build tree")?;
    let tree_lock = host_lock::acquire(LockKind::Tree, Some(&build_dir), "qml-aot-stats")?;
    let build_lock = host_lock::acquire(LockKind::Build, Some(&build_dir), "qml-aot-stats")?;
    let env = if cfg!(windows)
        && cache.contains("CMAKE_GENERATOR:INTERNAL=Ninja")
        && cache.contains("cl.exe")
    {
        crate::msvc::environment()?
            .map(|(_, variables)| {
                variables
                    .into_iter()
                    .map(|(key, value)| (key.into(), value.into()))
                    .collect()
            })
            .unwrap_or_default()
    } else {
        Vec::new()
    };
    let runner = StepRunner {
        log_dir: root
            .join(".workspace/qml-aot")
            .join(host_lock::tree_key(&build_dir)),
        output: OutputMode::Compact,
        failure_tail_lines: 120,
        env,
    };
    let args = [
        "--build".into(),
        build_dir.to_string_lossy().into_owned(),
        "--config".into(),
        config.into(),
        "--target".into(),
        "all_aotstats".into(),
        "--parallel".into(),
        jobs.to_string(),
    ];
    let (exit, log) = runner.run(
        "aotstats",
        "cmake",
        &args,
        root,
        &[tree_lock.child_env(), build_lock.child_env()],
    )?;
    if exit != StepExit::Code(0) {
        runner.print_failure("aotstats", "FAIL", &log);
        bail!("Qt AOT statistics failed; full log: {}", log.display());
    }
    let generated = build_dir.join(".rcc/qmlcache/all_aotstats.txt");
    let text = std::fs::read_to_string(&generated).context("Qt did not produce an AOT report")?;
    let concise = summary(&text)?;
    let destination = report_path.map_or_else(|| generated.clone(), |path| root.join(path));
    if destination != generated {
        if let Some(parent) = destination.parent() {
            std::fs::create_dir_all(parent)?;
        }
        std::fs::copy(&generated, &destination)?;
    }
    println!("{concise}");
    println!("Detailed AOT report: {}", destination.display());
    Ok(())
}

fn summary(report: &str) -> Result<&str> {
    let Some((_, summary)) =
        report.split_once("############ AOT COMPILATION STATS SUMMARY ############")
    else {
        bail!("Qt AOT report has no compilation summary");
    };
    ensure!(
        !summary.trim().is_empty(),
        "Qt AOT compilation summary is empty"
    );
    Ok(summary.trim())
}

#[cfg(test)]
mod tests {
    use super::summary;

    #[test]
    fn concise_summary_keeps_fallback_modules_without_function_details() {
        let report = "File Main.qml\n  binding failed: dynamic property\n\
            ############ AOT COMPILATION STATS SUMMARY ############\n\
            Module Main: 2 of 3 bindings compiled\n\
            Modules only compiled to bytecode: Preview\n";
        assert_eq!(
            summary(report).unwrap(),
            "Module Main: 2 of 3 bindings compiled\nModules only compiled to bytecode: Preview"
        );
    }

    #[test]
    fn incomplete_report_cannot_be_reported_as_a_summary() {
        assert!(summary("compiler failed before producing statistics").is_err());
    }
}
