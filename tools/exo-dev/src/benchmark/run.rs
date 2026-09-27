//! One accepted (or calibration) run of the frontend recording benchmark.
//!
//! Development tooling. Nothing here belongs in the shipped application: the
//! application knows how to record and how to report on itself, and knows
//! nothing about Superposition, run identities or artifact layouts.
//!
//! The run is:
//!
//!   verify display topology
//!   -> start the external workload (when the scenario has one)
//!   -> let the workload warm up
//!   -> start the ExoSnap frontend with --auto-record
//!   -> the app's own warm-up/measure/stop sequence runs inside that
//!   -> stop the workload
//!   -> collect artifacts into one run directory
//!
//! The ExoSnap measurement window is owned entirely by the application
//! (--benchmark-warmup + --duration), so nothing here has to guess when the
//! measured interval opened.
//!
//! Accepted measurements need a Release build configured with
//! `EXOSNAP_BUILD_BENCHMARK_HARNESS=ON`. A Debug binary is only allowed
//! through `calibration`, and the run directory is then marked as such.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use anyhow::{Context, bail};
use serde::{Deserialize, Serialize};

use crate::benchmark::topology::{self, TopologyExpectation};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Frontend {
    /// Removed with the Qt Quick cutover. Kept as a variant so the refusal in
    /// `resolve_executable` has something concrete to refuse; do not add a
    /// Widgets build back to make this branch reachable in a normal run.
    Widgets,
    Quick,
}

impl Frontend {
    pub fn as_str(self) -> &'static str {
        match self {
            Frontend::Widgets => "widgets",
            Frontend::Quick => "quick",
        }
    }
}

impl std::str::FromStr for Frontend {
    type Err = String;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "widgets" => Ok(Frontend::Widgets),
            "quick" => Ok(Frontend::Quick),
            other => Err(format!(
                "unknown frontend '{other}' (expected widgets or quick)"
            )),
        }
    }
}

impl std::fmt::Display for Frontend {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Clone, Debug, Deserialize)]
pub struct WorkloadCli {
    pub api: String,
    pub resolution: String,
    pub fullscreen: i32,
    pub quality: String,
    pub textures: String,
    pub dof: i32,
    pub motion_blur: i32,
    pub sound: i32,
    #[serde(default)]
    pub mode: Option<String>,
    #[serde(default)]
    pub scene: Option<i32>,
    #[serde(default)]
    pub frame: Option<i32>,
    #[serde(default)]
    pub mode_duration_minutes: i64,
}

#[derive(Clone, Debug, Deserialize)]
pub struct Workload {
    pub kind: String,
    #[serde(default)]
    pub warmup_seconds: u64,
    #[serde(default)]
    pub cli: Option<WorkloadCli>,
}

#[derive(Clone, Debug, Deserialize)]
pub struct ExoSnapSettings {
    pub target: String,
    pub duration_seconds: u64,
    pub frame_rate: u32,
    pub container: String,
    pub video_codec: String,
    pub audio_codec: String,
    pub chroma: String,
    pub bit_depth: u32,
    pub hdr_mode: String,
    pub warmup_seconds: u64,
    #[serde(default)]
    pub audio_rows: Vec<String>,
    #[serde(default)]
    pub target_window_title: Option<String>,
}

#[derive(Clone, Debug, Deserialize)]
pub struct ScenarioDefinition {
    pub id: String,
    pub topology: TopologyExpectation,
    pub workload: Workload,
    pub exosnap: ExoSnapSettings,
    #[serde(default)]
    pub source_notes: Option<String>,
}

/// Where a named scenario's definition lives, relative to the repository root.
pub fn scenario_path(repo_root: &Path, name: &str) -> PathBuf {
    repo_root
        .join("tools/benchmark/scenarios")
        .join(format!("{name}.json"))
}

pub fn load_scenario_definition(path: &Path) -> anyhow::Result<ScenarioDefinition> {
    let text = std::fs::read_to_string(path).with_context(|| {
        format!(
            "unknown scenario. Expected a definition at {}",
            path.display()
        )
    })?;
    serde_json::from_str(&text)
        .with_context(|| format!("could not parse scenario definition {}", path.display()))
}

/// Everything one `run` call needs: the scenario definition plus the run's
/// own parameters (which frontend, which executables, where results go).
pub struct Scenario {
    pub definition: ScenarioDefinition,
    pub frontend: Frontend,
    pub run_index: u32,
    pub output_root: PathBuf,
    pub widgets_exe: Option<PathBuf>,
    pub quick_exe: PathBuf,
    pub superposition_cli: PathBuf,
    /// A disposable validation run: allowed to use a Debug binary, excluded
    /// from campaign statistics, written into a separate tree.
    pub calibration: bool,
    /// Escape hatch for working on the tooling without the target displays
    /// attached. An accepted run must never use it.
    pub skip_topology_check: bool,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct WindowPlacement {
    pub screen_device_name: String,
    pub screen_primary: bool,
    pub screen_bounds: String,
}

#[derive(Clone, Debug, Serialize)]
pub struct RunManifest {
    pub run_id: String,
    pub frontend: String,
    pub scenario: String,
    pub run_index: u32,
    pub calibration: bool,
    pub topology_verified: bool,
    pub executable: PathBuf,
    pub exosnap_args: Vec<String>,
    pub exosnap_exit_code: i32,
    pub workload_kind: String,
    pub workload_args: Vec<String>,
    pub exosnap_report: Option<String>,
    pub recording: Option<String>,
    pub superposition_csv: Option<String>,
    pub superposition_txt: Option<String>,
    pub attached_panels: Vec<String>,
    pub frontend_window: Option<WindowPlacement>,
}

/// The Qt Widgets frontend was removed with the Qt Quick cutover, so there is
/// no executable to measure by default. Asking for it anyway fails loudly
/// rather than silently falling back to the Quick binary and mislabelling the
/// result: the archived comparison under the historical results tree is the
/// record of that A/B, and re-running it needs a caller-supplied binary.
pub fn resolve_executable(
    frontend: Frontend,
    widgets_exe: Option<&Path>,
    quick_exe: &Path,
) -> anyhow::Result<PathBuf> {
    match frontend {
        Frontend::Quick => Ok(quick_exe.to_path_buf()),
        Frontend::Widgets => match widgets_exe {
            Some(exe) => Ok(exe.to_path_buf()),
            None => bail!(
                "The Qt Widgets frontend was removed with the Qt Quick cutover, so there is no \
                 executable to measure. The archived A/B results are the record of that \
                 comparison. To re-run it, check out the pre-cutover checkpoint and build there, \
                 or pass an explicit widgets executable."
            ),
        },
    }
}

/// The marker the app compiles in when `EXOSNAP_BUILD_BENCHMARK_HARNESS` is
/// on: the `--benchmark-scenario` option string itself, in UTF-16 as Qt
/// stores literals. A shipping Release build does not know this switch, does
/// not fail on it, starts as the ordinary application, persists into the
/// user's real configuration directory and never exits, which from the
/// outside looks like a hang rather than the wrong executable. Scanning the
/// binary catches that before a process is ever started.
pub const HARNESS_MARKER: &str = "--benchmark-scenario";

pub fn contains_harness_marker(bytes: &[u8]) -> bool {
    let marker: Vec<u8> = HARNESS_MARKER
        .encode_utf16()
        .flat_map(u16::to_le_bytes)
        .collect();
    if marker.is_empty() || bytes.len() < marker.len() {
        return false;
    }
    bytes.windows(marker.len()).any(|window| window == marker)
}

/// The reasons a completed run is not acceptable into a campaign, ported
/// verbatim from the run script's acceptance gate. Pure over the facts a run
/// produced, so it is tested without spawning anything.
#[allow(clippy::too_many_arguments)]
pub fn acceptance_problems(
    workload_kind: &str,
    has_report: bool,
    has_superposition_csv: bool,
    has_superposition_txt: bool,
    exit_code: i32,
    skip_topology_check: bool,
    window_placement: Option<&WindowPlacement>,
) -> Vec<String> {
    let mut missing = Vec::new();
    if !has_report {
        missing.push("ExoSnap benchmark JSON".to_string());
    }
    if workload_kind == "superposition" {
        if !has_superposition_csv {
            missing.push("Superposition CSV".to_string());
        }
        if !has_superposition_txt {
            missing.push("Superposition TXT".to_string());
        }
    }
    if exit_code != 0 {
        missing.push(format!("ExoSnap exit code {exit_code}"));
    }
    if !skip_topology_check {
        match window_placement {
            None => missing.push("frontend window display could not be confirmed".to_string()),
            Some(placement) if placement.screen_primary => {
                missing.push("frontend window was on the capture display".to_string());
            }
            Some(_) => {}
        }
    }
    missing
}

fn superposition_args(cli: &WorkloadCli, run_dir: &Path) -> Vec<String> {
    let mut args = vec![
        "-api".to_string(),
        cli.api.clone(),
        "-resolution".to_string(),
        cli.resolution.clone(),
        "-fullscreen".to_string(),
        cli.fullscreen.to_string(),
        "-quality".to_string(),
        cli.quality.clone(),
        "-textures".to_string(),
        cli.textures.clone(),
        "-dof".to_string(),
        cli.dof.to_string(),
        "-motion_blur".to_string(),
        cli.motion_blur.to_string(),
        "-sound".to_string(),
        cli.sound.to_string(),
        "-iterations".to_string(),
        "1".to_string(),
    ];
    match cli.mode.as_deref() {
        Some("scene") => {
            args.push("-mode".to_string());
            args.push("scene".to_string());
            args.push(cli.scene.unwrap_or_default().to_string());
            args.push("-mode_duration".to_string());
            args.push(cli.mode_duration_minutes.to_string());
        }
        Some("frame") => {
            args.push("-mode".to_string());
            args.push("frame".to_string());
            args.push(cli.frame.unwrap_or_default().to_string());
            args.push("-mode_duration".to_string());
            args.push(cli.mode_duration_minutes.to_string());
        }
        _ => {
            args.push("-mode".to_string());
            args.push("default".to_string());
        }
    }
    args.push("-log_csv".to_string());
    args.push(run_dir.join("superposition.csv").display().to_string());
    args.push("-log_csv_step".to_string());
    args.push("0".to_string());
    args.push("-log_txt".to_string());
    args.push(run_dir.join("superposition.txt").display().to_string());
    args
}

fn exosnap_args(
    settings: &ExoSnapSettings,
    definition: &ScenarioDefinition,
    run_dir: &Path,
) -> Vec<String> {
    let mut args = vec![
        "--auto-record".to_string(),
        "--enable-preview".to_string(),
        "--target".to_string(),
        settings.target.clone(),
        "--duration".to_string(),
        settings.duration_seconds.to_string(),
        "--frame-rate".to_string(),
        settings.frame_rate.to_string(),
        "--container".to_string(),
        settings.container.clone(),
        "--video-codec".to_string(),
        settings.video_codec.clone(),
        "--audio-codec".to_string(),
        settings.audio_codec.clone(),
        "--chroma".to_string(),
        settings.chroma.clone(),
        "--bit-depth".to_string(),
        settings.bit_depth.to_string(),
        "--hdr".to_string(),
        settings.hdr_mode.clone(),
        "--benchmark-scenario".to_string(),
        definition.id.clone(),
        "--benchmark-output".to_string(),
        run_dir.display().to_string(),
        "--benchmark-warmup".to_string(),
        settings.warmup_seconds.to_string(),
    ];
    if !settings.audio_rows.is_empty() {
        args.push("--audio-rows".to_string());
        args.push(settings.audio_rows.join(","));
    }
    if settings.target == "window"
        && let Some(title) = &settings.target_window_title
    {
        args.push("--target-window-title".to_string());
        args.push(title.clone());
    }
    if let Some(notes) = &definition.source_notes {
        args.push("--benchmark-notes".to_string());
        args.push(notes.clone());
    }
    args
}

/// Runs one measurement and returns its manifest. Returns an error both when
/// the run could not be started at all and when it completed but is not
/// acceptable into a campaign; in the latter case the manifest has already
/// been written to `run_dir/run.json` before the error is returned, so the
/// evidence survives the rejection.
pub fn run(scenario: &Scenario) -> anyhow::Result<RunManifest> {
    let exe = resolve_executable(
        scenario.frontend,
        scenario.widgets_exe.as_deref(),
        &scenario.quick_exe,
    )?;
    if !exe.is_file() {
        bail!(
            "{} executable not found at {}. Build it with EXOSNAP_BUILD_BENCHMARK_HARNESS=ON first.",
            scenario.frontend,
            exe.display()
        );
    }
    let bytes = std::fs::read(&exe)
        .with_context(|| format!("could not read {} to check for the harness", exe.display()))?;
    if !contains_harness_marker(&bytes) {
        bail!(
            "{} was built WITHOUT the benchmark harness. Reconfigure with \
             -DEXOSNAP_BUILD_BENCHMARK_HARNESS=ON and rebuild. A shipping build silently starts \
             as the normal application and writes to the real user configuration.",
            exe.display()
        );
    }
    drop(bytes);

    let topology_result = if scenario.skip_topology_check {
        eprintln!("Topology verification skipped. This run cannot be accepted into a campaign.");
        None
    } else {
        let result = topology::verify(&scenario.definition.topology)?;
        for problem in &result.problems {
            eprintln!("warning: {problem}");
        }
        if !result.ok {
            bail!(
                "Display topology does not match the scenario. Refusing to benchmark the wrong monitor."
            );
        }
        Some(result)
    };

    let run_id = format!("{}-run{:02}", scenario.frontend, scenario.run_index);
    let scenario_root = scenario.output_root.join(if scenario.calibration {
        format!("calibration/{}", scenario.definition.id)
    } else {
        scenario.definition.id.clone()
    });
    let run_dir = scenario_root.join(&run_id);
    std::fs::create_dir_all(&run_dir)
        .with_context(|| format!("could not create run directory {}", run_dir.display()))?;

    let mut workload_child: Option<Child> = None;
    let mut workload_args = Vec::new();
    if scenario.definition.workload.kind == "superposition" {
        if !scenario.superposition_cli.is_file() {
            bail!(
                "Superposition CLI not found at {}.",
                scenario.superposition_cli.display()
            );
        }
        let cli = scenario
            .definition
            .workload
            .cli
            .as_ref()
            .context("scenario declares a superposition workload without a cli block")?;
        workload_args = superposition_args(cli, &run_dir);
        let child = Command::new(&scenario.superposition_cli)
            .args(&workload_args)
            .spawn()
            .with_context(|| format!("could not start {}", scenario.superposition_cli.display()))?;
        workload_child = Some(child);
        std::thread::sleep(Duration::from_secs(
            scenario.definition.workload.warmup_seconds,
        ));
        if let Some(child) = &mut workload_child
            && let Some(status) = child.try_wait()?
        {
            bail!("Superposition exited during warm-up with code {status}.");
        }
    }

    let settings = &scenario.definition.exosnap;
    let exo_args = exosnap_args(settings, &scenario.definition, &run_dir);
    let stdout_path = run_dir.join("exosnap.stdout.log");
    let stderr_path = run_dir.join("exosnap.stderr.log");
    let mut exo_child = Command::new(&exe)
        .args(&exo_args)
        .env("EXOSNAP_OUTPUT_DIR", &run_dir)
        .stdin(Stdio::null())
        .stdout(Stdio::from(std::fs::File::create(&stdout_path)?))
        .stderr(Stdio::from(std::fs::File::create(&stderr_path)?))
        .spawn()
        .with_context(|| format!("could not start {}", exe.display()))?;

    let window_placement = resolve_window_placement(&mut exo_child, scenario.skip_topology_check);

    let exo_budget = Duration::from_secs(settings.warmup_seconds + settings.duration_seconds + 180);
    let (exo_exit_code, timed_out) = wait_bounded(&mut exo_child, exo_budget);
    if timed_out {
        eprintln!(
            "warning: ExoSnap did not exit within {}s. Killing it; this run cannot be accepted.",
            exo_budget.as_secs()
        );
        let _ = exo_child.kill();
        let _ = exo_child.wait();
    }

    if let Some(mut child) = workload_child {
        let remaining = Duration::from_secs(
            scenario
                .definition
                .workload
                .cli
                .as_ref()
                .map(|cli| cli.mode_duration_minutes)
                .unwrap_or_default() as u64
                * 60
                + 90,
        );
        let (_, workload_timed_out) = wait_bounded(&mut child, remaining);
        if workload_timed_out {
            eprintln!(
                "warning: Superposition would not exit; killing it. Its reports may be truncated."
            );
            let _ = child.kill();
            let _ = child.wait();
        }
    }

    let report_file = std::fs::read_dir(&run_dir)
        .into_iter()
        .flatten()
        .flatten()
        .find(|entry| {
            entry.path().extension().and_then(|e| e.to_str()) == Some("json")
                && entry.file_name() != "run.json"
        })
        .map(|entry| entry.file_name().to_string_lossy().into_owned());
    let recording = find_recording(&run_dir);
    let superposition_csv = run_dir.join("superposition.csv").is_file();
    let superposition_txt = run_dir.join("superposition.txt").is_file();

    let manifest = RunManifest {
        run_id,
        frontend: scenario.frontend.as_str().to_string(),
        scenario: scenario.definition.id.clone(),
        run_index: scenario.run_index,
        calibration: scenario.calibration,
        topology_verified: !scenario.skip_topology_check,
        executable: exe,
        exosnap_args: exo_args,
        exosnap_exit_code: exo_exit_code,
        workload_kind: scenario.definition.workload.kind.clone(),
        workload_args,
        exosnap_report: report_file.clone(),
        recording,
        superposition_csv: superposition_csv.then(|| "superposition.csv".to_string()),
        superposition_txt: superposition_txt.then(|| "superposition.txt".to_string()),
        attached_panels: topology_result
            .as_ref()
            .map(|r| r.attached_panels.clone())
            .unwrap_or_default(),
        frontend_window: window_placement.clone(),
    };

    let manifest_json = serde_json::to_string_pretty(&manifest)?;
    std::fs::write(run_dir.join("run.json"), manifest_json)?;
    std::fs::write(
        run_dir.join("scenario.json"),
        serde_json::to_string_pretty(&BTreeMap::from([("id", scenario.definition.id.clone())]))?,
    )
    .ok();

    let missing = acceptance_problems(
        &manifest.workload_kind,
        report_file.is_some(),
        superposition_csv,
        superposition_txt,
        exo_exit_code,
        scenario.skip_topology_check,
        window_placement.as_ref(),
    );
    if !missing.is_empty() {
        bail!("Run is NOT acceptable: {}", missing.join("; "));
    }
    Ok(manifest)
}

fn wait_bounded(child: &mut Child, budget: Duration) -> (i32, bool) {
    let deadline = Instant::now() + budget;
    loop {
        match child.try_wait() {
            Ok(Some(status)) => return (status.code().unwrap_or(-1), false),
            Ok(None) => {
                if Instant::now() >= deadline {
                    return (-1, true);
                }
                std::thread::sleep(Duration::from_millis(250));
            }
            Err(_) => return (-1, false),
        }
    }
}

fn find_recording(run_dir: &Path) -> Option<String> {
    for extension in ["mkv", "mp4", "webm"] {
        if let Ok(entries) = std::fs::read_dir(run_dir) {
            for entry in entries.flatten() {
                if entry.path().extension().and_then(|e| e.to_str()) == Some(extension) {
                    return Some(entry.file_name().to_string_lossy().into_owned());
                }
            }
        }
    }
    None
}

/// Where the frontend window actually landed, sampled once while the run is
/// live. Read-only: the window handle is resolved from our own child
/// process id and only its rectangle/monitor is queried, so this never
/// synthesizes input or takes focus.
#[cfg(windows)]
fn resolve_window_placement(
    child: &mut Child,
    skip_topology_check: bool,
) -> Option<WindowPlacement> {
    let placement = win32::wait_for_window(child.id(), Duration::from_secs(30));
    if let Some(placement) = &placement {
        eprintln!(
            "ExoSnap window is on {} (primary: {})",
            placement.screen_device_name, placement.screen_primary
        );
        if placement.screen_primary && !skip_topology_check {
            eprintln!("warning: the frontend window is on the CAPTURE display.");
        }
    } else if let Ok(None) = child.try_wait() {
        eprintln!(
            "warning: could not resolve the ExoSnap window; its display cannot be confirmed for this run."
        );
    }
    placement
}

#[cfg(not(windows))]
fn resolve_window_placement(
    _child: &mut Child,
    _skip_topology_check: bool,
) -> Option<WindowPlacement> {
    None
}

#[cfg(windows)]
mod win32 {
    use super::WindowPlacement;
    use std::time::{Duration, Instant};

    use windows::Win32::Foundation::{HWND, LPARAM};
    use windows::Win32::Graphics::Gdi::{
        GetMonitorInfoW, MONITOR_DEFAULTTONULL, MONITORINFOEXW, MonitorFromWindow,
    };
    use windows::Win32::UI::WindowsAndMessaging::{
        EnumWindows, GWL_STYLE, GetWindowLongPtrW, GetWindowThreadProcessId, IsWindowVisible,
        MONITORINFOF_PRIMARY, WS_VISIBLE,
    };
    use windows::core::BOOL;

    struct FindContext {
        pid: u32,
        found: Option<HWND>,
    }

    pub fn wait_for_window(pid: u32, budget: Duration) -> Option<WindowPlacement> {
        let deadline = Instant::now() + budget;
        loop {
            if let Some(hwnd) = find_main_window(pid) {
                return placement_for_window(hwnd);
            }
            if Instant::now() >= deadline {
                return None;
            }
            std::thread::sleep(Duration::from_millis(500));
        }
    }

    fn find_main_window(pid: u32) -> Option<HWND> {
        let mut ctx = FindContext { pid, found: None };
        // SAFETY: `enum_proc` only reads its LPARAM as a `*mut FindContext` that
        // this call itself provides, and never outlives it.
        unsafe {
            let _ = EnumWindows(
                Some(enum_proc),
                LPARAM(&mut ctx as *mut FindContext as isize),
            );
        }
        ctx.found
    }

    unsafe extern "system" fn enum_proc(hwnd: HWND, lparam: LPARAM) -> BOOL {
        // SAFETY: `lparam` is the address of a live `FindContext` for the
        // duration of the `EnumWindows` call that provided it.
        let ctx = unsafe { &mut *(lparam.0 as *mut FindContext) };
        let mut window_pid = 0u32;
        // SAFETY: `hwnd` is a window handle EnumWindows itself supplies.
        unsafe { GetWindowThreadProcessId(hwnd, Some(&mut window_pid)) };
        if window_pid == ctx.pid {
            let visible = unsafe { IsWindowVisible(hwnd) }.as_bool();
            let style = unsafe { GetWindowLongPtrW(hwnd, GWL_STYLE) };
            if visible && (style as u32 & WS_VISIBLE.0) != 0 {
                ctx.found = Some(hwnd);
                return BOOL(0);
            }
        }
        BOOL(1)
    }

    fn placement_for_window(hwnd: HWND) -> Option<WindowPlacement> {
        // SAFETY: `hwnd` came from `find_main_window`, and MONITOR_DEFAULTTONULL
        // returns a null handle rather than an invalid one when it fails.
        let monitor = unsafe { MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) };
        if monitor.is_invalid() {
            return None;
        }
        let mut info = MONITORINFOEXW::default();
        info.monitorInfo.cbSize = std::mem::size_of::<MONITORINFOEXW>() as u32;
        // SAFETY: `info.monitorInfo.cbSize` is set as the API requires.
        let ok = unsafe { GetMonitorInfoW(monitor, &mut info.monitorInfo) };
        if !ok.as_bool() {
            return None;
        }
        let device_name = String::from_utf16_lossy(&info.szDevice)
            .trim_end_matches('\0')
            .to_string();
        let bounds = info.monitorInfo.rcMonitor;
        Some(WindowPlacement {
            screen_device_name: device_name,
            screen_primary: info.monitorInfo.dwFlags & MONITORINFOF_PRIMARY != 0,
            screen_bounds: format!(
                "{{X={},Y={},Width={},Height={}}}",
                bounds.left,
                bounds.top,
                bounds.right - bounds.left,
                bounds.bottom - bounds.top
            ),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolving_quick_never_needs_a_widgets_executable() {
        let resolved =
            resolve_executable(Frontend::Quick, None, Path::new("C:/exosnap.exe")).unwrap();
        assert_eq!(resolved, Path::new("C:/exosnap.exe"));
    }

    #[test]
    fn resolving_widgets_without_an_explicit_executable_refuses_with_an_explanation() {
        let error = resolve_executable(Frontend::Widgets, None, Path::new("C:/exosnap.exe"))
            .expect_err("must refuse, not silently measure the quick binary");
        let message = error.to_string();
        assert!(message.contains("removed"), "{message}");
        assert!(message.contains("Qt Quick cutover"), "{message}");
    }

    #[test]
    fn resolving_widgets_with_an_explicit_executable_is_allowed() {
        let resolved = resolve_executable(
            Frontend::Widgets,
            Some(Path::new("C:/widgets.exe")),
            Path::new("C:/quick.exe"),
        )
        .unwrap();
        assert_eq!(resolved, Path::new("C:/widgets.exe"));
    }

    #[test]
    fn a_release_binary_without_the_harness_option_string_is_not_detected() {
        let bytes = "a shipping release build".as_bytes();
        assert!(!contains_harness_marker(bytes));
    }

    #[test]
    fn a_harness_enabled_binary_is_detected_by_its_utf16_option_string() {
        let mut bytes = vec![0xDEu8, 0xAD, 0xBE, 0xEF];
        bytes.extend(HARNESS_MARKER.encode_utf16().flat_map(u16::to_le_bytes));
        bytes.extend([0x00, 0x00, 0xCA, 0xFE]);
        assert!(contains_harness_marker(&bytes));
    }

    #[test]
    fn acceptance_requires_the_report_a_clean_exit_and_a_confirmed_ui_display() {
        let clean_window = WindowPlacement {
            screen_device_name: "\\\\.\\DISPLAY2".to_string(),
            screen_primary: false,
            screen_bounds: String::new(),
        };
        assert!(
            acceptance_problems("none", true, false, false, 0, false, Some(&clean_window))
                .is_empty()
        );

        let missing_report =
            acceptance_problems("none", false, false, false, 0, false, Some(&clean_window));
        assert!(missing_report.iter().any(|p| p.contains("benchmark JSON")));

        let bad_exit =
            acceptance_problems("none", true, false, false, 1, false, Some(&clean_window));
        assert!(bad_exit.iter().any(|p| p.contains("exit code 1")));

        let on_capture_display = WindowPlacement {
            screen_primary: true,
            ..clean_window.clone()
        };
        let on_capture = acceptance_problems(
            "none",
            true,
            false,
            false,
            0,
            false,
            Some(&on_capture_display),
        );
        assert!(on_capture.iter().any(|p| p.contains("capture display")));

        let no_window = acceptance_problems("none", true, false, false, 0, false, None);
        assert!(
            no_window
                .iter()
                .any(|p| p.contains("could not be confirmed"))
        );

        let skipped_topology = acceptance_problems("none", true, false, false, 0, true, None);
        assert!(skipped_topology.is_empty());
    }

    #[test]
    fn superposition_workloads_require_both_report_files() {
        let clean_window = WindowPlacement {
            screen_device_name: "\\\\.\\DISPLAY2".to_string(),
            screen_primary: false,
            screen_bounds: String::new(),
        };
        let missing_csv = acceptance_problems(
            "superposition",
            true,
            false,
            true,
            0,
            false,
            Some(&clean_window),
        );
        assert!(missing_csv.iter().any(|p| p.contains("Superposition CSV")));
        let missing_txt = acceptance_problems(
            "superposition",
            true,
            true,
            false,
            0,
            false,
            Some(&clean_window),
        );
        assert!(missing_txt.iter().any(|p| p.contains("Superposition TXT")));
        let complete = acceptance_problems(
            "superposition",
            true,
            true,
            true,
            0,
            false,
            Some(&clean_window),
        );
        assert!(complete.is_empty());
    }
}
