//! A true DXGI exclusive-fullscreen source checked against independent ETW decoding.

use serde_json::{Value, json};
use std::path::PathBuf;
use std::process::{Command, Stdio};

use super::common::{self, secs};
use crate::capability::Capability;
use crate::context::Context;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![Scenario {
        id: "capture.exclusive-fullscreen",
        revision: 2,
        title: "True exclusive fullscreen capture is detected and explained",
        claim: "a probe that enters DXGI exclusive fullscreen is independently observed by PresentMon and the candidate reports the same presentation mode",
        lane: Lane::Hardware,
        also: &[],
        tier: Tier::Recommended,
        requires: &[
            Capability::Windows,
            Capability::Admin,
            Capability::InteractiveDesktop,
            Capability::D3d11,
            Capability::Operator,
        ],
        timeout: secs(240.0),
        run: exclusive_fullscreen,
    }]
}

fn presentmon_path() -> Step<PathBuf> {
    if let Some(path) = std::env::var_os("EXOSNAP_PRESENTMON") {
        let path = PathBuf::from(path);
        if path.is_file() {
            return Ok(path);
        }
    }
    crate::tools::resolve("PresentMon")
        .ok_or_else(|| Stop::unavailable("PresentMon.exe is not available"))
}

fn probe_path() -> Step<PathBuf> {
    crate::tools::resolve("probe_fullscreen_present")
        .ok_or_else(|| Stop::unavailable("probe_fullscreen_present.exe is not available; no true DXGI exclusive fullscreen source can be created"))
}

fn capture_presentmon(ctx: &mut Context, exe: &PathBuf, pid: u32) -> Step<String> {
    let help = crate::tools::run(Command::new(exe).arg("--help"), secs(20.0))?;
    let advertised = format!("{}\n{}", help.stdout, help.stderr);
    infra_ensure!(
        advertised.contains("--process_id") && advertised.contains("--output_file"),
        "PresentMon does not advertise process-bound CSV capture"
    );
    let csv = ctx.scenario_dir.join("presentmon-fse.csv");
    let mut command = Command::new(exe);
    command.args(["--process_id", &pid.to_string()]);
    command.arg("--output_file").arg(&csv);
    command.args([
        "--timed",
        "5",
        "--terminate_after_timed",
        "--stop_existing_session",
    ]);
    if advertised.contains("--no_console_stats") {
        command.arg("--no_console_stats");
    } else if advertised.contains("--no_top") {
        command.arg("--no_top");
    }
    if advertised.contains("--v1_metrics") {
        command.arg("--v1_metrics");
    }
    let result = crate::tools::run(&mut command, secs(65.0))?;
    infra_ensure!(
        result.success(),
        "PresentMon failed to capture process {pid}: {}",
        result.stderr
    );
    infra_ensure!(csv.is_file(), "PresentMon wrote no CSV for process {pid}");
    ctx.keep(&csv);
    Ok(std::fs::read_to_string(csv)?)
}

fn csv_fields(line: &str) -> Vec<String> {
    let mut fields = Vec::new();
    let mut current = String::new();
    let mut quoted = false;
    let mut chars = line.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '"' if quoted && chars.peek() == Some(&'"') => {
                current.push('"');
                chars.next();
            }
            '"' => quoted = !quoted,
            ',' if !quoted => {
                fields.push(std::mem::take(&mut current));
            }
            _ => current.push(c),
        }
    }
    fields.push(current);
    fields
}

fn judge_oracle(csv: &str, pid: u32) -> Step<usize> {
    let mut lines = csv.lines().filter(|line| !line.trim().is_empty());
    let header = csv_fields(
        lines
            .next()
            .ok_or_else(|| Stop::infra("PresentMon CSV is empty"))?,
    );
    let pid_index = header
        .iter()
        .position(|field| field.eq_ignore_ascii_case("ProcessID"))
        .ok_or_else(|| Stop::infra("PresentMon CSV has no ProcessID column"))?;
    let mode_index = header
        .iter()
        .position(|field| field.eq_ignore_ascii_case("PresentMode"))
        .ok_or_else(|| Stop::infra("PresentMon CSV has no PresentMode column"))?;
    let mut total = 0;
    let mut exclusive = 0;
    for line in lines {
        let fields = csv_fields(line);
        if fields.len() < header.len() {
            continue;
        }
        if fields[pid_index].trim().parse::<u32>().ok() != Some(pid) {
            continue;
        }
        total += 1;
        let mode = fields[mode_index].trim();
        if matches!(
            mode,
            "Hardware: Legacy Flip" | "Hardware: Legacy Copy to front buffer"
        ) {
            exclusive += 1;
        }
    }
    if total == 0 {
        return Err(Stop::unavailable(format!(
            "PresentMon observed no presents from probe process {pid}"
        )));
    }
    if exclusive == 0 {
        return Err(Stop::unavailable(format!(
            "PresentMon observed {total} probe presents, but none in DXGI exclusive fullscreen"
        )));
    }
    Ok(exclusive)
}

fn judge_product(environment: &Value) -> Step {
    let present = &environment["present"];
    if present["available"] != true {
        return Err(Stop::unavailable(format!(
            "candidate present diagnostics unavailable ({})",
            present["availability"]
        )));
    }
    product_ensure!(
        present["presentCount"]
            .as_f64()
            .is_some_and(|count| count > 0.0),
        "candidate reported no attributed presents from the exclusive-fullscreen source"
    );
    product_ensure!(
        present["mode"] == "exclusiveFullscreen",
        "PresentMon measured exclusive fullscreen, but candidate reported {}",
        present["mode"]
    );
    Ok(())
}

fn exclusive_fullscreen(ctx: &mut Context) -> Step {
    let probe_exe = probe_path()?;
    let presentmon_exe = presentmon_path()?;
    if !ctx.ask("Is this machine free for the DXGI exclusive-fullscreen probe to take focus and for PresentMon to record ETW for five seconds?")? {
        return Err(Stop::unavailable("the operator did not release the desktop for the focus-taking probe"));
    }
    let mut app = ctx.launch(&[])?;
    app.client
        .request(
            "diagnostics.setInDepth",
            json!({"enabled": true}),
            secs(15.0),
        )?
        .map_err(|refusal| {
            Stop::infra(format!("in-depth diagnostics opt-in refused: {refusal}"))
        })?;
    let before = app.call("environment.snapshot", json!({}))?;
    if before["present"]["available"] != true {
        return Err(Stop::unavailable(format!(
            "candidate present diagnostics unavailable ({})",
            before["present"]["availability"]
        )));
    }
    let mut probe = ctx.spawn(
        Command::new(probe_exe)
            .args(["--display", "0", "--seconds", "90"])
            .stdout(Stdio::null())
            .stderr(Stdio::null()),
    )?;
    let pid = probe.id();
    let result = (|| -> Step {
        std::thread::sleep(secs(3.0));
        infra_ensure!(
            probe.try_wait()?.is_none(),
            "the exclusive-fullscreen probe exited before measurement"
        );
        common::select_window(&mut app, &format!("ExoSnap FSE probe [{pid}]"))?;
        std::thread::sleep(secs(5.0));
        let csv = capture_presentmon(ctx, &presentmon_exe, pid)?;
        let independent_presents = judge_oracle(&csv, pid)?;
        let environment = app.call("environment.snapshot", json!({}))?;
        ctx.evidence.put("present", environment.clone());
        ctx.evidence
            .put("independentExclusivePresents", independent_presents);
        judge_product(&environment)
    })();
    let _ = probe.kill();
    let _ = probe.wait();
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn presentmon_rows_are_bound_to_the_probe_pid() {
        let csv = "Application,ProcessID,PresentMode\n\"Other, game\",42,Hardware: Legacy Flip\nProbe,73,Hardware: Legacy Flip\n";
        assert!(matches!(judge_oracle(csv, 73), Ok(1)));
        assert!(matches!(judge_oracle(csv, 99), Err(Stop::Unavailable(_))));
    }

    #[test]
    fn independent_exclusive_mode_requires_product_agreement() {
        assert!(matches!(
            judge_product(
                &json!({"present": {"available": true, "mode": "composed", "presentCount": 10}})
            ),
            Err(Stop::Fail(_))
        ));
        judge_product(&json!({"present": {"available": true, "mode": "exclusiveFullscreen", "presentCount": 10}})).unwrap();
    }
}
