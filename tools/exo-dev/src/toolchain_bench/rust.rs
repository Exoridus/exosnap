//! Cargo with and without `RUSTC_WRAPPER=sccache`, in target directories and a
//! cache directory private to the experiment.

use std::path::Path;

use anyhow::{Result, bail};
use clap::Args;

use super::measure::timed;
use super::report::{RunReport, write_run_report};
use super::{
    CommonArgs, Llvm, Sample, Skipped, detect_tools, out_dir, read_sccache, reset_sccache,
    samples_summary, toolchain_env, touch,
};
use crate::process;

const CARGO_PLAIN: &str = "cargo-plain";
const CARGO_SCCACHE: &str = "cargo-sccache";
const SCCACHE_PORT: u16 = 43999;

#[derive(Args)]
pub struct RustArgs {
    #[command(flatten)]
    pub common: CommonArgs,
    /// Package of the tools workspace to build.
    #[arg(long, default_value = "exo-dev")]
    pub package: String,
    /// Source file, relative to the repository, touched by the touch scenario.
    #[arg(long, default_value = "tools/exo-dev/src/lib.rs")]
    pub touch: String,
    #[arg(long, default_value_t = 3)]
    pub reps: usize,
}

pub fn run(repo: &Path, args: RustArgs) -> Result<()> {
    let out = out_dir(repo, &args.common, "rust")?;
    let env = toolchain_env()?;
    let tools = detect_tools(Llvm::find(args.common.llvm_dir.as_deref()).as_ref(), &env);
    let have_sccache = tools
        .iter()
        .any(|t| t.name == "sccache" && t.path.is_some());
    let cache_dir = out.join("sccache-rust");
    let mut samples: Vec<Sample> = Vec::new();
    let mut skipped: Vec<Skipped> = Vec::new();
    if !have_sccache {
        skipped.push(Skipped {
            variant: CARGO_SCCACHE.into(),
            reason: "sccache not found".into(),
        });
    }

    let build = |mode: &str, target: &Path, log: &Path| -> Result<super::Timing> {
        let mut command = process::command("cargo");
        command
            .current_dir(repo.join("tools"))
            .args(["build", "-p", &args.package, "--target-dir"])
            .arg(target)
            // Incremental artifacts cannot be cached, so both modes build
            // non-incrementally to stay comparable.
            .env("CARGO_INCREMENTAL", "0");
        if mode == CARGO_SCCACHE {
            command
                .env("RUSTC_WRAPPER", "sccache")
                .env("SCCACHE_DIR", &cache_dir)
                .env("SCCACHE_SERVER_PORT", SCCACHE_PORT.to_string());
        }
        timed(command, log)
    };

    let modes: Vec<&str> = if have_sccache {
        vec![CARGO_PLAIN, CARGO_SCCACHE]
    } else {
        vec![CARGO_PLAIN]
    };
    for mode in modes {
        let target = out.join(format!("target-{mode}"));
        let scenarios: &[&str] = if mode == CARGO_SCCACHE {
            &["cold", "warm", "touch"]
        } else {
            &["cold", "touch"]
        };
        for &scenario in scenarios {
            for rep in 1..=args.reps {
                if scenario != "touch" {
                    let _ = std::fs::remove_dir_all(&target);
                }
                if mode == CARGO_SCCACHE && scenario != "touch" {
                    reset_sccache(&cache_dir, SCCACHE_PORT, scenario == "cold");
                }
                if scenario == "touch" {
                    // The first build leaves a populated target to touch.
                    if rep == 1 && !target.join("debug").is_dir() {
                        build(
                            mode,
                            &target,
                            &out.join("logs").join(format!("{mode}-prime.log")),
                        )?;
                    }
                    touch(&repo.join(&args.touch))?;
                }
                let log = out
                    .join("logs")
                    .join(format!("{mode}-{scenario}-{rep}.log"));
                let timing = build(mode, &target, &log)?;
                eprintln!(
                    "{mode} {scenario} #{rep}: {:.1}s exit {}",
                    timing.wall_seconds, timing.exit_code
                );
                let sccache = (mode == CARGO_SCCACHE)
                    .then(|| read_sccache(&cache_dir, SCCACHE_PORT))
                    .flatten();
                samples.push(Sample {
                    variant: mode.into(),
                    scenario: scenario.into(),
                    rep,
                    timing,
                    sccache,
                    log,
                });
            }
        }
    }
    if samples.iter().all(|s| s.timing.exit_code != 0) {
        bail!(
            "every cargo build failed; see {}",
            out.join("logs").display()
        );
    }
    let revision = crate::git::Git::new(repo).head().unwrap_or_default();
    let summary = samples_summary(&samples);
    write_run_report(
        &out,
        &RunReport {
            revision,
            target: args.package.clone(),
            jobs: std::thread::available_parallelism().map_or(0, usize::from),
            reps: args.reps,
            baseline: CARGO_PLAIN.into(),
            tools,
            summary,
            samples,
            skipped,
        },
    )?;
    println!("report: {}", out.join("report.md").display());
    Ok(())
}
