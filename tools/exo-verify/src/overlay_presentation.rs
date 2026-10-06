//! Isolated overlay A/B recordings with a process-bound external presentation trace.

use anyhow::{Context, Result, bail, ensure};
use serde_json::{Value, json};
use std::collections::BTreeMap;
use std::fs::File;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::{Duration, Instant};

const VARIANTS: [&str; 6] = [
    "off",
    "hud-minimal",
    "hud-health",
    "hud-full",
    "dock-only",
    "hud-full-dock",
];

#[derive(clap::Args, Debug)]
pub struct Args {
    #[arg(long)]
    pub product: PathBuf,
    #[arg(long)]
    pub mpv: PathBuf,
    #[arg(long)]
    pub media: PathBuf,
    #[arg(long)]
    pub presentmon: Option<PathBuf>,
    #[arg(long)]
    pub out: PathBuf,
    /// Continue only missing round/variant slots from a retained manifest into a fresh directory.
    #[arg(long)]
    pub continue_from: Option<PathBuf>,
    /// Accept this exact prior product hash after a reviewed equivalent rebuild.
    #[arg(long, requires = "continue_from")]
    pub previous_product_sha256: Option<String>,
    /// Samples per variant, with reversed order on alternating rounds.
    #[arg(long, default_value_t = 10)]
    pub samples: u32,
    #[arg(long, default_value_t = 30)]
    pub seconds: u32,
    #[arg(long, default_value_t = 5)]
    pub warmup: u32,
    /// Measure overlay signals only; makes no claim about Windows presentation.
    #[arg(long)]
    pub cadence_only: bool,
    /// Production content variants; defaults to OFF, Minimal, Health, Technical and dock comparisons.
    #[arg(long, value_delimiter = ',')]
    pub variants: Vec<String>,
    #[arg(long, default_value = "fullscreen", value_parser = ["fullscreen", "windowed", "exclusive"])]
    pub target_mode: String,
    /// MPV swapchain format for presentation-mode baseline probes.
    #[arg(long, default_value = "auto", value_parser = ["auto", "rgba8", "rgba16f"])]
    pub target_output_format: String,
    /// Follow media timestamps instead of presenting as fast as display sync permits.
    #[arg(long)]
    pub media_clock: bool,
    #[arg(long, default_value = "independent", value_parser = ["independent", "if", "composed"])]
    pub baseline: String,
    /// Retain other process swapchains for display-activity experiments.
    #[arg(long)]
    pub trace_desktop: bool,
    /// Record operator-known VRR, power and display facts; never guesses these.
    #[arg(long, default_value = "VRR and power state unverified")]
    pub environment_notes: String,
}

struct Process(Child);

#[derive(clap::Args, Debug)]
pub struct LifecycleArgs {
    #[arg(long)]
    pub product: PathBuf,
    #[arg(long)]
    pub out: PathBuf,
    #[arg(long, default_value_t = 30)]
    pub cycles: u32,
    #[arg(long)]
    pub same_process: bool,
}

#[cfg(not(windows))]
pub fn run_lifecycle(_: LifecycleArgs) -> Result<()> {
    bail!("DXGI lifecycle requires Windows")
}

#[cfg(windows)]
pub fn run_lifecycle(args: LifecycleArgs) -> Result<()> {
    use std::os::windows::io::AsRawHandle;
    use windows::Win32::Foundation::{CloseHandle, FILETIME, HANDLE};
    use windows::Win32::System::Diagnostics::ToolHelp::{
        CreateToolhelp32Snapshot, PROCESSENTRY32W, Process32FirstW, Process32NextW,
        TH32CS_SNAPPROCESS,
    };
    use windows::Win32::System::Threading::GetProcessTimes;

    fn inventory() -> Result<Value> {
        let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) }?;
        let mut entry = PROCESSENTRY32W {
            dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32,
            ..Default::default()
        };
        let mut rows = Vec::new();
        let mut more = unsafe { Process32FirstW(snapshot, &mut entry) }.is_ok();
        while more {
            let end = entry
                .szExeFile
                .iter()
                .position(|u| *u == 0)
                .unwrap_or(entry.szExeFile.len());
            rows.push(
                json!({"pid": entry.th32ProcessID, "parentPid": entry.th32ParentProcessID,
                "name": String::from_utf16_lossy(&entry.szExeFile[..end])}),
            );
            more = unsafe { Process32NextW(snapshot, &mut entry) }.is_ok();
        }
        unsafe { CloseHandle(snapshot) }?;
        Ok(json!(rows))
    }
    fn ticks(time: FILETIME) -> u64 {
        (u64::from(time.dwHighDateTime) << 32) | u64::from(time.dwLowDateTime)
    }
    ensure!(args.cycles > 0, "cycles must be positive");
    ensure!(
        !args.out.exists(),
        "lifecycle output must be a fresh directory"
    );
    std::fs::create_dir_all(&args.out)?;
    let out = args.out.canonicalize()?;
    let product = args.product.canonicalize()?;
    for cycle in 0..if args.same_process { 1 } else { args.cycles } {
        let dir = out.join(format!("cycle-{cycle:02}"));
        std::fs::create_dir_all(&dir)?;
        let before = inventory()?;
        write_json(&dir.join("processes-before.json"), &before)?;
        ensure!(
            !before.as_array().unwrap().iter().any(|p| p["name"]
                .as_str()
                .is_some_and(|n| n.eq_ignore_ascii_case("exosnap.exe"))),
            "another ExoSnap process already exists"
        );
        let mut process = spawn(
            Command::new(&product)
                .args([
                    "--auto-record",
                    "--target",
                    "monitor",
                    "--duration",
                    "1",
                    "--frame-rate",
                    "60",
                    "--video-codec",
                    "h264",
                    "--hdr",
                    "off",
                    "--benchmark-warmup",
                    "1",
                    "--benchmark-scenario",
                    "dxgi-lifecycle",
                ])
                .arg("--repeat-cycles")
                .arg(if args.same_process { args.cycles } else { 1 }.to_string())
                .arg("--benchmark-output")
                .arg(&dir)
                .env("EXOSNAP_CONFIG_DIR", dir.join("config"))
                .env("EXOSNAP_OUTPUT_DIR", dir.join("recordings"))
                .env_remove("EXOSNAP_OVERLAY_PRESENTATION_VARIANT"),
            &dir.join("recorder.log"),
        )?;
        let pid = process.0.id();
        let waited = crate::tools::wait(
            &mut process.0,
            Duration::from_secs(if args.same_process {
                u64::from(args.cycles) * 35 + 45
            } else {
                45
            }),
        );
        let observed_status = process.0.try_wait()?;
        let (mut creation, mut exit, mut kernel, mut user) = (
            FILETIME::default(),
            FILETIME::default(),
            FILETIME::default(),
            FILETIME::default(),
        );
        unsafe {
            GetProcessTimes(
                HANDLE(process.0.as_raw_handle()),
                &mut creation,
                &mut exit,
                &mut kernel,
                &mut user,
            )
        }?;
        let after = inventory()?;
        write_json(&dir.join("processes-after.json"), &after)?;
        let children: Vec<_> = after
            .as_array()
            .unwrap()
            .iter()
            .filter(|p| p["parentPid"] == pid)
            .collect();
        write_json(
            &dir.join("process-lifetime.json"),
            &json!({"pid":pid,"exitCode":observed_status.and_then(|status| status.code()),
                "waitError":waited.as_ref().err().map(|error| format!("{error:#}")),
            "creationFiletimeUtc100ns":ticks(creation),"exitFiletimeUtc100ns":ticks(exit),"survivingChildren":children}),
        )?;
        let status = waited?;
        println!(
            "DXGI lifecycle: cycle {cycle} PID {pid} exit {:?}",
            status.code()
        );
        ensure!(
            children.is_empty(),
            "ExoSnap PID {pid} left child processes alive"
        );
        ensure!(
            status.success(),
            "DXGI lifecycle PID {pid} failed; see {}",
            dir.join("recorder.log").display()
        );
    }
    Ok(())
}

impl Drop for Process {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

fn spawn(command: &mut Command, log: &Path) -> Result<Process> {
    spawn_with_stdout(command, log, None)
}

fn spawn_with_stdout(command: &mut Command, log: &Path, output: Option<&Path>) -> Result<Process> {
    let file = File::create(log)?;
    let stdout = match output {
        Some(path) => File::create(path)?,
        None => file.try_clone()?,
    };
    Ok(Process(command.stdout(stdout).stderr(file).spawn()?))
}

fn read_json(path: &Path) -> Result<Value> {
    serde_json::from_slice(&std::fs::read(path)?).with_context(|| path.display().to_string())
}

fn read_presentmon_csv(path: &Path) -> Result<String> {
    // PresentMon's file writer emits UTF-8. Diagnostic replacement decoding must
    // never alter process names, column contents or measurements in this CSV.
    let bytes = std::fs::read(path)
        .with_context(|| format!("read PresentMon CSV file {}", path.display()))?;
    decode_presentmon_csv(&bytes)
        .map(str::to_owned)
        .with_context(|| {
            format!(
                "decode PresentMon CSV file {} as strict UTF-8 (optional UTF-8 BOM)",
                path.display()
            )
        })
}

fn decode_presentmon_csv(bytes: &[u8]) -> Result<&str> {
    let csv = std::str::from_utf8(bytes).context("invalid UTF-8")?;
    ensure!(
        !csv.contains('\0'),
        "unexpected NUL; unsupported encoding or binary input"
    );
    Ok(csv)
}

fn decode_presentmon_stdout(bytes: &[u8], complete_only: bool) -> Result<String> {
    // Redirected disk stdout is BOM-declared UTF-16. File-mode CSV is UTF-8,
    // but its CRT writer denies readers while open. Stdout also flushes rows.
    let utf16 = bytes
        .strip_prefix(&[0xff, 0xfe])
        .map(|body| (body, true))
        .or_else(|| bytes.strip_prefix(&[0xfe, 0xff]).map(|body| (body, false)));
    let text = if let Some((body, little_endian)) = utf16 {
        ensure!(
            complete_only || body.len() % 2 == 0,
            "truncated UTF-16 stdout code unit"
        );
        let mut units: Vec<_> = body
            .chunks_exact(2)
            .map(|pair| {
                if little_endian {
                    u16::from_le_bytes([pair[0], pair[1]])
                } else {
                    u16::from_be_bytes([pair[0], pair[1]])
                }
            })
            .collect();
        if complete_only {
            let length = units
                .iter()
                .rposition(|unit| *unit == 10)
                .map_or(0, |last| last + 1);
            units.truncate(length);
        }
        String::from_utf16(&units).context("invalid UTF-16 stdout surrogate sequence")?
    } else {
        let bytes = if complete_only {
            &bytes[..bytes
                .iter()
                .rposition(|byte| *byte == b'\n')
                .map_or(0, |last| last + 1)]
        } else {
            bytes
        };
        decode_presentmon_csv(bytes)?
            .trim_start_matches('\u{feff}')
            .to_owned()
    };
    ensure!(
        !text.contains('\0'),
        "unexpected NUL in PresentMon stdout CSV"
    );
    Ok(text)
}

fn target_data_ready(bytes: &[u8], pid: u32, minimum_qpc: i64) -> Result<bool> {
    // A live file can end inside a record or UTF-8 character. Only complete
    // records are inspected here; the closed file is decoded in full later.
    let Some(last_newline) = bytes.iter().rposition(|byte| *byte == b'\n') else {
        return Ok(false);
    };
    let csv = decode_presentmon_csv(&bytes[..=last_newline])?;
    let (header, rows) = presentmon_records(csv)?;
    let process = column(&header, "ProcessID")?;
    let qpc = column(&header, "QPCTime")?;
    let swapchain = column(&header, "SwapChainAddress")?;
    Ok(rows.iter().any(|row| {
        row[process].parse::<u32>() == Ok(pid)
            && row[qpc]
                .parse::<i64>()
                .is_ok_and(|ticks| ticks >= minimum_qpc)
            && u64::from_str_radix(&row[swapchain][2..], 16).is_ok_and(|address| address != 0)
    }))
}

fn wait_for_target_data(
    capture: &mut Child,
    path: &Path,
    pid: u32,
    minimum_qpc: i64,
    timeout: Duration,
) -> Result<()> {
    let deadline = Instant::now() + timeout;
    loop {
        ensure!(
            capture.try_wait()?.is_none(),
            "PresentMon PID {} exited before target PID {pid} data readiness",
            capture.id()
        );
        match std::fs::read(path) {
            Ok(bytes) => {
                let text = decode_presentmon_stdout(&bytes, true).with_context(|| format!(
                    "decode live PresentMon stdout file {} as BOM-declared UTF-16LE/BE or strict UTF-8", path.display()))?;
                if target_data_ready(text.as_bytes(), pid, minimum_qpc).with_context(|| {
                    format!(
                        "validate live PresentMon CSV {} as strict UTF-8",
                        path.display()
                    )
                })? {
                    return Ok(());
                }
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => {
                return Err(error).context(format!("read live PresentMon CSV {}", path.display()));
            }
        }
        ensure!(
            Instant::now() < deadline,
            "PresentMon PID {} produced no complete target PID {pid} rows at/after QPC {minimum_qpc} within {} s in {}; process startup is not data readiness",
            capture.id(),
            timeout.as_secs(),
            path.display()
        );
        std::thread::sleep(Duration::from_millis(50));
    }
}

struct PresentMonCapture {
    process: Process,
    tool: PathBuf,
    session: String,
    directory: PathBuf,
    stop_command: Command,
    finalized: bool,
}

fn presentmon_stop_command(tool: &Path, session: &str) -> Command {
    let mut command = Command::new(tool);
    command.args(["--terminate_existing_session", "--session_name", session]);
    command
}

fn presentmon_command(
    tool: &Path,
    directory: &Path,
    process: &str,
    session: &str,
    seconds: u32,
) -> Command {
    let mut command = Command::new(tool);
    command.current_dir(directory);
    if !process.is_empty() {
        command.args(["--process_name", process]);
    }
    command.args([
        "--v1_metrics",
        "--qpc_time",
        "--no_console_stats",
        "--terminate_after_timed",
        "--timed",
        &seconds.to_string(),
        "--session_name",
        session,
        "--output_stdout",
    ]);
    command
}

impl PresentMonCapture {
    fn start(
        tool: &Path,
        directory: &Path,
        target: &Path,
        seconds: u32,
        trace_desktop: bool,
    ) -> Result<Self> {
        let session = crate::control::new_run_id("exo-overlay");
        let name = target
            .file_name()
            .and_then(|name| name.to_str())
            .context("target executable name must be Unicode")?;
        let mut command = presentmon_command(
            tool,
            directory,
            if trace_desktop { "" } else { name },
            &session,
            seconds,
        );
        write_json(
            &directory.join("presentmon-command.json"),
            &json!({
                "tool": tool, "workingDirectory": directory,
                "arguments": command.get_args().map(|arg| arg.to_string_lossy()).collect::<Vec<_>>()
            }),
        )?;
        let process = spawn_with_stdout(
            &mut command,
            &directory.join("presentmon.log"),
            Some(&directory.join("presentmon-stdout.bin")),
        )?;
        let stop_command = presentmon_stop_command(tool, &session);
        let capture = Self {
            process,
            tool: tool.into(),
            session,
            directory: directory.into(),
            stop_command,
            finalized: false,
        };
        write_json(
            &directory.join("presentmon-session.json"),
            &json!({
                "sessionName": capture.session, "capturePid": capture.process.0.id(), "targetProcessName": name
            }),
        )?;
        Ok(capture)
    }

    fn context(&self, stage: &str) -> String {
        let log = self.directory.join("presentmon.log");
        format!(
            "{stage}: PresentMon {} PID {} session {}; raw stdout CSV {}; command {}; stderr {} (BOM-aware UTF-16LE/BE or UTF-8 replacement decoding): {}",
            self.tool.display(),
            self.process.0.id(),
            self.session,
            self.directory.join("presentmon-stdout.bin").display(),
            self.directory.join("presentmon-command.json").display(),
            log.display(),
            crate::tools::read_diagnostic_log(&log, "PresentMon")
                .unwrap_or_else(|error| format!("{error:#}"))
        )
    }

    fn finish(&mut self) -> Result<()> {
        if self.finalized {
            return Ok(());
        }
        self.finalized = true;
        finish_presentmon(&mut self.process.0, || {
            let stop = crate::tools::run(
                &mut self.stop_command,
                Duration::from_secs(10),
            ).with_context(|| format!("stop PresentMon session {}", self.session))?;
            write_json(&self.directory.join("presentmon-stop.json"), &json!({
                "sessionName": self.session, "exitCode": stop.code(), "stdout": stop.stdout, "stderr": stop.stderr
            }))?;
            ensure!(stop.success(), "PresentMon session {} stop failed; stdout: {}; stderr: {}", self.session, stop.stdout, stop.stderr);
            Ok(())
        }, Duration::from_secs(20)).with_context(|| self.context("finalize capture"))
    }
}

impl Drop for PresentMonCapture {
    fn drop(&mut self) {
        if let Err(error) = self.finish() {
            let message = format!("{error:#}");
            let _ = write_json(
                &self.directory.join("presentmon-cleanup-error.json"),
                &json!({"error": message}),
            );
            eprintln!("PresentMon cleanup: {message}");
        }
    }
}

fn write_json(path: &Path, value: &Value) -> Result<()> {
    std::fs::write(path, serde_json::to_vec_pretty(value)?)
        .with_context(|| path.display().to_string())
}

fn file_identity(path: &Path) -> Result<Value> {
    use sha2::{Digest, Sha256};
    Ok(
        json!({"path": path.canonicalize()?, "sha256": hex::encode(Sha256::digest(std::fs::read(path)?))}),
    )
}

fn finish_presentmon(
    capture: &mut Child,
    request_stop: impl FnOnce() -> Result<()>,
    timeout: Duration,
) -> Result<()> {
    let stop_error = if capture.try_wait()?.is_none_or(|status| !status.success()) {
        request_stop().err()
    } else {
        None
    };
    let status = crate::tools::wait(capture, timeout).with_context(|| {
        format!(
            "PresentMon PID {} did not exit and flush CSV after session shutdown{}",
            capture.id(),
            stop_error
                .as_ref()
                .map(|error| format!("; stop request failed: {error:#}"))
                .unwrap_or_default()
        )
    })?;
    // A natural exit may race the stop request. The capture's successful exit
    // still establishes that its output thread closed the CSV.
    ensure!(
        status.success(),
        "PresentMon capture exited with {status}{}",
        stop_error
            .as_ref()
            .map(|error| format!("; owned session cleanup failed: {error:#}"))
            .unwrap_or_default()
    );
    Ok(())
}

#[cfg(windows)]
fn desktop_facts(
    target_pid: u32,
    recorder_pid: u32,
    focus: bool,
    fullscreen: bool,
) -> Result<Value> {
    use windows::Win32::Foundation::{POINT, RECT};
    use windows::Win32::Graphics::Gdi::{
        GetMonitorInfoW, MONITOR_DEFAULTTONEAREST, MONITORINFO, MONITORINFOEXW, MonitorFromWindow,
    };
    use windows::Win32::UI::WindowsAndMessaging::{
        GetForegroundWindow, GetWindowRect, GetWindowThreadProcessId, SetForegroundWindow,
        WindowFromPoint,
    };
    let target_windows = crate::win::process_windows(target_pid);
    ensure!(
        target_windows.len() == 1,
        "target must have one visible top-level window"
    );
    let target = target_windows[0];
    if focus {
        unsafe {
            let _ = SetForegroundWindow(target);
        }
    }
    let foreground = unsafe { GetForegroundWindow() };
    let mut foreground_pid = 0;
    unsafe {
        GetWindowThreadProcessId(foreground, Some(&mut foreground_pid));
    }
    ensure!(
        foreground == target,
        "target PID {target_pid} HWND {} did not acquire/retain foreground focus; foreground PID {foreground_pid} HWND {}; recorder PID {recorder_pid}; focus requested={focus}",
        target.0 as usize,
        foreground.0 as usize
    );
    let mut rect = RECT::default();
    unsafe {
        GetWindowRect(target, &mut rect)?;
    }
    let monitor = unsafe { MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST) };
    let mut info = MONITORINFOEXW::default();
    info.monitorInfo.cbSize = std::mem::size_of::<MONITORINFOEXW>() as u32;
    ensure!(
        unsafe { GetMonitorInfoW(monitor, &mut info as *mut _ as *mut MONITORINFO) }.as_bool(),
        "target monitor unavailable"
    );
    let bounds = info.monitorInfo.rcMonitor;
    if fullscreen {
        ensure!(
            rect.left == bounds.left
                && rect.top == bounds.top
                && rect.right == bounds.right
                && rect.bottom == bounds.bottom,
            "target does not cover its monitor"
        );
        for x in [bounds.left + 8, bounds.right - 8] {
            ensure!(
                unsafe {
                    WindowFromPoint(POINT {
                        x,
                        y: bounds.bottom - 2,
                    })
                } == target,
                "taskbar/another window covers the target at a bottom-edge probe"
            );
        }
    }
    let mut main = None;
    for window in crate::win::process_windows(recorder_pid) {
        let mut r = RECT::default();
        unsafe {
            GetWindowRect(window, &mut r)?;
        }
        let area = (r.right - r.left) as i64 * (r.bottom - r.top) as i64;
        if main.is_none_or(|(_, _, previous_area)| area > previous_area) {
            main = Some((window, r, area));
        }
    }
    let (main_window, main_rect, _) = main.context("recorder main window is not visible yet")?;
    let main_monitor = unsafe { MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST) };
    let mut main_info = MONITORINFOEXW::default();
    main_info.monitorInfo.cbSize = std::mem::size_of::<MONITORINFOEXW>() as u32;
    ensure!(
        unsafe { GetMonitorInfoW(main_monitor, &mut main_info as *mut _ as *mut MONITORINFO) }
            .as_bool(),
        "main monitor unavailable"
    );
    ensure!(
        main_monitor != monitor
            && (main_rect.right <= bounds.left
                || main_rect.left >= bounds.right
                || main_rect.bottom <= bounds.top
                || main_rect.top >= bounds.bottom),
        "ExoSnap main window overlaps the target display"
    );
    Ok(
        json!({"targetHwnd": target.0 as usize, "foregroundHwnd": target.0 as usize,
        "targetRect": [rect.left, rect.top, rect.right, rect.bottom], "targetMonitorRect": [bounds.left, bounds.top, bounds.right, bounds.bottom], "targetMonitor": String::from_utf16_lossy(&info.szDevice).trim_end_matches('\0'),
        "recorderMainRect": [main_rect.left, main_rect.top, main_rect.right, main_rect.bottom], "recorderOnSeparateMonitor": true,
        "recorderMainMonitor": String::from_utf16_lossy(&main_info.szDevice).trim_end_matches('\0'),
        "targetOwnsBottomEdgeProbePoints": fullscreen}),
    )
}

#[cfg(not(windows))]
fn desktop_facts(_: u32, _: u32, _: bool, _: bool) -> Result<Value> {
    bail!("Windows desktop required")
}

fn completed_slots(
    runs: &Value,
    samples: u32,
    variants: &[&str],
) -> Result<std::collections::BTreeSet<(u32, String)>> {
    let mut slots = std::collections::BTreeSet::new();
    for run in runs.as_array().context("continuation runs missing")? {
        let round = run["round"]
            .as_u64()
            .context("continuation round missing")?;
        let variant = run["variant"]
            .as_str()
            .context("continuation variant missing")?;
        ensure!(
            round <= u64::from(samples) && variants.contains(&variant),
            "continuation slot outside requested matrix"
        );
        ensure!(
            run["warmupRun"] == (round == 0),
            "continuation warm-up classification differs"
        );
        ensure!(
            slots.insert((round as u32, variant.to_owned())),
            "duplicate continuation slot"
        );
    }
    Ok(slots)
}

fn validate_continuation_contract(
    prior: &Value,
    current: &Value,
    accepted_product: Option<&str>,
) -> Result<()> {
    ensure!(
        ["incomplete", "complete"].contains(&prior["status"].as_str().unwrap_or("")),
        "continuation manifest must describe a stopped run"
    );
    for key in [
        "schema",
        "cadenceOnly",
        "samplesPerVariant",
        "seconds",
        "warmup",
        "variants",
        "baselineRequirement",
        "targetMode",
        "traceDesktop",
        "targetArguments",
    ] {
        ensure!(
            prior.get(key).is_some() && prior[key] == current[key],
            "continuation configuration differs: {key}"
        );
    }
    for key in ["target", "media", "presentmon"] {
        if key == "presentmon" && current["cadenceOnly"] == true {
            ensure!(
                prior[key] == current[key],
                "continuation PresentMon selection differs"
            );
            continue;
        }
        ensure!(
            prior[key]["sha256"].is_string() && prior[key]["sha256"] == current[key]["sha256"],
            "continuation tool/workload hash differs: {key}"
        );
    }
    let prior_product = prior["product"]["sha256"]
        .as_str()
        .context("prior product hash missing")?;
    ensure!(
        prior["product"]["sha256"] == current["product"]["sha256"]
            || accepted_product == Some(prior_product),
        "continuation product hash differs; a reviewed equivalent rebuild requires --previous-product-sha256 {prior_product}"
    );
    Ok(())
}

fn persisted_summary_matches(computed: &Value, retained: &Value) -> Result<bool> {
    // Derived f64 values must be compared through the same JSON storage roundtrip.
    // Comparing an in-memory result directly with a parsed manifest can differ by one ULP.
    let stored: Value = serde_json::from_slice(&serde_json::to_vec(computed)?)?;
    Ok(computed == retained || stored == *retained)
}

fn import_continuation(
    path: &Path,
    manifest: &mut Value,
    accepted_product: Option<&str>,
) -> Result<()> {
    let path = path.canonicalize()?;
    let prior = read_json(&path)?;
    validate_continuation_contract(&prior, manifest, accepted_product)?;
    let variants = manifest["variants"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v.as_str().unwrap())
        .collect::<Vec<_>>();
    completed_slots(
        &prior["runs"],
        manifest["samplesPerVariant"].as_u64().unwrap() as u32,
        &variants,
    )?;
    for mut run in prior["runs"]
        .as_array()
        .context("continuation runs missing")?
        .iter()
        .cloned()
    {
        let variant = run["variant"].as_str().unwrap();
        let dir = Path::new(
            run["directory"]
                .as_str()
                .context("continuation sample directory missing")?,
        );
        let overlay = read_json(&dir.join("overlay-measurement.json"))?;
        ensure!(
            overlay == run["overlay"],
            "continuation overlay evidence changed: {}",
            dir.display()
        );
        validate_overlay(&overlay, variant)?;
        ensure!(
            overlay["endPipeline"]["health"] == "Good",
            "continuation sample recording health is not Good"
        );
        let benchmark = read_json(&dir.join(format!("quick-overlay-{variant}-run01.json")))?;
        for file in ["desktop-start.json", "desktop-end.json"] {
            let desktop = read_json(&dir.join(file))?;
            ensure!(
                desktop["targetHwnd"].is_u64()
                    && desktop["targetHwnd"] == desktop["foregroundHwnd"],
                "continuation focus evidence invalid: {file}"
            );
            validate_display_bindings(&benchmark, &desktop, &overlay)?;
        }
        ensure!(
            benchmark["effective_recording_config"]["available"] == true
                && benchmark["effective_recording_config"]["fingerprint"]
                    == prior["effectiveConfigFingerprint"],
            "continuation recording configuration differs"
        );
        if manifest["cadenceOnly"] != true {
            let pid = u32::try_from(
                run["targetPid"]
                    .as_u64()
                    .context("continuation target PID missing")?,
            )?;
            let start = overlay["startQpc"]
                .as_i64()
                .context("continuation start QPC missing")?;
            let end = overlay["endQpc"]
                .as_i64()
                .context("continuation end QPC missing")?;
            let csv = read_presentmon_csv(&dir.join("presentmon.csv"))?;
            trace_coverage(&csv, pid, start, end)?;
            let summary = summarize(
                &csv,
                pid,
                start,
                end,
                overlay["qpcFrequency"]
                    .as_i64()
                    .context("continuation QPC frequency missing")?,
            )?;
            validate_presentation_sample(&summary)?;
            ensure!(
                persisted_summary_matches(&summary, &run["presentation"])?,
                "continuation presentation evidence changed"
            );
            if variant == "off" {
                ensure!(
                    baseline_matches(&summary, manifest["baselineRequirement"].as_str().unwrap()),
                    "continuation OFF baseline is unsuitable"
                );
            }
        }
        if run.get("product").is_none() {
            run["product"] = prior["product"].clone();
        }
        manifest["runs"].as_array_mut().unwrap().push(run);
    }
    manifest["effectiveConfigFingerprint"] = prior["effectiveConfigFingerprint"].clone();
    manifest["continuedFrom"] = file_identity(&path)?;
    manifest["previousProduct"] = prior["product"].clone();
    manifest["interruptedAttempts"] = prior
        .get("interruptedAttempts")
        .cloned()
        .unwrap_or_else(|| json!([]));
    if prior["status"] == "incomplete" {
        manifest["interruptedAttempts"]
            .as_array_mut()
            .unwrap()
            .push(json!({"manifest": file_identity(&path)?, "error": prior["error"]}));
    }
    Ok(())
}

pub fn run(args: Args) -> Result<()> {
    let variants: Vec<String> = if args.variants.is_empty() {
        VARIANTS.iter().map(|v| (*v).into()).collect()
    } else {
        args.variants.clone()
    };
    ensure!(
        variants.iter().all(|v| valid_variant(v)),
        "unsupported production variant"
    );
    ensure!(
        args.cadence_only || variants.iter().any(|v| v == "off"),
        "a presentation comparison requires its own recording+OFF baseline"
    );
    ensure!(
        variants
            .iter()
            .collect::<std::collections::BTreeSet<_>>()
            .len()
            == variants.len(),
        "duplicate variants"
    );
    let fullscreen = args.target_mode != "windowed";
    ensure!(
        args.samples > 0 && args.seconds >= 5 && args.warmup >= 2,
        "use positive samples, seconds >= 5 and warmup >= 2"
    );
    ensure!(
        !args.out.exists(),
        "output directory already exists; use a fresh directory"
    );
    let product = args.product.canonicalize()?;
    let mpv = args.mpv.canonicalize()?;
    let media = args.media.canonicalize()?;
    let presentmon_tool = args
        .presentmon
        .as_ref()
        .map(|path| path.canonicalize())
        .transpose()?;
    ensure!(
        args.cadence_only || args.presentmon.is_some(),
        "--presentmon is required unless --cadence-only is selected"
    );
    std::fs::create_dir_all(&args.out)?;
    let out = args.out.canonicalize()?;
    let mut manifest = json!({
        "schema": 1, "cadenceOnly": args.cadence_only, "samplesPerVariant": args.samples,
        "seconds": args.seconds, "warmup": args.warmup, "environmentNotes": args.environment_notes,
        "product": file_identity(&product)?, "target": file_identity(&mpv)?, "media": file_identity(&media)?,
        "presentmon": args.presentmon.as_ref().map(|p| file_identity(p)).transpose()?,
        "targetArguments": ["--no-config", "--vo=gpu", "--gpu-api=d3d11", "--gpu-context=d3d11", "--d3d11-flip=yes", "--d3d11-sync-interval=1", "--d3d11-exclusive-fs=no", "--fullscreen=yes", "--screen=0", "--audio=no", "--untimed=yes", "--loop-file=inf", "--osd-level=0"],
        "runs": [], "status": "running"
    });
    manifest["variants"] = json!(variants);
    manifest["baselineRequirement"] = json!(args.baseline);
    manifest["targetMode"] = json!(args.target_mode);
    manifest["traceDesktop"] = json!(args.trace_desktop);
    let target_args = manifest["targetArguments"].as_array_mut().unwrap();
    if args.target_mode == "windowed" {
        target_args.retain(|v| v != "--fullscreen=yes");
        target_args.extend([json!("--fullscreen=no"), json!("--geometry=1280x720+40+40")]);
    } else if args.target_mode == "exclusive" {
        for arg in target_args.iter_mut() {
            if arg == "--d3d11-exclusive-fs=no" {
                *arg = json!("--d3d11-exclusive-fs=yes");
            }
        }
    }
    if args.target_output_format != "auto" {
        target_args.push(json!(format!(
            "--d3d11-output-format={}",
            args.target_output_format
        )));
    }
    if args.media_clock {
        for arg in target_args.iter_mut() {
            if arg == "--untimed=yes" {
                *arg = json!("--untimed=no");
            }
            if arg == "--d3d11-sync-interval=1" {
                *arg = json!("--d3d11-sync-interval=0");
            }
        }
        target_args.push(json!("--video-sync=audio"));
    }
    if let Some(path) = &args.continue_from {
        import_continuation(path, &mut manifest, args.previous_product_sha256.as_deref())?;
    }
    let completed = completed_slots(
        &manifest["runs"],
        args.samples,
        &variants.iter().map(String::as_str).collect::<Vec<_>>(),
    )?;
    write_json(&out.join("manifest.json"), &manifest)?;
    let measured = (|| -> Result<()> {
        // A complete warm-up round is retained, but excluded from measured results.
        for round in 0..=args.samples {
            let order: Vec<_> = if round % 2 == 0 {
                variants.iter().map(String::as_str).collect()
            } else {
                variants.iter().rev().map(String::as_str).collect()
            };
            for variant in order {
                if completed.contains(&(round, variant.to_owned())) {
                    continue;
                }
                let dir = out.join(format!("round-{round:02}-{variant}"));
                std::fs::create_dir(&dir)?;
                println!(
                    "overlay presentation: round {round}/{} {variant}",
                    args.samples
                );
                let mut presentmon = if args.cadence_only {
                    None
                } else {
                    let capture = PresentMonCapture::start(
                        presentmon_tool.as_ref().unwrap(),
                        &dir,
                        &mpv,
                        args.seconds + args.warmup + 90,
                        args.trace_desktop,
                    )?;
                    Some(capture)
                };
                // Subscribe before the target creates its graphics resources.
                // Final attribution still uses its actual PID, swapchain and QPC.
                if let Some(pm) = presentmon.as_mut() {
                    std::thread::sleep(Duration::from_secs(1));
                    ensure!(
                        pm.process.0.try_wait()?.is_none(),
                        "{}",
                        pm.context("trace startup failed")
                    );
                }
                let mut target = spawn(
                    Command::new(&mpv)
                        .args(
                            manifest["targetArguments"]
                                .as_array()
                                .unwrap()
                                .iter()
                                .map(|v| v.as_str().unwrap()),
                        )
                        .args([
                            "--title=ExoOverlayPresentationTarget",
                            "--input-default-bindings=no",
                            "--input-cursor=no",
                            "--cursor-autohide=always",
                        ])
                        .arg(&media),
                    &dir.join("target.log"),
                )?;
                std::thread::sleep(Duration::from_secs(2));
                ensure!(
                    target.0.try_wait()?.is_none(),
                    "target exited before recording; see {}",
                    dir.join("target.log").display()
                );
                let target_pid = target.0.id();
                if let Some(pm) = presentmon.as_mut() {
                    write_json(
                        &dir.join("presentmon-session.json"),
                        &json!({"sessionName": pm.session, "capturePid": pm.process.0.id(), "targetPid": target_pid}),
                    )?;
                    wait_for_target_data(
                        &mut pm.process.0,
                        &dir.join("presentmon-stdout.bin"),
                        target_pid,
                        0,
                        Duration::from_secs(15),
                    )
                    .with_context(|| pm.context("target data readiness failed"))?;
                    write_json(
                        &dir.join("presentmon-ready.json"),
                        &json!({"targetPid": target_pid, "completeTargetRowsObserved": true}),
                    )?;
                }
                let mut recorder = spawn(
                    Command::new(&product)
                        .args([
                            "--auto-record",
                            "--target",
                            "monitor",
                            "--video-codec",
                            "h264",
                            "--frame-rate",
                            "60",
                            "--hdr",
                            "off",
                            "--duration",
                            &args.seconds.to_string(),
                            "--benchmark-warmup",
                            &args.warmup.to_string(),
                            "--benchmark-scenario",
                            &format!("overlay-{variant}"),
                        ])
                        .arg("--benchmark-output")
                        .arg(&dir)
                        .arg("--benchmark-notes")
                        .arg(format!(
                            "MPV D3D11 mode={}, media_clock={}, requested arguments={}; {}",
                            args.target_mode,
                            args.media_clock,
                            manifest["targetArguments"],
                            args.environment_notes
                        ))
                        .env("EXOSNAP_CONFIG_DIR", dir.join("config"))
                        .env("EXOSNAP_OUTPUT_DIR", dir.join("recordings"))
                        .env("EXOSNAP_OVERLAY_PRESENTATION_VARIANT", variant)
                        .env("QSG_INFO", "1"),
                    &dir.join("recorder.log"),
                )?;
                let ready_deadline =
                    Instant::now() + Duration::from_secs((args.warmup + 45).into());
                let mut focused = None;
                while !dir.join("overlay-measurement-start.json").is_file() {
                    if let Some(pm) = presentmon.as_mut() {
                        ensure!(
                            pm.process.0.try_wait()?.is_none(),
                            "{}",
                            pm.context("capture exited during recorder startup")
                        );
                    }
                    ensure!(
                        Instant::now() < ready_deadline && recorder.0.try_wait()?.is_none(),
                        "recorder did not open the measurement window; see recorder.log"
                    );
                    if focused.is_none() {
                        focused = desktop_facts(target_pid, recorder.0.id(), true, fullscreen).ok();
                    }
                    std::thread::sleep(Duration::from_millis(50));
                }
                // Delayed recorder activation can supersede an earlier warm-up
                // focus request. Establish focus again at the measurement boundary.
                let start_desktop = desktop_facts(target_pid, recorder.0.id(), true, fullscreen)
                    .context("measurement start desktop validation")?;
                write_json(&dir.join("desktop-start.json"), &start_desktop)?;
                // Observe before the recorder hides its overlays at stop.
                let monitor_deadline =
                    Instant::now() + Duration::from_secs(args.seconds.saturating_sub(1).into());
                let mut end_desktop = start_desktop.clone();
                while Instant::now() < monitor_deadline {
                    if let Some(pm) = presentmon.as_mut() {
                        ensure!(
                            pm.process.0.try_wait()?.is_none(),
                            "{}",
                            pm.context("capture exited during measurement")
                        );
                    }
                    end_desktop = desktop_facts(target_pid, recorder.0.id(), false, fullscreen)?;
                    std::thread::sleep(Duration::from_millis(250));
                }
                write_json(&dir.join("desktop-end.json"), &end_desktop)?;
                let status = crate::tools::wait(&mut recorder.0, Duration::from_secs(60))?;
                ensure!(
                    status.success(),
                    "recording failed: {}",
                    dir.join("recorder.log").display()
                );
                let overlay = read_json(&dir.join("overlay-measurement.json"))?;
                validate_overlay(&overlay, variant)?;
                let benchmark =
                    read_json(&dir.join(format!("quick-overlay-{variant}-run01.json")))?;
                validate_display_bindings(&benchmark, &start_desktop, &overlay)?;
                validate_display_bindings(&benchmark, &end_desktop, &overlay)?;
                ensure!(
                    benchmark["effective_recording_config"]["available"] == true,
                    "effective recording configuration unavailable"
                );
                let fingerprint = benchmark["effective_recording_config"]["fingerprint"]
                    .as_str()
                    .context("effective configuration fingerprint missing")?;
                if manifest["effectiveConfigFingerprint"].is_null() {
                    manifest["effectiveConfigFingerprint"] = json!(fingerprint);
                } else {
                    ensure!(
                        manifest["effectiveConfigFingerprint"] == fingerprint,
                        "effective recording configuration changed between samples"
                    );
                }
                let presentation = if let Some(pm) = presentmon.as_mut() {
                    ensure!(
                        pm.process.0.try_wait()?.is_none(),
                        "{}",
                        pm.context("capture exited before measurement finished")
                    );
                    let start = overlay["startQpc"].as_i64().context("start QPC missing")?;
                    let end = overlay["endQpc"].as_i64().context("end QPC missing")?;
                    let csv_path = dir.join("presentmon.csv");
                    let stdout_path = dir.join("presentmon-stdout.bin");
                    // Completed display metrics and file buffers can lag presents.
                    // Keep the target alive until the CSV passes the end boundary.
                    wait_for_target_data(
                        &mut pm.process.0,
                        &stdout_path,
                        target_pid,
                        end.checked_add(1).context("end QPC overflow")?,
                        Duration::from_secs(10),
                    )
                    .with_context(|| pm.context("measurement end data readiness failed"))?;
                    pm.finish()?;
                    let bytes = std::fs::read(&stdout_path)
                        .with_context(|| pm.context("read completed stdout failed"))?;
                    let decoded = decode_presentmon_stdout(&bytes, false).with_context(|| pm.context(
                        "decode completed stdout as BOM-declared UTF-16LE/BE or strict UTF-8 failed"))?;
                    std::fs::write(&csv_path, decoded).with_context(|| {
                        format!("write normalized UTF-8 CSV {}", csv_path.display())
                    })?;
                    let csv = read_presentmon_csv(&csv_path)
                        .with_context(|| pm.context("read completed capture failed"))?;
                    let coverage = trace_coverage(&csv, target_pid, start, end)
                        .with_context(|| pm.context("measurement coverage failed"))?;
                    write_json(&dir.join("presentmon-coverage.json"), &coverage)?;
                    let summary = summarize(
                        &csv,
                        target_pid,
                        start,
                        end,
                        overlay["qpcFrequency"]
                            .as_i64()
                            .context("QPC frequency missing")?,
                    )
                    .with_context(|| format!("parse PresentMon CSV file {}", csv_path.display()))?;
                    validate_presentation_sample(&summary)?;
                    write_json(&dir.join("presentation.json"), &summary)?;
                    Some(summary)
                } else {
                    None
                };
                let product_identity = manifest["product"].clone();
                manifest["runs"].as_array_mut().unwrap().push(json!({"round": round, "warmupRun": round == 0, "variant": variant, "directory": dir, "product": product_identity, "targetPid": target_pid, "recorderPid": recorder.0.id(), "overlay": overlay, "presentation": presentation}));
                write_json(&out.join("manifest.json"), &manifest)?;
                if variant == "off" && !args.cadence_only {
                    let presentation =
                        &manifest["runs"].as_array().unwrap().last().unwrap()["presentation"];
                    ensure!(
                        baseline_matches(presentation, &args.baseline),
                        "OFF baseline did not establish >= 90% requested baseline {} on one swapchain; evidence retained",
                        args.baseline
                    );
                }
            }
        }
        Ok(())
    })();
    manifest["status"] = json!(if measured.is_ok() {
        "complete"
    } else {
        "incomplete"
    });
    manifest["error"] = measured
        .as_ref()
        .err()
        .map(|e| json!(format!("{e:#}")))
        .unwrap_or(Value::Null);
    write_json(&out.join("manifest.json"), &manifest)?;
    measured
}

fn validate_overlay(overlay: &Value, variant: &str) -> Result<()> {
    for key in ["startPipeline", "endPipeline"] {
        ensure!(
            overlay[key]["valid"] == true && overlay[key]["lifecycle"] == "recording",
            "{variant}: recording was not active at {key}"
        );
    }
    let hud = variant.starts_with("hud-");
    let dock = variant == "dock-only" || variant.ends_with("-dock");
    for key in ["startWindows", "endWindows"] {
        let windows = overlay[key]
            .as_array()
            .context("overlay snapshots missing")?;
        for (index, expected) in [hud, dock].into_iter().enumerate() {
            let window = windows.get(index).context("overlay window missing")?;
            ensure!(
                window["visible"].as_bool() == Some(expected),
                "{variant}: {key} visibility mismatch: {window}"
            );
            if expected {
                ensure!(
                    window["exposed"] == true
                        && window["nativeValid"] == true
                        && window["nativeVisible"] == true
                        && window["captureExcluded"] == true,
                    "{variant}: visible/excluded native window not proven: {window}"
                );
                ensure!(
                    window["transparentForInput"] == json!(index == 0)
                        && window["layered"] == json!(index == 0),
                    "overlay input/composition styles differ from baseline"
                );
            }
        }
    }
    if hud {
        let content_variant = variant.strip_suffix("-dock").unwrap_or(variant);
        for key in ["startWindows", "endWindows"] {
            ensure!(
                overlay[key][0]["overlayState"] == 1,
                "HUD was not in Recording state at {key}"
            );
            ensure!(
                overlay[key][0]["diagnosticsActive"]
                    == json!(["hud-full", "hud-health"].contains(&content_variant)),
                "wrong diagnostics configuration"
            );
            if ["hud-full", "hud-health"].contains(&content_variant) {
                for (property, expected) in [
                    ("showFps", content_variant == "hud-full"),
                    ("showDrop", true),
                    ("showDrift", content_variant == "hud-full"),
                    ("showDiagnosticsSize", content_variant == "hud-full"),
                    ("showMutedSources", true),
                    ("showHealth", true),
                ] {
                    ensure!(
                        overlay[key][0][property] == expected,
                        "{variant}: wrong content flag {property} at {key}"
                    );
                }
            }
        }
    }
    Ok(())
}

fn validate_display_bindings(report: &Value, desktop: &Value, overlay: &Value) -> Result<()> {
    ensure!(
        desktop["targetMonitor"] == "\\\\.\\DISPLAY1"
            && desktop["recorderMainMonitor"] == "\\\\.\\DISPLAY2",
        "target/main windows must occupy DISPLAY1/DISPLAY2 respectively"
    );
    ensure!(
        report["recording_config"]["target_kind"] == "monitor"
            && report["recording_config"]["target_description"] == desktop["targetMonitor"],
        "recorded monitor differs from target monitor"
    );
    let bounds = desktop
        .get("targetMonitorRect")
        .unwrap_or(&desktop["targetRect"])
        .as_array()
        .context("target bounds missing")?;
    ensure!(bounds.len() == 4, "invalid target bounds");
    let coordinate = |value: &Value| value.as_i64().context("window coordinate missing");
    for key in ["startWindows", "endWindows"] {
        for window in overlay[key].as_array().context("overlay windows missing")? {
            if window["visible"] != true {
                continue;
            }
            let x = coordinate(&window["x"])?;
            let y = coordinate(&window["y"])?;
            let width = coordinate(&window["width"])?;
            let height = coordinate(&window["height"])?;
            ensure!(
                width > 0
                    && height > 0
                    && x >= coordinate(&bounds[0])?
                    && y >= coordinate(&bounds[1])?
                    && x + width <= coordinate(&bounds[2])?
                    && y + height <= coordinate(&bounds[3])?,
                "visible overlay does not lie wholly on the target monitor: {window}"
            );
        }
    }
    Ok(())
}

fn validate_presentation_sample(summary: &Value) -> Result<()> {
    ensure!(
        summary["swapchains"]
            .as_object()
            .is_some_and(|chains| chains.len() == 1),
        "every presentation sample requires exactly one target swapchain"
    );
    Ok(())
}

#[cfg(test)]
fn suitable_baseline(summary: &Value) -> bool {
    baseline_matches(summary, "independent")
}

fn valid_variant(variant: &str) -> bool {
    VARIANTS.contains(&variant)
}

fn baseline_matches(summary: &Value, requirement: &str) -> bool {
    let Some(chains) = summary["swapchains"].as_object() else {
        return false;
    };
    if chains.len() != 1 {
        return false;
    }
    let chain = chains.values().next().unwrap();
    let modes = &chain["modes"];
    let independent = match requirement {
        "if" => modes["Hardware: Independent Flip"].as_u64().unwrap_or(0),
        "composed" => modes["Composed: Flip"].as_u64().unwrap_or(0),
        _ => {
            modes["Hardware: Independent Flip"].as_u64().unwrap_or(0)
                + modes["Hardware Composed: Independent Flip"]
                    .as_u64()
                    .unwrap_or(0)
        }
    };
    independent as f64 / chain["presentCount"].as_u64().unwrap_or(1) as f64 >= 0.9
}

#[derive(Default)]
struct Chain {
    modes: BTreeMap<String, u64>,
    tearing: BTreeMap<String, u64>,
    sync: BTreeMap<String, u64>,
    transitions: Vec<Value>,
    last_mode: Option<String>,
    last_qpc: Option<i64>,
    intervals: Vec<f64>,
    displayed: Vec<f64>,
    latency: Vec<f64>,
    render_latency: Vec<f64>,
    gpu: Vec<f64>,
    dropped: u64,
    count: u64,
}

fn stats(mut values: Vec<f64>) -> Value {
    if values.is_empty() {
        return Value::Null;
    }
    values.sort_by(f64::total_cmp);
    let percentile = |p: f64| values[((values.len() as f64 * p).ceil() as usize).saturating_sub(1)];
    json!({"samples": values.len(), "p50": percentile(0.5), "p95": percentile(0.95), "p99": percentile(0.99), "max": values.last()})
}

fn column(header: &[String], name: &str) -> Result<usize> {
    header
        .iter()
        .position(|field| field == name)
        .with_context(|| format!("PresentMon v1 column {name} missing"))
}

fn presentmon_records(csv: &str) -> Result<(Vec<String>, Vec<Vec<String>>)> {
    let mut lines = csv.lines();
    let parse = |line: &str| {
        crate::scenarios::present::split_csv(line).map_err(|error| anyhow::anyhow!("{error:?}"))
    };
    let header = parse(
        lines
            .next()
            .context("CSV header missing")?
            .trim_start_matches('\u{feff}'),
    )?;
    ensure!(
        header
            .iter()
            .enumerate()
            .all(|(i, name)| !header[..i].contains(name)),
        "duplicate PresentMon CSV column"
    );
    for name in [
        "ProcessID",
        "SwapChainAddress",
        "QPCTime",
        "PresentMode",
        "Dropped",
        "SyncInterval",
        "AllowsTearing",
    ] {
        column(&header, name)?;
    }
    let mut rows = Vec::new();
    for (index, line) in lines
        .enumerate()
        .filter(|(_, line)| !line.trim().is_empty())
    {
        let row = parse(line)?;
        ensure!(
            row.len() == header.len(),
            "PresentMon CSV row length mismatch at line {}",
            index + 2
        );
        let validate = || -> Result<()> {
            let field = |name| -> Result<&str> { Ok(&row[column(&header, name)?]) };
            ensure!(field("ProcessID")?.parse::<u32>()? > 0, "invalid ProcessID");
            ensure!(field("QPCTime")?.parse::<i64>()? > 0, "invalid QPCTime");
            let address = field("SwapChainAddress")?
                .strip_prefix("0x")
                .context("invalid SwapChainAddress")?;
            u64::from_str_radix(address, 16).context("invalid SwapChainAddress")?;
            ensure!(!field("PresentMode")?.is_empty(), "empty PresentMode");
            field("SyncInterval")?
                .parse::<i32>()
                .context("invalid SyncInterval")?;
            for name in ["Dropped", "AllowsTearing"] {
                ensure!(matches!(field(name)?, "0" | "1"), "invalid {name}");
            }
            for name in ["msUntilDisplayed", "msUntilRenderComplete", "msGPUActive"] {
                if let Some(i) = header.iter().position(|field| field == name) {
                    let text = &row[i];
                    if text != "NA" && !text.is_empty() {
                        let value = text
                            .parse::<f64>()
                            .with_context(|| format!("invalid {name}"))?;
                        ensure!(
                            value.is_finite() && (name != "msGPUActive" || value >= 0.0),
                            "invalid {name}"
                        );
                    }
                }
            }
            Ok(())
        };
        validate().with_context(|| format!("PresentMon CSV line {}", index + 2))?;
        rows.push(row);
    }
    Ok((header, rows))
}

fn trace_coverage(csv: &str, pid: u32, start: i64, end: i64) -> Result<Value> {
    ensure!(start > 0 && end > start, "invalid QPC measurement window");
    let (header, rows) = presentmon_records(csv)?;
    let process = column(&header, "ProcessID")?;
    let qpc = column(&header, "QPCTime")?;
    let ticks: Vec<i64> = rows
        .iter()
        .filter(|row| row[process].parse::<u32>() == Ok(pid))
        .map(|row| row[qpc].parse::<i64>().unwrap())
        .collect();
    let first = ticks.iter().min().context("no attributed target rows")?;
    let last = ticks.iter().max().unwrap();
    ensure!(
        *first < start && *last > end,
        "target PID {pid} CSV does not bracket measured QPC window {start}..{end}; observed {first}..{last}; sample is incomplete"
    );
    Ok(
        json!({"targetPid": pid, "firstTargetQpc": first, "lastTargetQpc": last,
        "measurementStartQpc": start, "measurementEndQpc": end, "measurementBracketed": true}),
    )
}

fn summarize(csv: &str, pid: u32, start: i64, end: i64, frequency: i64) -> Result<Value> {
    ensure!(
        frequency > 0 && end > start,
        "invalid QPC measurement window"
    );
    let (header, rows) = presentmon_records(csv)?;
    let process = column(&header, "ProcessID")?;
    let swapchain = column(&header, "SwapChainAddress")?;
    let qpc = column(&header, "QPCTime")?;
    let mode = column(&header, "PresentMode")?;
    let dropped = column(&header, "Dropped")?;
    let sync = column(&header, "SyncInterval")?;
    let tearing = column(&header, "AllowsTearing")?;
    let mut chains: BTreeMap<String, Chain> = BTreeMap::new();
    for row in rows {
        if row[process].parse::<u32>()? != pid {
            continue;
        }
        let ticks: i64 = row[qpc].parse()?;
        if !(start..end).contains(&ticks) {
            continue;
        }
        let chain = chains.entry(row[swapchain].clone()).or_default();
        chain.count += 1;
        *chain.modes.entry(row[mode].clone()).or_default() += 1;
        *chain.sync.entry(row[sync].clone()).or_default() += 1;
        *chain.tearing.entry(row[tearing].clone()).or_default() += 1;
        if let Some(previous) = &chain.last_mode {
            if previous != &row[mode] {
                chain.transitions.push(json!({"from": previous, "to": row[mode], "qpc": ticks, "secondsFromMeasurementStart": (ticks - start) as f64 / frequency as f64}));
            }
        }
        chain.last_mode = Some(row[mode].clone());
        if let Some(previous) = chain.last_qpc {
            ensure!(ticks >= previous, "nonmonotonic presents within swapchain");
            if ticks > previous {
                chain
                    .intervals
                    .push((ticks - previous) as f64 * 1000.0 / frequency as f64);
            }
        }
        chain.last_qpc = Some(ticks);
        let number = |name: &str| {
            header
                .iter()
                .position(|v| v == name)
                .and_then(|i| row[i].parse::<f64>().ok())
                .filter(|v| v.is_finite() && *v != 0.0)
        };
        let was_dropped = match row[dropped].as_str() {
            "1" => true,
            "0" => false,
            other => bail!("invalid Dropped value {other}"),
        };
        if was_dropped {
            chain.dropped += 1;
        } else if let Some(latency) = number("msUntilDisplayed") {
            chain.latency.push(latency);
            let displayed = ticks as f64 + latency * frequency as f64 / 1000.0;
            if displayed >= start as f64 && displayed < end as f64 {
                chain.displayed.push(displayed);
            }
        }
        if let Some(value) = number("msUntilRenderComplete") {
            chain.render_latency.push(value);
        }
        if let Some(value) = number("msGPUActive") {
            chain.gpu.push(value);
        }
    }
    ensure!(
        !chains.is_empty(),
        "no target presents in the measured QPC window"
    );
    let seconds = (end - start) as f64 / frequency as f64;
    let mut result = serde_json::Map::new();
    let mut total = 0;
    for (address, mut chain) in chains {
        total += chain.count;
        chain.displayed.sort_by(f64::total_cmp);
        chain.displayed.dedup();
        let display_intervals: Vec<_> = chain
            .displayed
            .windows(2)
            .map(|pair| (pair[1] - pair[0]) * 1000.0 / frequency as f64)
            .collect();
        result.insert(address, json!({"presentCount": chain.count, "modes": chain.modes, "transitions": chain.transitions,
            "presentedFps": chain.count as f64 / seconds,
            "displayedFps": if chain.displayed.is_empty() { None } else { Some(chain.displayed.len() as f64 / seconds) },
            "dropped": chain.dropped, "allowsTearing": chain.tearing, "syncInterval": chain.sync,
            "presentIntervalMs": stats(chain.intervals), "displayIntervalMs": stats(display_intervals),
            "displayLatencyMs": stats(chain.latency), "presentToRenderCompleteMs": stats(chain.render_latency), "gpuActiveMs": stats(chain.gpu)}));
    }
    Ok(
        json!({"targetPid": pid, "startQpc": start, "endQpc": end, "seconds": seconds, "presentCount": total, "swapchains": result}),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    const HEADER: &str = "Application,ProcessID,SwapChainAddress,QPCTime,PresentMode,Dropped,SyncInterval,AllowsTearing,msUntilDisplayed,msUntilRenderComplete,msGPUActive\n";

    #[test]
    fn continuation_keeps_valid_slots_and_retries_only_the_missing_slot() {
        let variants = ["off", "hud-minimal", "hud-health", "hud-full"];
        let completed = json!([
            {"round":0,"variant":"off","warmupRun":true},
            {"round":1,"variant":"hud-full","warmupRun":false},
            {"round":1,"variant":"hud-health","warmupRun":false},
            {"round":1,"variant":"hud-minimal","warmupRun":false},
            {"round":1,"variant":"off","warmupRun":false},
            {"round":2,"variant":"off","warmupRun":false},
            {"round":2,"variant":"hud-minimal","warmupRun":false},
            {"round":2,"variant":"hud-health","warmupRun":false}
        ]);
        let slots = completed_slots(&completed, 5, &variants).unwrap();
        let missing = (1..=5)
            .flat_map(|round| variants.map(|variant| (round, variant.to_string())))
            .filter(|slot| !slots.contains(slot))
            .collect::<Vec<_>>();
        assert_eq!(missing.len(), 13);
        assert!(missing.contains(&(2, "hud-full".into())));
        assert!(!missing.contains(&(1, "hud-full".into())));
        for malformed in [
            json!([{"round":1,"variant":"off","warmupRun":false}, {"round":1,"variant":"off","warmupRun":false}]),
            json!([{"round":1,"variant":"unknown","warmupRun":false}]),
            json!([{"round":6,"variant":"off","warmupRun":false}]),
            json!([{"round":0,"variant":"off","warmupRun":false}]),
        ] {
            assert!(completed_slots(&malformed, 5, &variants).is_err());
        }
    }

    #[test]
    fn continuation_rejects_changed_measurements_and_unreviewed_product_bytes() {
        let prior = json!({"status":"incomplete", "schema":1, "cadenceOnly":false,
            "samplesPerVariant":5,"seconds":30,"warmup":5,"variants":["off","hud-minimal"],
            "baselineRequirement":"independent","targetMode":"fullscreen","traceDesktop":false,
            "targetArguments":["--fullscreen=yes"],"product":{"sha256":"original"},
            "target":{"sha256":"mpv"},"media":{"sha256":"clip"},"presentmon":{"sha256":"pm"}});
        assert!(validate_continuation_contract(&prior, &prior, None).is_ok());
        for field in [
            "seconds",
            "warmup",
            "variants",
            "targetArguments",
            "target",
            "media",
            "presentmon",
        ] {
            let mut changed = prior.clone();
            changed[field] = json!("changed");
            assert!(
                validate_continuation_contract(&prior, &changed, None).is_err(),
                "{field}"
            );
        }
        let mut rebuilt = prior.clone();
        rebuilt["product"]["sha256"] = json!("rebuilt");
        assert!(validate_continuation_contract(&prior, &rebuilt, None).is_err());
        assert!(validate_continuation_contract(&prior, &rebuilt, Some("wrong")).is_err());
        assert!(validate_continuation_contract(&prior, &rebuilt, Some("original")).is_ok());
        let mut running = prior.clone();
        running["status"] = json!("running");
        assert!(validate_continuation_contract(&running, &prior, None).is_err());
    }

    #[test]
    fn continuation_summary_uses_the_manifest_float_storage_roundtrip() {
        let computed = json!({"presentedFps":143.99991840004625_f64,"presentCount":4320,
            "modes":{"Hardware Composed: Independent Flip":4320},"unavailable":null});
        let stored: Value =
            serde_json::from_slice(&serde_json::to_vec(&computed).unwrap()).unwrap();
        assert_ne!(computed, stored);
        assert!(persisted_summary_matches(&computed, &stored).unwrap());
        for key in ["presentedFps", "presentCount", "modes", "unavailable"] {
            let mut changed = stored.clone();
            changed[key] = json!(0);
            assert!(
                !persisted_summary_matches(&computed, &changed).unwrap(),
                "{key}"
            );
        }
    }

    #[test]
    fn readiness_requires_complete_attributed_target_rows() {
        let row = "mpv.exe,42,0x1,1100,Composed: Flip,0,1,0,2,1,NA\n";
        for input in ["", "\u{feff}", HEADER, &format!("{HEADER}mpv.exe,42,0x1")] {
            assert!(
                !target_data_ready(input.as_bytes(), 42, 0).unwrap(),
                "{input}"
            );
        }
        let csv = format!("\u{feff}{HEADER}{row}");
        assert!(target_data_ready(csv.as_bytes(), 42, 0).unwrap());
        assert!(!target_data_ready(csv.as_bytes(), 99, 0).unwrap());
        assert!(!target_data_ready(csv.as_bytes(), 42, 2000).unwrap());
        // The writer can be observed midway through the next UTF-8 character.
        let mut partial = csv.into_bytes();
        partial.extend_from_slice(&[b'm', 0xe6, 0x9d]);
        assert!(target_data_ready(&partial, 42, 0).unwrap());
    }

    #[test]
    fn readiness_rejects_malformed_complete_machine_records() {
        for csv in [
            format!("{HEADER}mpv.exe,42,0x1\n"),
            format!("{HEADER}mpv.exe,42,0x1,bad-qpc,Composed: Flip,0,1,0,2,1,NA\n"),
            format!("{HEADER}mpv.exe,42,0x1,1100,Composed: Flip,invalid,1,0,2,1,NA\n"),
        ] {
            assert!(target_data_ready(csv.as_bytes(), 42, 0).is_err(), "{csv}");
        }
        assert!(target_data_ready(b"\xff\xfeA\0\n", 42, 0).is_err());
    }

    #[test]
    fn commands_use_stdout_and_only_owned_session() {
        let tool = Path::new("PresentMon.exe");
        let directory = Path::new("sample");
        let command = presentmon_command(tool, directory, "mpv.exe", "owned-session", 102);
        assert_eq!(command.get_current_dir(), Some(directory));
        let arguments: Vec<_> = command
            .get_args()
            .map(|arg| arg.to_str().unwrap())
            .collect();
        assert_eq!(
            arguments,
            [
                "--process_name",
                "mpv.exe",
                "--v1_metrics",
                "--qpc_time",
                "--no_console_stats",
                "--terminate_after_timed",
                "--timed",
                "102",
                "--session_name",
                "owned-session",
                "--output_stdout"
            ]
        );
        let stop = presentmon_stop_command(tool, "owned-session");
        assert_eq!(
            stop.get_args()
                .map(|arg| arg.to_str().unwrap())
                .collect::<Vec<_>>(),
            [
                "--terminate_existing_session",
                "--session_name",
                "owned-session"
            ]
        );
    }

    #[test]
    fn stdout_csv_decoding_is_lossless_and_strict() {
        let text = format!("{HEADER}münchen東京.exe,42,0x1,1100,Composed: Flip,0,1,0,2.125,1,NA\n");
        let mut raw = vec![0xff, 0xfe];
        raw.extend(text.encode_utf16().flat_map(u16::to_le_bytes));
        assert_eq!(decode_presentmon_stdout(&raw, false).unwrap(), text);
        assert_eq!(
            decode_presentmon_stdout(text.as_bytes(), false).unwrap(),
            text
        );
        let mut partial = raw.clone();
        partial.extend_from_slice(&[0x00, 0xd8, 0x01]);
        assert_eq!(decode_presentmon_stdout(&partial, true).unwrap(), text);
        assert!(decode_presentmon_stdout(&partial, false).is_err());
        assert!(decode_presentmon_stdout(&[0xff, 0xfe, 0x00, 0xd8, 10, 0], true).is_err());
    }

    #[test]
    fn coverage_requires_target_data_bracketing_the_entire_window() {
        let row = |pid, qpc| format!("mpv.exe,{pid},0x1,{qpc},Composed: Flip,0,1,0,2,1,NA\n");
        let valid = format!("{HEADER}{}{}{}", row(42, 900), row(42, 1100), row(42, 2100));
        assert_eq!(
            trace_coverage(&valid, 42, 1000, 2000).unwrap()["measurementBracketed"],
            true
        );
        for csv in [
            format!("{HEADER}{}", row(42, 1100)),
            format!("{HEADER}{}{}", row(99, 900), row(42, 2100)),
            format!("{HEADER}{}{}", row(42, 900), row(99, 2100)),
        ] {
            assert!(trace_coverage(&csv, 42, 1000, 2000).is_err());
        }
    }

    #[test]
    fn parser_rejects_malformed_metrics_instead_of_marking_unavailable() {
        for values in ["broken,1,NA", "2,NaN,NA", "2,1,infinity", "2,1,-1"] {
            let csv = format!("{HEADER}mpv.exe,42,0x1,1100,Composed: Flip,0,1,0,{values}\n");
            assert!(summarize(&csv, 42, 1000, 2000, 1000).is_err());
            assert!(target_data_ready(csv.as_bytes(), 42, 0).is_err());
        }
    }

    #[test]
    fn parser_preserves_signed_sync_and_kernel_swapchain_sentinels() {
        let csv = format!("{HEADER}mpv.exe,42,0x0,1100,Other,0,-1,0,NA,NA,NA\n");
        let summary = summarize(&csv, 42, 1000, 2000, 1000).unwrap();
        assert_eq!(summary["swapchains"]["0x0"]["syncInterval"]["-1"], 1);
        assert!(!target_data_ready(csv.as_bytes(), 42, 0).unwrap());
        let csv = csv.replace("0x0", "0x1");
        assert!(target_data_ready(csv.as_bytes(), 42, 0).unwrap());
    }

    #[test]
    fn completed_render_before_present_retains_signed_duration() {
        let csv = format!("{HEADER}mpv.exe,42,0x1,1100,Composed: Flip,0,1,0,2,-0.125,0.3\n");
        let summary = summarize(&csv, 42, 1000, 2000, 1000).unwrap();
        assert_eq!(
            summary["swapchains"]["0x1"]["presentToRenderCompleteMs"]["p50"],
            -0.125
        );
    }

    #[test]
    #[ignore = "Controlled child process used by collection lifecycle tests"]
    fn presentmon_collection_fixture() {
        use std::io::Write;
        let directory = PathBuf::from(std::env::var_os("EXO_VERIFY_PRESENTMON_FIXTURE").unwrap());
        let mode = std::env::var("EXO_VERIFY_PRESENTMON_FIXTURE_MODE").unwrap();
        if mode == "stop-session" {
            std::fs::write(directory.join("stop-session"), b"owned-session").unwrap();
            return;
        }
        std::fs::write(directory.join("started"), b"started").unwrap();
        if mode == "no-output-exit" {
            return;
        }
        std::thread::sleep(Duration::from_millis(100));
        let mut csv = if mode == "lazy-output" {
            let mut file =
                std::io::BufWriter::new(File::create(directory.join("presentmon.csv")).unwrap());
            write!(
                file,
                "\u{feff}{HEADER}other.exe,99,0x1,900,Composed: Flip,0,1,0,2,1,NA\nmpv.exe,42,0x1,"
            )
            .unwrap();
            file.flush().unwrap();
            std::thread::sleep(Duration::from_millis(100));
            file.write_all(b"1100,Composed: Flip,0,1,0,2,1,NA\n")
                .unwrap();
            file.flush().unwrap();
            std::thread::sleep(Duration::from_millis(100));
            file.write_all(b"mpv.exe,42,0x1,2100,Composed: Flip,0,1,0,2,1,NA\n")
                .unwrap();
            file.flush().unwrap();
            Some(file)
        } else {
            None
        };
        let deadline = Instant::now() + Duration::from_secs(10);
        while !directory.join("stop-session").exists() {
            assert!(Instant::now() < deadline, "fixture session never stopped");
            std::thread::sleep(Duration::from_millis(10));
        }
        if let Some(file) = csv.as_mut() {
            file.write_all(b"mpv.exe,42,0x1,2200,Composed: Flip,0,1,0,2,1,NA\n")
                .unwrap();
            file.flush().unwrap();
        }
    }

    fn collection_fixture_command(directory: &Path, mode: &str) -> Command {
        let mut command = Command::new(std::env::current_exe().unwrap());
        command
            .args([
                "--ignored",
                "--exact",
                "overlay_presentation::tests::presentmon_collection_fixture",
            ])
            .env("EXO_VERIFY_PRESENTMON_FIXTURE", directory)
            .env("EXO_VERIFY_PRESENTMON_FIXTURE_MODE", mode);
        command
    }

    fn collection_fixture(directory: &Path, mode: &str) -> PresentMonCapture {
        let process = spawn(
            &mut collection_fixture_command(directory, mode),
            &directory.join("presentmon.log"),
        )
        .unwrap();
        let capture = PresentMonCapture {
            process,
            tool: std::env::current_exe().unwrap(),
            session: "owned-session".into(),
            directory: directory.into(),
            stop_command: collection_fixture_command(directory, "stop-session"),
            finalized: false,
        };
        let deadline = Instant::now() + Duration::from_secs(5);
        while !directory.join("started").exists() {
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(10));
        }
        capture
    }

    #[test]
    fn collection_waits_for_lazy_complete_target_data_and_flushes_on_stop() {
        let directory = tempfile::tempdir().unwrap();
        let mut capture = collection_fixture(directory.path(), "lazy-output");
        let path = directory.path().join("presentmon.csv");
        wait_for_target_data(&mut capture.process.0, &path, 42, 0, Duration::from_secs(3)).unwrap();
        wait_for_target_data(
            &mut capture.process.0,
            &path,
            42,
            2000,
            Duration::from_secs(3),
        )
        .unwrap();
        capture.finish().unwrap();
        let csv = read_presentmon_csv(&path).unwrap();
        assert_eq!(
            summarize(&csv, 42, 1000, 2000, 1000).unwrap()["presentCount"],
            1
        );
        assert!(trace_coverage(&csv, 42, 1000, 2000).is_err()); // Wrong-PID prefix cannot establish coverage.
        assert!(csv.contains("mpv.exe,42,0x1,2200"));
        assert_eq!(
            std::fs::read(directory.path().join("stop-session")).unwrap(),
            b"owned-session"
        );
    }

    #[test]
    fn zero_exit_without_csv_is_collection_failure_with_source_context() {
        let directory = tempfile::tempdir().unwrap();
        let mut capture = collection_fixture(directory.path(), "no-output-exit");
        let error = wait_for_target_data(
            &mut capture.process.0,
            &directory.path().join("presentmon.csv"),
            42,
            0,
            Duration::from_secs(2),
        )
        .with_context(|| capture.context("target data readiness failed"))
        .unwrap_err();
        let text = format!("{error:#}");
        for expected in [
            "PresentMon",
            "presentmon-stdout.bin",
            "presentmon.log",
            "owned-session",
            "before target PID 42 data readiness",
        ] {
            assert!(text.contains(expected), "{text}");
        }
    }

    #[test]
    #[ignore = "Requires explicitly provided PresentMon executable and offline ETL"]
    fn offline_presentmon_stdout_contract() {
        let tool = PathBuf::from(std::env::var_os("EXO_VERIFY_OFFLINE_PRESENTMON").unwrap());
        let etl = PathBuf::from(std::env::var_os("EXO_VERIFY_OFFLINE_ETL").unwrap());
        let directory = tempfile::tempdir().unwrap();
        let raw = directory.path().join("presentmon-stdout.bin");
        let mut capture = spawn_with_stdout(
            Command::new(tool).arg("--etl_file").arg(etl).args([
                "--process_name",
                "dwm.exe",
                "--v1_metrics",
                "--qpc_time",
                "--output_stdout",
            ]),
            &directory.path().join("presentmon.log"),
            Some(&raw),
        )
        .unwrap();
        assert!(
            crate::tools::wait(&mut capture.0, Duration::from_secs(10))
                .unwrap()
                .success()
        );
        let bytes = std::fs::read(&raw).unwrap();
        assert!(bytes.starts_with(&[0xff, 0xfe]));
        let csv = decode_presentmon_stdout(&bytes, false).unwrap();
        assert!(target_data_ready(csv.as_bytes(), 1300, 0).unwrap());
        assert_eq!(decode_presentmon_stdout(&bytes, true).unwrap(), csv);
        let normalized = directory.path().join("presentmon.csv");
        std::fs::write(&normalized, &csv).unwrap();
        assert_eq!(read_presentmon_csv(&normalized).unwrap(), csv);
        assert_eq!(
            csv.encode_utf16()
                .flat_map(u16::to_le_bytes)
                .collect::<Vec<_>>(),
            bytes[2..]
        );
    }

    #[test]
    fn missing_data_times_out_and_error_drop_stops_only_owned_session() {
        let directory = tempfile::tempdir().unwrap();
        let mut capture = collection_fixture(directory.path(), "no-data");
        let error = wait_for_target_data(
            &mut capture.process.0,
            &directory.path().join("presentmon.csv"),
            42,
            0,
            Duration::from_millis(100),
        )
        .unwrap_err();
        assert!(error.to_string().contains("no complete target PID 42 rows"));
        drop(capture);
        assert_eq!(
            std::fs::read(directory.path().join("stop-session")).unwrap(),
            b"owned-session"
        );
        let stop = read_json(&directory.path().join("presentmon-stop.json")).unwrap();
        assert_eq!(stop["sessionName"], "owned-session");
        assert_eq!(stop["exitCode"], 0);
        assert!(
            !directory
                .path()
                .join("presentmon-cleanup-error.json")
                .exists()
        );
    }

    #[test]
    #[ignore = "Controlled child process used by finalization tests"]
    fn presentmon_finalization_fixture() {
        use std::io::{Read, Write};
        let directory = PathBuf::from(std::env::var_os("EXO_VERIFY_PRESENTMON_FIXTURE").unwrap());
        let mode = std::env::var("EXO_VERIFY_PRESENTMON_FIXTURE_MODE").unwrap();
        let mut csv =
            std::io::BufWriter::new(File::create(directory.join("presentmon.csv")).unwrap());
        csv.write_all(b"Application,ProcessID\nmpv.exe,").unwrap();
        csv.flush().unwrap();
        std::fs::write(directory.join("ready"), b"ready").unwrap();
        if mode != "exited" {
            let mut stop = [0u8; 4];
            std::io::stdin().read_exact(&mut stop).unwrap();
            assert_eq!(&stop, b"stop");
        }
        if mode == "ignore-stop" {
            std::thread::sleep(Duration::from_secs(5));
        }
        csv.write_all(b"42\n").unwrap();
        std::thread::sleep(Duration::from_millis(50));
        csv.flush().unwrap();
    }

    fn finalization_fixture(directory: &Path, mode: &str) -> Child {
        use std::process::Stdio;
        let child = Command::new(std::env::current_exe().unwrap())
            .args([
                "--ignored",
                "--exact",
                "overlay_presentation::tests::presentmon_finalization_fixture",
            ])
            .env("EXO_VERIFY_PRESENTMON_FIXTURE", directory)
            .env("EXO_VERIFY_PRESENTMON_FIXTURE_MODE", mode)
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(5);
        while !directory.join("ready").exists() {
            assert!(Instant::now() < deadline, "fixture did not start");
            std::thread::sleep(Duration::from_millis(10));
        }
        child
    }

    #[test]
    fn finalization_requests_stop_and_waits_for_csv_flush() {
        use std::io::Write;
        let directory = tempfile::tempdir().unwrap();
        let mut capture = finalization_fixture(directory.path(), "normal");
        let mut control = capture.stdin.take().unwrap();
        finish_presentmon(
            &mut capture,
            || Ok(control.write_all(b"stop")?),
            Duration::from_secs(1),
        )
        .unwrap();
        assert_eq!(
            std::fs::read(directory.path().join("presentmon.csv")).unwrap(),
            b"Application,ProcessID\nmpv.exe,42\n"
        );
    }

    #[test]
    fn finalization_does_not_stop_an_already_exited_capture() {
        let directory = tempfile::tempdir().unwrap();
        let mut capture = finalization_fixture(directory.path(), "exited");
        assert!(
            crate::tools::wait(&mut capture, Duration::from_secs(2))
                .unwrap()
                .success()
        );
        finish_presentmon(
            &mut capture,
            || panic!("already exited capture must not receive a stop request"),
            Duration::from_secs(1),
        )
        .unwrap();
    }

    #[test]
    fn finalization_stops_owned_session_after_abnormal_process_exit() {
        let directory = tempfile::tempdir().unwrap();
        let mut capture = finalization_fixture(directory.path(), "ignore-stop");
        capture.kill().unwrap();
        assert!(!capture.wait().unwrap().success());
        let mut stopped = false;
        let error = finish_presentmon(
            &mut capture,
            || {
                stopped = true;
                Ok(())
            },
            Duration::from_secs(1),
        )
        .unwrap_err();
        assert!(
            stopped,
            "an abnormal exit does not establish ETW session teardown"
        );
        assert!(error.to_string().contains("PresentMon capture exited"));
    }

    #[test]
    fn finalization_timeout_names_presentmon_and_preserves_stop_failure() {
        use std::io::Write;
        let directory = tempfile::tempdir().unwrap();
        let mut capture = finalization_fixture(directory.path(), "ignore-stop");
        let mut control = capture.stdin.take().unwrap();
        let error = finish_presentmon(
            &mut capture,
            || {
                control.write_all(b"stop")?;
                bail!("session stop fixture failed")
            },
            Duration::from_millis(100),
        )
        .unwrap_err();
        let message = format!("{error:#}");
        assert!(message.contains("PresentMon"), "{message}");
        assert!(message.contains("session stop fixture failed"), "{message}");
        assert!(capture.try_wait().unwrap().is_some());
    }

    #[test]
    fn finalization_accepts_natural_exit_racing_a_failed_stop_request() {
        use std::io::Write;
        let directory = tempfile::tempdir().unwrap();
        let mut capture = finalization_fixture(directory.path(), "normal");
        let mut control = capture.stdin.take().unwrap();
        finish_presentmon(
            &mut capture,
            || {
                control.write_all(b"stop")?;
                bail!("session already ended")
            },
            Duration::from_secs(1),
        )
        .unwrap();
        assert_eq!(
            std::fs::read(directory.path().join("presentmon.csv")).unwrap(),
            b"Application,ProcessID\nmpv.exe,42\n"
        );
    }

    #[test]
    fn parser_rejects_truncated_final_csv_row() {
        let csv = format!(
            "{HEADER}mpv.exe,42,0x1,1100,Composed: Flip,0,1,0,2,1,NA\nmpv.exe,42,0x1,1200,Composed: Flip,0,"
        );
        assert!(
            summarize(&csv, 42, 1000, 2000, 1000)
                .unwrap_err()
                .to_string()
                .contains("row length mismatch")
        );
    }

    #[test]
    fn presentmon_csv_utf8_and_bom_preserve_fields() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("presentmon.csv");
        let csv = format!(
            "{HEADER}münchen東京.exe,42,0x1,1100,Hardware: Independent Flip,0,1,0,2.125,1,NA\n"
        );
        for prefix in ["", "\u{feff}"] {
            std::fs::write(&path, format!("{prefix}{csv}")).unwrap();
            let decoded = read_presentmon_csv(&path).unwrap();
            assert_eq!(decoded.trim_start_matches('\u{feff}'), csv);
            let parsed = summarize(&decoded, 42, 1000, 2000, 1000).unwrap();
            assert_eq!(
                parsed["swapchains"]["0x1"]["displayLatencyMs"]["p50"],
                2.125
            );
        }
    }

    #[test]
    fn presentmon_csv_rejects_invalid_encoding_with_attribution() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("presentmon.csv");
        let nul_text = format!("{HEADER}mpv\0.exe,42,0x1,1100,Composed: Flip,0,1,0,2,1,NA\n");
        for bytes in [
            vec![0xff, 0xfe, b'A', 0],
            vec![0xfe, 0xff, 0, b'A'],
            vec![0x80],
            nul_text.into_bytes(),
        ] {
            std::fs::write(&path, bytes).unwrap();
            let error = format!("{:#}", read_presentmon_csv(&path).unwrap_err());
            assert!(error.contains("PresentMon"), "{error}");
            assert!(error.contains("presentmon.csv"), "{error}");
            assert!(error.contains("UTF-8"), "{error}");
        }
    }

    #[test]
    fn presentmon_csv_malformed_utf8_is_not_silently_parsed() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("presentmon.csv");
        let csv = format!("{HEADER}mpv.exe,42,0x1,not-a-qpc,Composed: Flip,0,1,0,2,1,NA\n");
        std::fs::write(&path, csv).unwrap();
        let decoded = read_presentmon_csv(&path).unwrap();
        assert!(summarize(&decoded, 42, 1000, 2000, 1000).is_err());
    }

    #[test]
    fn binds_pid_swapchain_and_qpc_window_and_retains_exact_modes() {
        let csv = format!(
            "{HEADER}mpv.exe,42,0x1,900,Composed: Flip,0,1,0,2,1,NA\nother.exe,99,0x1,1100,Composed: Flip,0,1,0,2,1,NA\nmpv.exe,42,0x1,1100,Hardware: Independent Flip,0,1,0,2,1,NA\nmpv.exe,42,0x1,1200,Hardware Composed: Independent Flip,0,1,0,2,1,NA\nmpv.exe,42,0x1,1300,Composed: Flip,0,1,0,2,1,NA\nmpv.exe,42,0x2,1400,Hardware: Independent Flip,0,1,0,2,1,NA\nmpv.exe,42,0x1,2100,Composed: Flip,0,1,0,2,1,NA\n"
        );
        let result = summarize(&csv, 42, 1000, 2000, 1000).unwrap();
        assert_eq!(result["presentCount"], 4);
        assert_eq!(
            result["swapchains"]["0x1"]["transitions"]
                .as_array()
                .unwrap()
                .len(),
            2
        );
        assert_eq!(
            result["swapchains"]["0x1"]["modes"]["Hardware Composed: Independent Flip"],
            1
        );
        assert_eq!(
            result["swapchains"]["0x1"]["transitions"][0]["secondsFromMeasurementStart"],
            0.2
        );
        assert_eq!(
            result["swapchains"]["0x1"]["presentIntervalMs"]["p99"],
            100.0
        );
        assert!(result["swapchains"]["0x1"]["gpuActiveMs"].is_null());
    }

    #[test]
    fn discarded_presents_do_not_fabricate_display_latency() {
        let csv = format!(
            "{HEADER}mpv.exe,42,0x1,1100,Composed: Flip,1,0,1,0,0,0\nmpv.exe,42,0x1,1200,Composed: Flip,0,0,1,NA,NA,NA\n"
        );
        let result = summarize(&csv, 42, 1000, 2000, 1000).unwrap();
        let chain = &result["swapchains"]["0x1"];
        assert_eq!(chain["dropped"], 1);
        assert!(chain["displayLatencyMs"].is_null());
        assert!(chain["displayIntervalMs"].is_null());
        assert_eq!(chain["allowsTearing"]["1"], 2);
    }

    #[test]
    fn rejects_unattributed_empty_and_malformed_traces() {
        assert!(summarize(HEADER, 42, 1000, 2000, 1000).is_err());
        assert!(summarize("PresentMode\nComposed: Flip\n", 42, 1000, 2000, 1000).is_err());
        let csv = format!("{HEADER}mpv.exe,42,0x1,1100,\"unterminated,0,1,0,2,1,NA\n");
        assert!(summarize(&csv, 42, 1000, 2000, 1000).is_err());
    }

    #[test]
    fn rejects_a_stopped_recording_and_wrong_diagnostics_preset() {
        let hud = json!({"visible": true, "exposed": true, "nativeValid": true, "nativeVisible": true,
            "captureExcluded": true, "transparentForInput": true, "layered": true, "overlayState": 1,
            "diagnosticsActive": false});
        let windows = json!([hud, {"visible": false}]);
        let mut evidence = json!({"startWindows": windows, "endWindows": windows,
            "startPipeline": {"valid": true, "lifecycle": "recording"},
            "endPipeline": {"valid": true, "lifecycle": "recording"}});
        assert!(validate_overlay(&evidence, "hud-minimal").is_ok());
        evidence["endPipeline"]["lifecycle"] = json!("idle");
        assert!(validate_overlay(&evidence, "hud-minimal").is_err());
        evidence["endPipeline"]["lifecycle"] = json!("recording");
        evidence["endWindows"][0]["diagnosticsActive"] = json!(true);
        assert!(validate_overlay(&evidence, "hud-minimal").is_err());
    }

    #[test]
    fn verifies_production_health_and_technical_content() {
        for variant in ["hud-health", "hud-full", "hud-full-dock"] {
            let full = variant != "hud-health";
            let dock = variant.ends_with("-dock");
            let hud = json!({"visible": true, "exposed": true, "nativeValid": true,
                "nativeVisible": true, "captureExcluded": true, "transparentForInput": true,
                "layered": true, "overlayState": 1, "diagnosticsActive": true,
                "showFps": full, "showDrop": true, "showDrift": full,
                "showDiagnosticsSize": full, "showMutedSources": true, "showHealth": true});
            let control = json!({"visible": dock, "exposed": dock, "nativeValid": true,
                "nativeVisible": dock, "captureExcluded": true,
                "transparentForInput": false, "layered": false});
            let windows = json!([hud, control]);
            let mut evidence = json!({"startWindows": windows, "endWindows": windows,
                "startPipeline": {"valid": true, "lifecycle": "recording"},
                "endPipeline": {"valid": true, "lifecycle": "recording"}});
            assert!(validate_overlay(&evidence, variant).is_ok(), "{variant}");
            evidence["endWindows"][0]["showHealth"] = json!(false);
            assert!(validate_overlay(&evidence, variant).is_err(), "{variant}");
            evidence["endWindows"][0]["showHealth"] = json!(true);
            evidence["startWindows"][0]["showDrift"] = json!(!full);
            assert!(validate_overlay(&evidence, variant).is_err(), "{variant}");
        }
    }

    #[test]
    fn composed_baseline_and_multiple_swapchains_cannot_establish_neutrality() {
        assert!(!suitable_baseline(
            &json!({"swapchains": {"1": {"presentCount": 100, "modes": {"Composed: Flip": 100}}}})
        ));
        let independent = json!({"presentCount": 100, "modes": {"Hardware: Independent Flip": 50, "Hardware Composed: Independent Flip": 50}});
        assert!(suitable_baseline(
            &json!({"swapchains": {"1": independent}})
        ));
        assert!(!suitable_baseline(
            &json!({"swapchains": {"1": independent, "2": independent}})
        ));
    }

    #[test]
    fn every_variant_requires_one_target_swapchain() {
        assert!(validate_presentation_sample(&json!({"swapchains": {"1": {}}})).is_ok());
        assert!(validate_presentation_sample(&json!({"swapchains": {"1": {}, "2": {}}})).is_err());
    }

    #[test]
    fn binds_capture_target_overlays_and_main_to_exact_displays() {
        let report = json!({"recording_config": {"target_kind": "monitor", "target_description": "\\\\.\\DISPLAY1"}});
        let desktop = json!({"targetMonitor": "\\\\.\\DISPLAY1", "recorderMainMonitor": "\\\\.\\DISPLAY2", "targetRect": [0, 0, 2560, 1440]});
        let windows = json!([{"visible": true, "x": 2420, "y": 20, "width": 120, "height": 31}, {"visible": false}]);
        let mut overlay = json!({"startWindows": windows, "endWindows": windows});
        assert!(validate_display_bindings(&report, &desktop, &overlay).is_ok());
        let wrong = json!({"recording_config": {"target_kind": "monitor", "target_description": "\\\\.\\DISPLAY2"}});
        assert!(validate_display_bindings(&wrong, &desktop, &overlay).is_err());
        overlay["endWindows"][0]["x"] = json!(2600);
        assert!(validate_display_bindings(&report, &desktop, &overlay).is_err());
    }
}
#[test]
fn retired_synthetic_variants_are_rejected() {
    for variant in [
        "hud-paint-4",
        "hud-text-60",
        "hud-animated",
        "hud-animated-dock",
    ] {
        assert!(!valid_variant(variant), "{variant}");
    }
}

#[test]
fn production_variants_and_mode_requirements_are_explicit() {
    for variant in [
        "off",
        "hud-minimal",
        "hud-health",
        "hud-full",
        "dock-only",
        "hud-full-dock",
    ] {
        assert!(valid_variant(variant), "{variant}");
    }
    for rate in [1, 4, 10, 30, 60, 144] {
        for kind in ["paint", "text"] {
            assert!(!valid_variant(&format!("hud-{kind}-{rate}")));
        }
    }
    for variant in ["hud-animated", "hud-animated-dock", "hud-normal"] {
        assert!(!valid_variant(variant), "{variant}");
    }
    assert!(!valid_variant("hud-paint-120"));
    let composed = json!({"swapchains":{"1":{"presentCount":100,"modes":{"Composed: Flip":100}}}});
    assert!(baseline_matches(&composed, "composed"));
    assert!(!baseline_matches(&composed, "independent"));
    let hcif = json!({"swapchains":{"1":{"presentCount":100,"modes":{"Hardware Composed: Independent Flip":100}}}});
    assert!(baseline_matches(&hcif, "independent"));
    assert!(!baseline_matches(&hcif, "if"));
    let command = presentmon_command(
        Path::new("PresentMon.exe"),
        Path::new("sample"),
        "",
        "owned",
        60,
    );
    assert!(!command.get_args().any(|v| v == "--process_name"));
    assert!(command.get_args().any(|v| v == "--session_name"));
}
