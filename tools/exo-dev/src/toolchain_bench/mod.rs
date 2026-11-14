//! Toolchain A/B measurement: compiler, linker and compiler-cache variants of
//! one CMake target, measured with identical settings.
//!
//! A variant id is `<compiler>[-lld|-link][-sccache]-<debug|release>` with compiler
//! `msvc` or `clangcl`. Each variant owns a build tree `build/toolbench-<id>`
//! that is configured once and reused. Every experiment is independent: an
//! unavailable tool skips its variants with a reason in the report instead of
//! failing the run.

mod measure;
mod probe;
mod report;
mod rust;

use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context, Result, bail};
use clap::{Args, Subcommand};
use serde::Serialize;

use crate::msvc::{self, EnvVars};
use crate::process;
use measure::{Timing, median, timed};

#[derive(Subcommand)]
pub enum ToolchainBenchCommand {
    /// Builds a target under each variant and scenario and reports wall time,
    /// peak memory and compiler-cache statistics.
    Run(RunArgs),
    /// Compiles chosen translation units once with compiler diagnostics that
    /// explain compile time (MSVC /Bt+ and /d1reportTime, clang -ftime-trace)
    /// or vectorization (clang loop-vectorize remarks).
    Probe(probe::ProbeArgs),
    /// Measures `RUSTC_WRAPPER=sccache` against a plain cargo build of the tools workspace.
    Rust(rust::RustArgs),
    /// Prints the detected tools and their versions as JSON.
    Tools(CommonArgs),
}

#[derive(Args, Clone)]
pub struct CommonArgs {
    /// Directory that receives logs and reports.
    #[arg(long)]
    pub out_dir: Option<PathBuf>,
    /// Root of an LLVM distribution that provides clang-cl and lld-link.
    /// Defaults to EXO_LLVM_DIR, then the Visual Studio and system installs.
    #[arg(long)]
    pub llvm_dir: Option<PathBuf>,
}

#[derive(Args)]
pub struct RunArgs {
    #[command(flatten)]
    pub common: CommonArgs,
    /// Variant ids, for example msvc-debug, clangcl-lld-debug, msvc-sccache-debug.
    #[arg(long = "variant", required = true)]
    pub variants: Vec<String>,
    /// cold, warm (compiler cache kept, outputs cleaned), noop, touch-cpp,
    /// touch-header, relink, fulllink.
    #[arg(long = "scenario", default_values_t = ["cold".to_string(), "touch-cpp".to_string()])]
    pub scenarios: Vec<String>,
    #[arg(long, default_value = "exosnap_engine")]
    pub target: String,
    /// Executable target whose link step the relink scenario measures.
    #[arg(long, default_value = "exosnap")]
    pub link_target: String,
    /// Source file, relative to the repository, touched by touch-cpp.
    #[arg(long, default_value = "libs/engine/src/version.cpp")]
    pub touch_cpp: PathBuf,
    /// Header, relative to the repository, touched by touch-header.
    #[arg(long, default_value = "libs/engine/include/exosnap/engine/version.h")]
    pub touch_header: PathBuf,
    #[arg(long, default_value_t = 3)]
    pub reps: usize,
    /// Parallel jobs passed to ninja. Defaults to the logical CPU count.
    #[arg(long)]
    pub jobs: Option<usize>,
    /// Extra `cmake` argument applied only to clangcl variants, for example
    /// `-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /DFMT_CONSTEVAL=`. Repeatable.
    #[arg(long = "clang-cmake-arg", allow_hyphen_values = true)]
    pub clang_cmake_args: Vec<String>,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Compiler {
    Msvc,
    ClangCl,
}

#[derive(Clone, Debug, Serialize)]
pub struct Variant {
    pub id: String,
    pub compiler: Compiler,
    pub lld: bool,
    /// Forces MSVC `link.exe`. clang-cl otherwise links with `lld-link`.
    pub msvc_link: bool,
    pub sccache: bool,
    pub release: bool,
}

impl Variant {
    pub fn parse(id: &str) -> Result<Variant> {
        let mut parts: Vec<&str> = id.split('-').collect();
        let release = match parts.pop() {
            Some("debug") => false,
            Some("release") => true,
            _ => bail!("variant '{id}' must end in -debug or -release"),
        };
        let compiler = match parts.first() {
            Some(&"msvc") => Compiler::Msvc,
            Some(&"clangcl") => Compiler::ClangCl,
            _ => bail!("variant '{id}' must start with msvc or clangcl"),
        };
        let flags = &parts[1..];
        if let Some(unknown) = flags
            .iter()
            .find(|flag| !matches!(**flag, "lld" | "link" | "sccache"))
        {
            bail!("variant '{id}': unknown flag '{unknown}'");
        }
        Ok(Variant {
            id: id.to_string(),
            compiler,
            lld: flags.contains(&"lld"),
            msvc_link: flags.contains(&"link"),
            sccache: flags.contains(&"sccache"),
            release,
        })
    }

    pub fn build_dir(&self, repo: &Path) -> PathBuf {
        repo.join("build").join(format!("toolbench-{}", self.id))
    }

    /// A fixed port per variant so concurrent trees never share a cache server.
    fn sccache_port(&self) -> u16 {
        let sum: u32 = self.id.bytes().map(u32::from).sum();
        (43000 + sum % 1000) as u16
    }
}

pub struct Llvm {
    pub bin: PathBuf,
}

impl Llvm {
    pub fn find(explicit: Option<&Path>) -> Option<Llvm> {
        let mut roots: Vec<PathBuf> = Vec::new();
        roots.extend(explicit.map(Path::to_path_buf));
        roots.extend(std::env::var_os("EXO_LLVM_DIR").map(PathBuf::from));
        roots.push(PathBuf::from(r"C:\Program Files\LLVM"));
        for root in roots {
            for sub in [
                "bin",
                "clang+llvm-23.1.5-x86_64-pc-windows-msvc/bin",
                "x64/bin",
            ] {
                let bin = root.join(sub);
                if bin.join("clang-cl.exe").is_file() && bin.join("lld-link.exe").is_file() {
                    return Some(Llvm { bin });
                }
            }
            if root.join("clang-cl.exe").is_file() && root.join("lld-link.exe").is_file() {
                return Some(Llvm { bin: root });
            }
        }
        None
    }

    pub fn clang_cl(&self) -> PathBuf {
        self.bin.join("clang-cl.exe")
    }

    pub fn lld_link(&self) -> PathBuf {
        self.bin.join("lld-link.exe")
    }
}

/// The MSVC environment, imported once. Empty when `cl.exe` is already on PATH.
pub fn toolchain_env() -> Result<EnvVars> {
    Ok(msvc::environment()?
        .map(|(_, vars)| vars)
        .unwrap_or_default())
}

pub fn apply_env(command: &mut Command, env: &EnvVars) {
    for (name, value) in env {
        command.env(name, value);
    }
}

pub fn out_dir(repo: &Path, common: &CommonArgs, kind: &str) -> Result<PathBuf> {
    let dir = match &common.out_dir {
        Some(dir) => dir.clone(),
        None => {
            let stamp = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)?
                .as_secs();
            repo.join(".workspace")
                .join("toolchain-bench")
                .join(format!("{kind}-{stamp}"))
        }
    };
    std::fs::create_dir_all(dir.join("logs"))?;
    Ok(dir)
}

fn tool_version(program: &Path, arg: &str, env: &EnvVars) -> Option<String> {
    let mut command = process::command(&program.to_string_lossy());
    if !arg.is_empty() {
        command.arg(arg);
    }
    apply_env(&mut command, env);
    let output = command.output().ok()?;
    let text = format!(
        "{}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    text.lines()
        .find(|line| !line.trim().is_empty())
        .map(|line| line.trim().to_string())
}

fn find_on_path(name: &str, env: &EnvVars) -> Option<PathBuf> {
    let path = env
        .iter()
        .find(|(key, _)| key.eq_ignore_ascii_case("path"))
        .map(|(_, value)| value.clone())
        .or_else(|| std::env::var("PATH").ok())?;
    std::env::split_paths(&path)
        .map(|dir| dir.join(name))
        .find(|candidate| candidate.is_file())
}

#[derive(Serialize)]
pub struct ToolInfo {
    pub name: &'static str,
    pub path: Option<PathBuf>,
    pub version: Option<String>,
}

pub fn detect_tools(llvm: Option<&Llvm>, env: &EnvVars) -> Vec<ToolInfo> {
    let mut tools = Vec::new();
    let mut add = |name: &'static str, path: Option<PathBuf>, arg: &str| {
        let version = path.as_deref().and_then(|p| tool_version(p, arg, env));
        tools.push(ToolInfo {
            name,
            path,
            version,
        });
    };
    // cl prints its version banner only when started without arguments.
    add("cl", find_on_path("cl.exe", env), "");
    add("link", find_on_path("link.exe", env), "/?");
    add("clang-cl", llvm.map(Llvm::clang_cl), "--version");
    add("lld-link", llvm.map(Llvm::lld_link), "--version");
    add("cmake", find_on_path("cmake.exe", env), "--version");
    add("ninja", find_on_path("ninja.exe", env), "--version");
    add("sccache", find_on_path("sccache.exe", env), "--version");
    add("rustc", find_on_path("rustc.exe", env), "--version");
    add("cargo", find_on_path("cargo.exe", env), "--version");
    add("vcperf", find_on_path("vcperf.exe", env), "/?");
    tools
}

pub fn run(repo: &Path, command: ToolchainBenchCommand) -> Result<()> {
    match command {
        ToolchainBenchCommand::Run(args) => run_variants(repo, args),
        ToolchainBenchCommand::Probe(args) => probe::run(repo, args),
        ToolchainBenchCommand::Rust(args) => rust::run(repo, args),
        ToolchainBenchCommand::Tools(common) => {
            let env = toolchain_env()?;
            let llvm = Llvm::find(common.llvm_dir.as_deref());
            println!(
                "{}",
                serde_json::to_string_pretty(&detect_tools(llvm.as_ref(), &env))?
            );
            Ok(())
        }
    }
}

#[derive(Serialize, Clone)]
pub struct Sample {
    pub variant: String,
    pub scenario: String,
    pub rep: usize,
    #[serde(flatten)]
    pub timing: Timing,
    pub sccache: Option<SccacheStats>,
    pub log: PathBuf,
}

#[derive(Serialize, Clone, Default)]
pub struct SccacheStats {
    pub requests_executed: u64,
    pub hits: u64,
    pub misses: u64,
    pub cache_dir_bytes: u64,
}

#[derive(Serialize)]
pub struct Skipped {
    pub variant: String,
    pub reason: String,
}

fn configure(
    repo: &Path,
    variant: &Variant,
    llvm: Option<&Llvm>,
    env: &EnvVars,
    extra: &[String],
    log: &Path,
) -> Result<()> {
    let dir = variant.build_dir(repo);
    if dir.join("build.ninja").is_file() && extra.is_empty() {
        return Ok(());
    }
    let preset = if variant.release {
        "windows-x64-ninja-release"
    } else {
        "windows-x64-ninja-debug"
    };
    let mut command = process::command("cmake");
    command.current_dir(repo).arg("--preset").arg(preset);
    command
        .arg("-B")
        .arg(&dir)
        .arg("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
    if variant.compiler == Compiler::ClangCl {
        let llvm = llvm.context("clang-cl is not available")?;
        // CMake rejects mixing clang-cl and cl.exe across languages, so both move.
        for language in ["C", "CXX"] {
            command.arg(format!(
                "-DCMAKE_{language}_COMPILER={}",
                slashed(&llvm.clang_cl())
            ));
        }
    }
    if variant.compiler == Compiler::ClangCl {
        command.args(extra);
        let shim = repo.join("build").join("toolbench-clang-cl-shim.cmake");
        std::fs::write(&shim, CLANG_CL_SHIM)?;
        command.arg(format!("-DCMAKE_PROJECT_INCLUDE={}", slashed(&shim)));
    }
    if variant.msvc_link {
        let cl = find_on_path("cl.exe", env).context("cl.exe is not on the toolchain PATH")?;
        command.arg(format!(
            "-DCMAKE_LINKER={}",
            slashed(&cl.with_file_name("link.exe"))
        ));
    }
    if variant.lld {
        let llvm = llvm.context("lld-link is not available")?;
        command.arg(format!("-DCMAKE_LINKER={}", slashed(&llvm.lld_link())));
    }
    if variant.sccache {
        command.args(["-DEXOSNAP_USE_SCCACHE=ON", "-DEXOSNAP_SCCACHE_ISOLATED=ON"]);
        command.arg(format!("-DEXOSNAP_SCCACHE_PORT={}", variant.sccache_port()));
    }
    apply_env(&mut command, env);
    let timing = timed(command, log)?;
    if timing.exit_code != 0 {
        bail!(
            "configure of {} failed with {}, see {}",
            variant.id,
            timing.exit_code,
            log.display()
        );
    }
    Ok(())
}

/// The project's Clang warning branch passes `-Wall`, which clang-cl reads as
/// `/Wall` (`-Weverything`), so every file fails on `-Wc++98-compat` under `-Werror`.
/// Re-stating the GNU-style set through `/clang:` (placed after the target
/// options, before the rule's `--` separator) restores the intended warnings and
/// keeps them non-fatal.
const CLANG_CL_SHIM: &str = "string(REPLACE \"<FLAGS>\" \"<FLAGS> /clang:-Wno-everything /clang:-Wall /clang:-Wextra /clang:-Wpedantic /clang:-Wno-error\" CMAKE_CXX_COMPILE_OBJECT \"${CMAKE_CXX_COMPILE_OBJECT}\")
";

fn slashed(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

pub fn sccache_command(cache_dir: &Path, port: u16, args: &[&str]) -> Command {
    let mut command = process::command("sccache");
    command
        .args(args)
        .env("SCCACHE_DIR", cache_dir)
        .env("SCCACHE_SERVER_PORT", port.to_string());
    command
}

pub fn dir_size(path: &Path) -> u64 {
    std::fs::read_dir(path)
        .map(|entries| {
            entries
                .flatten()
                .map(|entry| match entry.metadata() {
                    Ok(meta) if meta.is_dir() => dir_size(&entry.path()),
                    Ok(meta) => meta.len(),
                    Err(_) => 0,
                })
                .sum()
        })
        .unwrap_or(0)
}

pub fn read_sccache(cache_dir: &Path, port: u16) -> Option<SccacheStats> {
    let output = sccache_command(cache_dir, port, &["--show-stats", "--stats-format=json"])
        .output()
        .ok()?;
    let value: serde_json::Value = serde_json::from_slice(&output.stdout).ok()?;
    let stats = value.get("stats")?;
    let sum = |key: &str| -> u64 {
        stats
            .get(key)
            .and_then(|counts| counts.get("counts"))
            .and_then(serde_json::Value::as_object)
            .map(|counts| counts.values().filter_map(serde_json::Value::as_u64).sum())
            .unwrap_or(0)
    };
    Some(SccacheStats {
        requests_executed: stats
            .get("requests_executed")
            .and_then(serde_json::Value::as_u64)
            .unwrap_or(0),
        hits: sum("cache_hits"),
        misses: sum("cache_misses"),
        cache_dir_bytes: dir_size(cache_dir),
    })
}

/// Cache lifecycle steps are mandatory preconditions for measured scenarios.
pub fn reset_sccache(cache_dir: &Path, port: u16, wipe: bool) -> Result<()> {
    let stop = sccache_command(cache_dir, port, &["--stop-server"])
        .output()
        .context("stop sccache server")?;
    if !stop.status.success() {
        let message = String::from_utf8_lossy(&stop.stderr).to_ascii_lowercase();
        anyhow::ensure!(
            message.contains("no server running") || message.contains("not running"),
            "sccache stop-server failed: {}",
            String::from_utf8_lossy(&stop.stderr).trim()
        );
    }
    if wipe {
        match std::fs::remove_dir_all(cache_dir) {
            Ok(()) => {}
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => {
                return Err(error)
                    .with_context(|| format!("wipe sccache cache {}", cache_dir.display()));
            }
        }
    }
    for flag in ["--start-server", "--zero-stats"] {
        let output = sccache_command(cache_dir, port, &[flag])
            .output()
            .with_context(|| format!("sccache {flag}"))?;
        anyhow::ensure!(
            output.status.success(),
            "sccache {flag} failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        );
    }
    Ok(())
}

pub fn ninja(repo: &Path, dir: &Path, env: &EnvVars, jobs: usize, args: &[&str]) -> Command {
    let mut command = process::command("ninja");
    command
        .current_dir(repo)
        .arg("-C")
        .arg(dir)
        .arg(format!("-j{jobs}"))
        .args(args);
    apply_env(&mut command, env);
    command
}

pub fn touch(path: &Path) -> Result<()> {
    let file = std::fs::File::options().append(true).open(path)?;
    file.set_modified(std::time::SystemTime::now())?;
    Ok(())
}

fn require_success(timing: Timing, step: &str) -> Result<()> {
    anyhow::ensure!(
        timing.exit_code == 0,
        "{step} failed with exit code {}",
        timing.exit_code
    );
    Ok(())
}

fn measure_scenario(
    repo: &Path,
    variant: &Variant,
    scenario: &str,
    args: &RunArgs,
    env: &EnvVars,
    jobs: usize,
    log: &Path,
) -> Result<(Timing, Option<SccacheStats>)> {
    let dir = variant.build_dir(repo);
    let build = |target: &str| ninja(repo, &dir, env, jobs, &[target]);
    let cache_state = |wipe: bool| -> Result<()> {
        if variant.sccache {
            reset_sccache(&dir.join("sccache"), variant.sccache_port(), wipe)?;
        }
        Ok(())
    };
    let timing = match scenario {
        "cold" => {
            cache_state(true)?;
            require_success(
                timed(
                    ninja(repo, &dir, env, jobs, &["-t", "clean", &args.target]),
                    log,
                )?,
                "cold: ninja clean",
            )?;
            timed(build(&args.target), log)?
        }
        "warm" => {
            anyhow::ensure!(variant.sccache, "warm needs a compiler cache");
            require_success(
                timed(
                    ninja(repo, &dir, env, jobs, &["-t", "clean", &args.target]),
                    log,
                )?,
                "warm: ninja clean",
            )?;
            cache_state(false)?;
            timed(build(&args.target), log)?
        }
        "noop" => timed(build(&args.target), log)?,
        "touch-cpp" => {
            touch(&repo.join(&args.touch_cpp))?;
            cache_state(false)?;
            timed(build(&args.target), log)?
        }
        "touch-header" => {
            touch(&repo.join(&args.touch_header))?;
            cache_state(false)?;
            timed(build(&args.target), log)?
        }
        "fulllink" => {
            // Cleaning by linker rule removes only the executable outputs, so the
            // rebuild is a link from scratch with every object still current.
            let rule = format!(
                "CXX_EXECUTABLE_LINKER__{}_{}",
                args.link_target,
                if variant.release { "Release" } else { "Debug" }
            );
            require_success(
                timed(
                    ninja(repo, &dir, env, jobs, &["-t", "clean", "-r", &rule]),
                    log,
                )?,
                "fulllink: ninja clean",
            )?;
            timed(build(&args.link_target), log)?
        }
        "relink" => {
            let object_source = repo.join(&args.touch_cpp);
            touch(&object_source)?;
            timed(build(&args.link_target), log)?
        }
        other => bail!("unknown scenario '{other}'"),
    };
    let stats = variant
        .sccache
        .then(|| read_sccache(&dir.join("sccache"), variant.sccache_port()))
        .flatten();
    Ok((timing, stats))
}

fn run_variants(repo: &Path, args: RunArgs) -> Result<()> {
    let out = out_dir(repo, &args.common, "run")?;
    let env = toolchain_env()?;
    let llvm = Llvm::find(args.common.llvm_dir.as_deref());
    let tools = detect_tools(llvm.as_ref(), &env);
    let jobs = args
        .jobs
        .unwrap_or_else(|| std::thread::available_parallelism().map_or(8, usize::from));
    let revision = crate::git::Git::new(repo).head().unwrap_or_default();

    let mut samples: Vec<Sample> = Vec::new();
    let mut skipped: Vec<Skipped> = Vec::new();
    for id in &args.variants {
        let variant = Variant::parse(id)?;
        let needs_llvm = variant.compiler == Compiler::ClangCl || variant.lld;
        if needs_llvm && llvm.is_none() {
            skipped.push(Skipped {
                variant: id.clone(),
                reason: "LLVM (clang-cl, lld-link) not found".into(),
            });
            continue;
        }
        if variant.sccache
            && !tools
                .iter()
                .any(|t| t.name == "sccache" && t.path.is_some())
        {
            skipped.push(Skipped {
                variant: id.clone(),
                reason: "sccache not found".into(),
            });
            continue;
        }
        let configure_log = out.join("logs").join(format!("{id}-configure.log"));
        if let Err(error) = configure(
            repo,
            &variant,
            llvm.as_ref(),
            &env,
            &args.clang_cmake_args,
            &configure_log,
        ) {
            skipped.push(Skipped {
                variant: id.clone(),
                reason: format!("{error:#}"),
            });
            continue;
        }
        // The first build compiles dependencies and fills lazily generated
        // sources, so it never counts as a scenario measurement.
        let prime_log = out.join("logs").join(format!("{id}-prime.log"));
        let prime = timed(
            ninja(
                repo,
                &variant.build_dir(repo),
                &env,
                jobs,
                &[&args.target, &args.link_target],
            ),
            &prime_log,
        )?;
        if prime.exit_code != 0 {
            skipped.push(Skipped {
                variant: id.clone(),
                reason: format!(
                    "prime build failed ({}), see {}",
                    prime.exit_code,
                    prime_log.display()
                ),
            });
            continue;
        }
        for scenario in &args.scenarios {
            if scenario == "warm" && !variant.sccache {
                continue;
            }
            for rep in 1..=args.reps {
                let log = out.join("logs").join(format!("{id}-{scenario}-{rep}.log"));
                let (timing, sccache) =
                    measure_scenario(repo, &variant, scenario, &args, &env, jobs, &log)?;
                eprintln!(
                    "{id} {scenario} #{rep}: {:.1}s exit {}",
                    timing.wall_seconds, timing.exit_code
                );
                samples.push(Sample {
                    variant: id.clone(),
                    scenario: scenario.clone(),
                    rep,
                    timing,
                    sccache,
                    log,
                });
            }
        }
        if let Some(per_tu) = report::slowest_edges(&variant.build_dir(repo), 15) {
            std::fs::write(
                out.join(format!("{id}-slowest-edges.json")),
                serde_json::to_string_pretty(&per_tu)?,
            )?;
        }
    }
    let summary = samples_summary(&samples);
    report::write_run_report(
        &out,
        &report::RunReport {
            revision,
            target: args.target.clone(),
            jobs,
            reps: args.reps,
            baseline: args.variants.first().cloned().unwrap_or_default(),
            tools,
            summary,
            samples,
            skipped,
        },
    )?;
    println!("report: {}", out.join("report.md").display());
    Ok(())
}

/// Median wall seconds per (variant, scenario) over the successful samples.
pub fn samples_summary(samples: &[Sample]) -> Vec<(String, String, f64, Option<u64>)> {
    let mut keys: Vec<(String, String)> = Vec::new();
    for sample in samples {
        let key = (sample.variant.clone(), sample.scenario.clone());
        if !keys.contains(&key) {
            keys.push(key);
        }
    }
    keys.into_iter()
        .filter_map(|(variant, scenario)| {
            let group: Vec<&Sample> = samples
                .iter()
                .filter(|s| {
                    s.variant == variant && s.scenario == scenario && s.timing.exit_code == 0
                })
                .collect();
            let wall = median(
                &group
                    .iter()
                    .map(|s| s.timing.wall_seconds)
                    .collect::<Vec<_>>(),
            )?;
            let peak = group
                .iter()
                .filter_map(|s| s.timing.peak_commit_bytes)
                .max();
            Some((variant, scenario, wall, peak))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn variant_ids_parse_compiler_linker_and_cache() {
        let v = Variant::parse("clangcl-lld-sccache-release").unwrap();
        assert_eq!(v.compiler, Compiler::ClangCl);
        assert!(v.lld && v.sccache && v.release);
        let plain = Variant::parse("msvc-debug").unwrap();
        assert!(!plain.lld && !plain.sccache && !plain.release);
    }

    #[test]
    fn malformed_variant_ids_are_rejected() {
        assert!(Variant::parse("msvc").is_err());
        assert!(Variant::parse("gcc-debug").is_err());
        assert!(Variant::parse("msvc-fast-debug").is_err());
    }

    #[test]
    fn failed_measurement_precondition_is_rejected() {
        let timing = Timing {
            wall_seconds: 0.1,
            peak_commit_bytes: None,
            exit_code: 1,
        };
        assert!(require_success(timing.clone(), "ninja clean").is_err());
        assert!(
            require_success(
                Timing {
                    exit_code: 0,
                    ..timing
                },
                "ninja clean"
            )
            .is_ok()
        );
    }

    #[test]
    fn sccache_ports_stay_in_the_reserved_range() {
        let port = Variant::parse("msvc-sccache-debug").unwrap().sccache_port();
        assert!((43000..44000).contains(&port));
    }
}
