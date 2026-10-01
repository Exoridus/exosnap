//! Deterministic screenshots of the Qt Quick frontend, taken by the app itself.
//!
//! Each shot is one `exosnap.exe --visual-test` process: the app seeds the
//! requested state, waits for its page loader, saves `QQuickWindow::grabWindow()`
//! plus one PNG per visible capture-excluded overlay, and exits. This module
//! only orchestrates. It never photographs the desktop, which would record
//! whatever else is on the developer's screen and could not show
//! capture-excluded overlays anyway.
//!
//! A run writes into one directory per source state, named after the commit,
//! so repeated runs on the same tree replace their own shots and older commits
//! stay available for comparison.
//!
//! The images prove scene content, not desktop composition, alpha or capture
//! exclusion; `exo-verify` owns those on the real desktop.

pub mod catalog;
pub mod invocation;
pub mod report;
pub mod sheet;

use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use anyhow::{Context, bail};
use clap::{Args, Subcommand};
use sha2::{Digest, Sha256};

use crate::build_artifacts::resolve_built_artifact;
use crate::git::Git;
use crate::host_lock::{self, LockKind};
use crate::test::environment::{ChildEnv, resolve_qt};
use crate::test::receipt::{format_system_time, utc_now};

use catalog::ProductValues;
use invocation::{Appearance, Axes, ShotSpec, Variant};
use report::{Binary, RunInfo, ShotResult, Status};

/// Without a subcommand, takes the sweep the options describe, which by
/// default is the whole suite.
#[derive(Args, Debug)]
#[command(args_conflicts_with_subcommands = true)]
pub struct ScreenshotArgs {
    #[command(subcommand)]
    pub command: Option<ScreenshotCommand>,
    #[command(flatten)]
    pub sweep: SweepArgs,
}

#[derive(Subcommand, Debug)]
pub enum ScreenshotCommand {
    /// Takes one screenshot, once per combination of the axis options.
    Shot(Box<ShotArgs>),
    /// Takes every shot of one or more named sets (see `list`).
    Sweep(Box<SweepArgs>),
    /// Lists the named sets and the values the current sources accept.
    List,
}

#[derive(Args, Debug)]
pub struct ShotArgs {
    #[command(flatten)]
    pub spec: ShotSpec,
    /// State part of the file name. Derived from the selection when omitted.
    #[arg(long)]
    pub name: Option<String>,
    #[command(flatten)]
    pub axes: Axes,
    #[command(flatten)]
    pub run: RunOptions,
}

#[derive(Args, Debug)]
pub struct SweepArgs {
    /// Comma-separated set names.
    #[arg(long, value_delimiter = ',', default_value = "all")]
    pub set: Vec<String>,
    /// Keeps only shots whose name contains this text.
    #[arg(long)]
    pub filter: Option<String>,
    #[command(flatten)]
    pub axes: Axes,
    #[command(flatten)]
    pub run: RunOptions,
}

#[derive(Args, Debug, Clone)]
pub struct RunOptions {
    /// The exosnap.exe to photograph. Defaults to the most recently built one
    /// across the local build trees.
    #[arg(long)]
    pub exe: Option<PathBuf>,
    /// Output directory. Defaults to
    /// `.workspace/screenshots/<commit date>_<short hash>[-dirty]`.
    #[arg(long)]
    pub out_dir: Option<PathBuf>,
    /// Appended to the default output directory name.
    #[arg(long)]
    pub label: Option<String>,
    /// Shots taken in parallel. Each is its own app process and window.
    #[arg(long, default_value_t = 4)]
    pub jobs: usize,
    /// Seconds before a shot's process is killed and reported as timed out.
    #[arg(long, default_value_t = 60)]
    pub timeout_secs: u64,
    /// Prints the app invocations without running anything.
    #[arg(long)]
    pub dry_run: bool,
}

struct Job {
    /// `<page>_<state>_<theme>_<size>`, for logs and the contact sheet.
    name: String,
    /// `<page>/<state>_<theme>_<size>`, relative to the run directory.
    stem: String,
    spec: ShotSpec,
    variant: Variant,
}

pub fn run(repo_root: &Path, args: ScreenshotArgs) -> anyhow::Result<ExitCode> {
    let values = ProductValues::load(repo_root)?;
    let (user_appearance, user_accent) = user_theme();
    // Without a subcommand the run shows the suite the way this developer
    // sees the app; `sweep` is the explicit matrix and covers both appearances.
    let explicit_sweep = matches!(args.command, Some(ScreenshotCommand::Sweep(_)));
    let command = args
        .command
        .unwrap_or_else(|| ScreenshotCommand::Sweep(Box::new(args.sweep)));
    let (shots, axes, options) = match command {
        ScreenshotCommand::List => {
            list(&values)?;
            return Ok(ExitCode::SUCCESS);
        }
        ScreenshotCommand::Shot(args) => {
            let ShotArgs {
                spec,
                name,
                axes,
                run,
            } = *args;
            let name = match name
                .map(|n| invocation::sanitize(&n))
                .filter(|n| !n.is_empty())
            {
                Some(state) => format!("{}_{state}", spec.page_id()),
                None => spec.derived_name(),
            };
            let axes = axes.or_defaults(&[user_appearance], &user_accent);
            (vec![(name, spec)], axes, run)
        }
        ScreenshotCommand::Sweep(args) => {
            let SweepArgs {
                set,
                filter,
                axes,
                run,
            } = *args;
            let mut shots: Vec<(String, ShotSpec)> = Vec::new();
            for name in &set {
                for shot in catalog::set(name, &values)? {
                    if !shots.iter().any(|(existing, _)| *existing == shot.0) {
                        shots.push(shot);
                    }
                }
            }
            if let Some(filter) = &filter {
                shots.retain(|(name, _)| name.contains(filter.as_str()));
            }
            if shots.is_empty() {
                bail!("no shot matches the selected sets and filter");
            }
            let appearances: &[Appearance] = if explicit_sweep {
                &[Appearance::Dark, Appearance::Light]
            } else {
                &[user_appearance]
            };
            (shots, axes.or_defaults(appearances, &user_accent), run)
        }
    };

    for (_, spec) in &shots {
        values.check(spec, &axes.accent)?;
    }
    let variants = axes.variants();
    let jobs: Vec<Job> = shots
        .iter()
        .flat_map(|(name, spec)| {
            // Every derived name is `<page>_<state>`, and a page id has no `_`.
            let (page, state) = name.split_once('_').unwrap_or(("record", name));
            variants.iter().map(move |variant| Job {
                name: format!("{page}_{state}_{}", variant.suffix()),
                stem: format!("{page}/{state}_{}", variant.suffix()),
                spec: spec.clone(),
                variant: variant.clone(),
            })
        })
        .collect();

    let binary = resolve_binary(repo_root, options.exe.as_deref())?;
    if options.dry_run {
        println!("exe: {}", binary.path);
        for job in &jobs {
            let (args, env) = job
                .spec
                .invocation(&job.variant, &format!("{}.png", job.stem));
            let env: Vec<String> = env.iter().map(|(k, v)| format!("{k}={v}")).collect();
            println!("{}: {} {}", job.stem, env.join(" "), args.join(" "));
        }
        println!("{} shot(s)", jobs.len());
        return Ok(ExitCode::SUCCESS);
    }

    let git = Git::new(repo_root);
    let out_dir = output_dir(repo_root, &git, &options);
    std::fs::create_dir_all(out_dir.join("logs"))
        .with_context(|| format!("could not create {}", out_dir.display()))?;

    let qt = resolve_qt(
        repo_root,
        std::env::var_os("EXOSNAP_QT_ROOT").as_deref(),
        std::env::var_os("SystemDrive").as_deref(),
    )?;
    if let Some(warning) = &qt.warning {
        println!("{warning}");
    }

    // A build relinking the binary halfway through a sweep would mix two
    // builds in one contact sheet under one hash.
    let _tree_lock = match &binary.tree {
        Some(tree) => {
            let lock = host_lock::acquire(
                LockKind::Tree,
                Some(&repo_root.join(tree)),
                &format!("exo-dev screenshot {}", std::process::id()),
            )?;
            if lock.waited() >= Duration::from_secs(2) {
                println!(
                    "waited {:.0}s for the build tree lock",
                    lock.waited().as_secs_f64()
                );
            }
            Some(lock)
        }
        None => None,
    };
    let binary = hash_binary(binary)?;
    println!(
        "{} shot(s) of {} (built {} UTC) into {}",
        jobs.len(),
        binary.path,
        binary.built_at_utc,
        out_dir.display()
    );

    let scratch = std::env::temp_dir().join(format!("exo-dev-screenshot-{}", std::process::id()));
    let mut child_env = ChildEnv::default();
    if let Some(bin) = &qt.bin {
        child_env.prepend_path(bin);
    }
    // Qt only writes to stderr from a GUI-subsystem binary when forced to, and
    // the log is where a request the app ignored becomes visible.
    child_env.set("QT_FORCE_STDERR_LOGGING", "1");
    // Shared across shots of one binary, so each shot does not recompile QML.
    child_env.set("QML_DISK_CACHE_PATH", scratch.join("qmlcache"));

    let context = RunContext {
        exe: PathBuf::from(&binary.path),
        binary_sha256: binary.sha256.clone(),
        out_dir: out_dir.clone(),
        scratch: scratch.clone(),
        child_env,
        timeout: Duration::from_secs(options.timeout_secs.max(1)),
    };
    let results = run_jobs(&context, &jobs, options.jobs.max(1));
    let sheets = overlay_sheets(&out_dir, &jobs, &results)?;
    let _ = std::fs::remove_dir_all(&scratch);

    let created_at_utc = utc_now();
    let info = RunInfo {
        created_at_utc: &created_at_utc,
        binary: &binary,
        head: git.head(),
        dirty: git.dirty(),
        source_fingerprint: crate::test::fingerprint::identify(repo_root)
            .fingerprint()
            .map(str::to_string),
        version: project_version(repo_root),
        qt_bin: qt.bin.as_ref().map(|b| b.display().to_string()),
        sheets,
    };
    let total = report::write(&out_dir, &info, &results)?;

    let count = |status: Status| results.iter().filter(|r| r.status == status).count();
    let failed = count(Status::Failed) + count(Status::TimedOut);
    println!(
        "\n{} ok, {} with warnings, {} failed ({total} shot(s) in this directory). Contact sheet: {}",
        count(Status::Ok),
        count(Status::Warn),
        failed,
        out_dir.join("index.html").display()
    );
    Ok(if failed == 0 {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    })
}

fn list(values: &ProductValues) -> anyhow::Result<()> {
    for name in catalog::SET_NAMES {
        let shots = catalog::set(name, values)?;
        println!("{name} ({} shot(s))", shots.len());
        if *name != "all" {
            for (shot, _) in shots {
                println!("  {shot}");
            }
        }
    }
    println!("\nrecord states: {}", values.record_states.join(", "));
    println!("accents: {}", values.accents.join(", "));
    Ok(())
}

fn resolve_binary(repo_root: &Path, explicit: Option<&Path>) -> anyhow::Result<Binary> {
    let (path, tree, built_at) = match explicit {
        Some(path) => {
            let metadata = std::fs::metadata(path)
                .with_context(|| format!("--exe {} does not exist", path.display()))?;
            (std::path::absolute(path)?, None, metadata.modified().ok())
        }
        None => {
            let artifact = resolve_built_artifact(repo_root, Path::new("app/exosnap.exe"))?
                .context(
                    "no local build of exosnap.exe; build one first \
                     (for example `cargo exo-dev test --filter quick.`) or pass --exe",
                )?;
            // The build-tree search joins with `/`; one separator reads better.
            let path: PathBuf = artifact.path.components().collect();
            (path, Some(artifact.tree), Some(artifact.built_at))
        }
    };
    Ok(Binary {
        path: path.display().to_string(),
        tree,
        built_at_utc: built_at.map(format_system_time).unwrap_or_default(),
        bytes: 0,
        sha256: String::new(),
    })
}

fn hash_binary(mut binary: Binary) -> anyhow::Result<Binary> {
    let mut file =
        File::open(&binary.path).with_context(|| format!("could not open {}", binary.path))?;
    let mut hasher = Sha256::new();
    binary.bytes = std::io::copy(&mut file, &mut hasher)?;
    binary.sha256 = hex::encode(hasher.finalize());
    Ok(binary)
}

/// Appearance and accent from the developer's own ExoSnap settings, dark and
/// aqua (the product defaults) when there are none. Only these two are read:
/// the rest of that configuration would make a screenshot depend on this
/// machine, so every shot still runs in a throwaway configuration.
fn user_theme() -> (Appearance, String) {
    let text = std::env::var_os("LOCALAPPDATA")
        .map(|dir| PathBuf::from(dir).join("ExoSnap").join("settings.ini"))
        .and_then(|path| std::fs::read_to_string(path).ok())
        .unwrap_or_default();
    parse_user_theme(&text)
}

fn parse_user_theme(text: &str) -> (Appearance, String) {
    let value = |key: &str| {
        text.lines()
            .filter_map(|line| line.split_once('='))
            .find(|(name, _)| name.trim() == key)
            .map(|(_, value)| value.trim().to_string())
            .filter(|value| !value.is_empty())
    };
    let appearance = match value("appearance_id").as_deref() {
        Some("light") => Appearance::Light,
        _ => Appearance::Dark,
    };
    let accent = value("accent_id").unwrap_or_else(|| "aqua".to_string());
    (appearance, accent)
}

/// `<commit date>_<short hash>`, with `-dirty` when the tree has uncommitted
/// changes. The commit date rather than today's keeps one commit in one
/// directory across days, while the directories still sort chronologically.
/// A dirty run gets its own directory so iterating on uncommitted work never
/// overwrites the shots of the clean commit.
fn output_dir(repo_root: &Path, git: &Git, options: &RunOptions) -> PathBuf {
    if let Some(dir) = &options.out_dir {
        return repo_root.join(dir);
    }
    let first_line = |args: &[&str]| {
        let (code, stdout) = git.run(args);
        let line = stdout.lines().next().unwrap_or("").trim().to_string();
        (code == 0 && !line.is_empty()).then_some(line)
    };
    let date = first_line(&["log", "-1", "--format=%cs"]).unwrap_or_else(|| "unknown".into());
    let hash = first_line(&["rev-parse", "--short=8", "HEAD"]).unwrap_or_else(|| "unknown".into());
    let mut leaf = format!("{date}_{hash}");
    if git.dirty() {
        leaf.push_str("-dirty");
    }
    if let Some(label) = options
        .label
        .as_deref()
        .map(invocation::sanitize)
        .filter(|label| !label.is_empty())
    {
        leaf = format!("{leaf}_{label}");
    }
    repo_root.join(".workspace").join("screenshots").join(leaf)
}

/// The version `project()` in the top-level CMakeLists.txt declares, which is
/// what a local build of the app carries.
fn project_version(repo_root: &Path) -> Option<String> {
    let text = std::fs::read_to_string(repo_root.join("CMakeLists.txt")).ok()?;
    regex::Regex::new(r"project\(exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)")
        .unwrap()
        .captures(&text)
        .map(|c| c[1].to_string())
}

/// One composed sheet per theme and size, from the overlay grabs of the HUD
/// shots in this run.
fn overlay_sheets(
    out_dir: &Path,
    jobs: &[Job],
    results: &[ShotResult],
) -> anyhow::Result<Vec<String>> {
    let mut groups: Vec<(String, Vec<Vec<String>>)> = Vec::new();
    for (job, result) in jobs.iter().zip(results) {
        let is_hud = job
            .spec
            .overlay_state
            .as_deref()
            .is_some_and(|state| catalog::HUD_STATES.contains(&state));
        if !is_hud || result.overlays.is_empty() {
            continue;
        }
        let key = job.variant.suffix();
        match groups.iter_mut().find(|(existing, _)| *existing == key) {
            Some((_, rows)) => rows.push(result.overlays.clone()),
            None => groups.push((key, vec![result.overlays.clone()])),
        }
    }
    let mut sheets = Vec::new();
    for (suffix, rows) in groups {
        std::fs::create_dir_all(out_dir.join("overlays"))?;
        if let Some(file) = sheet::compose(out_dir, &format!("overlays/sheet_{suffix}"), &rows)? {
            println!("overlay sheet: {file}");
            sheets.push(file);
        }
    }
    Ok(sheets)
}

struct RunContext {
    exe: PathBuf,
    binary_sha256: String,
    out_dir: PathBuf,
    scratch: PathBuf,
    child_env: ChildEnv,
    timeout: Duration,
}

fn run_jobs(context: &RunContext, jobs: &[Job], parallel: usize) -> Vec<ShotResult> {
    let next = AtomicUsize::new(0);
    let done = AtomicUsize::new(0);
    let results: Mutex<Vec<(usize, ShotResult)>> = Mutex::new(Vec::with_capacity(jobs.len()));
    std::thread::scope(|scope| {
        for _ in 0..parallel.min(jobs.len()) {
            scope.spawn(|| {
                loop {
                    let index = next.fetch_add(1, Ordering::SeqCst);
                    let Some(job) = jobs.get(index) else { break };
                    let result = take(context, index, job);
                    let finished = done.fetch_add(1, Ordering::SeqCst) + 1;
                    println!(
                        "[{finished:>3}/{}] {:<7} {} ({:.1}s){}",
                        jobs.len(),
                        result.status.id(),
                        job.stem,
                        result.duration_ms as f64 / 1000.0,
                        result
                            .warnings
                            .first()
                            .map(|w| format!("  {w}"))
                            .unwrap_or_default()
                    );
                    results.lock().unwrap().push((index, result));
                }
            });
        }
    });
    let mut results = results.into_inner().unwrap();
    results.sort_by_key(|(index, _)| *index);
    results.into_iter().map(|(_, result)| result).collect()
}

fn take(context: &RunContext, index: usize, job: &Job) -> ShotResult {
    let file = format!("{}.png", job.stem);
    let png = context.out_dir.join(&file);
    let log_relative = format!("logs/{}.log", job.name);
    let log_path = context.out_dir.join(&log_relative);
    let (args, env) = job
        .spec
        .invocation(&job.variant, &png.display().to_string());
    let mut result = ShotResult {
        name: job.name.clone(),
        file: file.clone(),
        overlays: Vec::new(),
        args: args.clone(),
        env: env.clone(),
        exit_code: None,
        duration_ms: 0,
        status: Status::Failed,
        warnings: Vec::new(),
        log: log_relative,
        binary_sha256: context.binary_sha256.clone(),
    };

    // A rerun replaces the shot. Its previous overlay grabs go too, or an
    // overlay that is no longer shown would still be listed with it.
    for stale in overlay_files(&context.out_dir, &job.stem) {
        let _ = std::fs::remove_file(context.out_dir.join(stale));
    }
    let _ = std::fs::remove_file(&png);
    if let Some(parent) = png.parent()
        && let Err(error) = std::fs::create_dir_all(parent)
    {
        result
            .warnings
            .push(format!("could not create {}: {error}", parent.display()));
        return result;
    }

    let started = Instant::now();
    let outcome = spawn_and_wait(context, index, &args, &env, &log_path);
    result.duration_ms = started.elapsed().as_millis() as u64;
    match outcome {
        Ok(Some(code)) => result.exit_code = Some(code),
        Ok(None) => result.status = Status::TimedOut,
        Err(error) => result.warnings.push(format!("{error:#}")),
    }
    result.warnings.extend(log_warnings(&log_path));
    result.overlays = overlay_files(&context.out_dir, &job.stem);

    let saved = std::fs::metadata(&png).is_ok_and(|m| m.len() > 0);
    if result.exit_code == Some(0) && saved {
        result.status = if result.warnings.is_empty() {
            Status::Ok
        } else {
            Status::Warn
        };
    }
    result
}

/// Runs one app process. `Ok(None)` is a timeout.
fn spawn_and_wait(
    context: &RunContext,
    index: usize,
    args: &[String],
    env: &[(String, String)],
    log_path: &Path,
) -> anyhow::Result<Option<i32>> {
    let config_dir = context.scratch.join(format!("config-{index}"));
    std::fs::create_dir_all(&config_dir)?;
    let log = File::create(log_path)?;

    let mut command = Command::new(&context.exe);
    // A seed or scale left in the caller's environment would silently apply
    // to every shot.
    for (name, _) in std::env::vars_os() {
        let name = name.to_string_lossy().to_ascii_uppercase();
        if name.starts_with("EXOSNAP_VISUAL_") || name == "QT_SCALE_FACTOR" {
            command.env_remove(&name);
        }
    }
    context.child_env.apply(&mut command);
    // Fresh per shot: a crash report or recovery entry left by an earlier
    // shot would otherwise cover every later one.
    command.env("EXOSNAP_CONFIG_DIR", &config_dir);
    for (name, value) in env {
        command.env(name, value);
    }
    command
        .args(args)
        .current_dir(&context.out_dir)
        .stdin(Stdio::null())
        .stdout(Stdio::from(log.try_clone()?))
        .stderr(Stdio::from(log));
    let mut child = command
        .spawn()
        .with_context(|| format!("could not start {}", context.exe.display()))?;

    let deadline = Instant::now() + context.timeout;
    let code = loop {
        if let Some(status) = child.try_wait()? {
            break Some(status.code().unwrap_or(-1));
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            break None;
        }
        std::thread::sleep(Duration::from_millis(50));
    };
    let _ = std::fs::remove_dir_all(&config_dir);
    Ok(code)
}

/// App log lines that say part of the request did not apply: a harness option
/// that found nothing to act on, an overlay that failed to save, a QML error.
fn log_warnings(log_path: &Path) -> Vec<String> {
    let Ok(file) = File::open(log_path) else {
        return Vec::new();
    };
    let pattern = regex::Regex::new(
        r"--(?:visual|record-visual|overlay-visual|settings-visual)[a-z-]*:|SAVE FAILED|\.qml:\d+.*(?:Error|Unable|Cannot|polish\(\) loop)",
    )
    .unwrap();
    let mut warnings: Vec<String> = Vec::new();
    for line in BufReader::new(file).lines().map_while(Result::ok) {
        let line = line.trim().to_string();
        if pattern.is_match(&line) && !warnings.contains(&line) {
            warnings.push(line);
        }
    }
    warnings
}

/// The overlay PNGs the app wrote next to the main capture,
/// `<stem>.<overlayObjectName>.png`, relative to the run directory.
fn overlay_files(out_dir: &Path, stem: &str) -> Vec<String> {
    let (directory, leaf) = match stem.rsplit_once('/') {
        Some((directory, leaf)) => (Some(directory), leaf),
        None => (None, stem),
    };
    let prefix = format!("{leaf}.");
    let main = format!("{leaf}.png");
    let search = directory.map_or_else(|| out_dir.to_path_buf(), |d| out_dir.join(d));
    let Ok(entries) = std::fs::read_dir(search) else {
        return Vec::new();
    };
    let mut files: Vec<String> = entries
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.file_name().to_string_lossy().into_owned())
        .filter(|file| file.starts_with(&prefix) && file.ends_with(".png") && *file != main)
        .map(|file| match directory {
            Some(directory) => format!("{directory}/{file}"),
            None => file,
        })
        .collect();
    files.sort();
    files
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_user_theme_comes_from_the_settings_file() {
        let text = "[General]
appearance_id=light
accent_id=violet
other=1
";
        assert_eq!(
            parse_user_theme(text),
            (Appearance::Light, "violet".to_string())
        );
        assert_eq!(parse_user_theme(""), (Appearance::Dark, "aqua".to_string()));
    }

    #[test]
    fn ignored_harness_requests_surface_as_warnings() {
        let dir = tempfile::tempdir().unwrap();
        let log = dir.path().join("shot.log");
        std::fs::write(
            &log,
            "quick-visual-test: isolated config dir C:/x\n\
             --visual-dialog: no dialog named preset-renam\n\
             overlay-grab: quickOverlayRecording skipped (not visible) geometry=0x0\n\
             overlay-grab: quickOverlayToast SAVE FAILED 10x10\n\
             qrc:/qt/qml/ExoSnap/Quick/RecordPage.qml:42: TypeError: Cannot read property 'x' of null\n\
             qrc:/qt/qml/ExoSnap/Quick/DiagnosticsPage.qml:397:21: QML Flow: possible QQuickItem::polish() loop\n",
        )
        .unwrap();
        let warnings = log_warnings(&log);
        assert_eq!(warnings.len(), 4, "{warnings:?}");
        assert!(warnings[0].starts_with("--visual-dialog"));
    }

    #[test]
    fn overlays_are_matched_by_their_shot_stem() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir(dir.path().join("record")).unwrap();
        for file in [
            "record/default_dark-aqua_1600x1000.png",
            "record/default_dark-aqua_1600x1000.quickOverlayRecording.png",
            "record/paused_dark-aqua_1600x1000.png",
        ] {
            std::fs::write(dir.path().join(file), b"x").unwrap();
        }
        assert_eq!(
            overlay_files(dir.path(), "record/default_dark-aqua_1600x1000"),
            ["record/default_dark-aqua_1600x1000.quickOverlayRecording.png"]
        );
    }
}
