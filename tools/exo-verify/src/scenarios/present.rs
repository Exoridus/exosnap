//! Privileged and independently observed presentation gates.

use crate::capability::Capability;
use crate::context::Context;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use serde_json::{Value, json};
use std::path::PathBuf;
use std::process::Command;
use std::time::{Duration, Instant};

use super::common::{self, secs};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "diagnostics.present-elevated",
            revision: 1,
            title: "Elevated present diagnostics decode real presents",
            claim: "an elevated product process opens present diagnostics and classifies real decoded presents during a recording",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::Windows,
                Capability::InteractiveDesktop,
                Capability::D3d11,
                Capability::Admin,
            ],
            timeout: std::time::Duration::from_secs(300),
            run: elevated_observation,
        },
        Scenario {
            id: "diagnostics.present-crosscheck",
            revision: 2,
            title: "PresentMon independently confirms the product's presentation mode",
            claim: "a PresentMon ETW capture attributed to this product process shows a present in the product's reported mode, and its most recently observed present agrees with the mode the product reports at snapshot time",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::Windows,
                Capability::InteractiveDesktop,
                Capability::D3d11,
                Capability::Admin,
            ],
            timeout: std::time::Duration::from_secs(300),
            run: independent_crosscheck,
        },
    ]
}

fn elevated_observation(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    app.call("diagnostics.setInDepth", json!({"enabled": true}))?;
    let environment = app.call("environment.snapshot", json!({}))?;
    let screen = common::primary_screen(&environment, true)
        .ok_or_else(|| Stop::infra("the elevated product reported no display"))?;
    common::select_display(&mut app, &common::screen_device(screen)?)?;
    common::start_recording(&mut app)?;
    let deadline = Instant::now() + secs(30.0);
    let mut present = Value::Null;
    while Instant::now() < deadline {
        present = app.call("environment.snapshot", json!({}))?["present"].clone();
        if present["available"] == true
            && present["presentCount"]
                .as_f64()
                .is_some_and(|count| count > 0.0)
        {
            break;
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    let result = common::stop_recording(&mut app)?;
    ctx.evidence.put("present", present.clone());
    ctx.evidence.put("recordResult", result);
    judge_elevated_present(&present)
}

fn independent_crosscheck(ctx: &mut Context) -> Step {
    let presentmon = std::env::var_os("EXO_VERIFY_PRESENTMON")
        .or_else(|| std::env::var_os("EXOSNAP_PRESENTMON"))
        .map(PathBuf::from)
        .or_else(|| crate::tools::resolve("PresentMon"))
        .ok_or_else(independent_crosscheck_unavailable_reason)?;
    if !presentmon.is_file() {
        return Err(Stop::unavailable(format!(
            "PresentMon executable does not exist: {}",
            presentmon.display()
        )));
    }
    let mut app = ctx.launch(&[])?;
    common::configure_exact(&mut app, &[("app.hideWindowFromCapture", json!(false))])?;
    app.call("diagnostics.setInDepth", json!({"enabled": true}))?;
    let windows = app.call("windows.snapshot", json!({}))?;
    let title = windows["windows"]
        .as_array()
        .and_then(|items| items.iter().find(|window| window["role"] == "main"))
        .and_then(|window| window["title"].as_str())
        .filter(|title| !title.is_empty())
        .ok_or_else(|| Stop::infra("the product's main window has no targetable title"))?;
    common::select_window(&mut app, title).map_err(|_| {
        Stop::unavailable(
            "the product does not offer its own window as a capture target on this machine",
        )
    })?;
    let pid = app.child.id();
    let selection = common::record_snapshot(&mut app)?;
    infra_ensure!(
        selection["presentAttributionPid"].as_f64() == Some(pid as f64),
        "the selected capture target is not attributed to the product process {pid}: {selection}"
    );
    common::start_recording(&mut app)?;
    let csv_path = ctx.scenario_dir.join("presentmon.csv");
    let session = crate::control::new_run_id("exov-present");
    let capture = crate::tools::run(
        Command::new(&presentmon)
            .args(["--process_id", &pid.to_string(), "--output_file"])
            .arg(&csv_path)
            .args([
                "--timed",
                "8",
                "--terminate_after_timed",
                "--session_name",
                &session,
            ]),
        secs(45.0),
    )?;
    let environment = app.call("environment.snapshot", json!({}))?;
    let pipeline = app.call("pipeline.snapshot", json!({}))?;
    let result = common::stop_recording(&mut app)?;
    infra_ensure!(
        capture.success() && csv_path.is_file(),
        "PresentMon could not capture this process: {}",
        capture.stderr.trim()
    );
    let present = &environment["present"];
    judge_elevated_present(present)?;
    let mode = present["mode"].as_str().unwrap();
    let pipeline_mode = pipeline["sourcePresentation"]["presentMode"]
        .as_str()
        .ok_or_else(|| Stop::infra("pipeline reports no mode for the attributed source process"))?;
    product_ensure!(
        pipeline_mode == mode,
        "product environment mode {mode} disagrees with pipeline mode {pipeline_mode}"
    );
    let csv = std::fs::read_to_string(&csv_path)?;
    let summary = compare_presentmon(&csv, pid, mode)?;
    ctx.evidence.put("present", present.clone());
    ctx.evidence.put(
        "pipelineSourcePresentation",
        pipeline["sourcePresentation"].clone(),
    );
    ctx.evidence.put("recordResult", result);
    ctx.evidence.put("presentMonProcessId", pid as u64);
    ctx.evidence
        .put("presentMonPresents", summary.total_presents as u64);
    ctx.evidence.put(
        "presentMonModeDistribution",
        json!(
            summary
                .distribution
                .iter()
                .map(|(mode, count)| (mode.to_string(), *count as u64))
                .collect::<std::collections::BTreeMap<_, _>>()
        ),
    );
    ctx.evidence.put("presentMonLastMode", summary.last_mode);
    ctx.keep(&csv_path);
    Ok(())
}

fn judge_elevated_present(present: &Value) -> Step {
    product_ensure!(
        present["elevated"] == true,
        "the elevated product reports present.elevated false"
    );
    product_ensure!(
        present["available"] == true,
        "present diagnostics stayed unavailable under elevation: {present}"
    );
    product_ensure!(
        present["presentCount"]
            .as_f64()
            .is_some_and(|count| count > 0.0),
        "the elevated session decoded no presents while recording"
    );
    product_ensure!(
        present["mode"].as_str().is_some_and(|mode| matches!(
            mode,
            "composed" | "independentFlip" | "exclusiveFullscreen"
        )),
        "presents were decoded but no presentation mode was classified"
    );
    Ok(())
}

fn independent_crosscheck_unavailable_reason() -> Stop {
    Stop::unavailable(
        "no pinned PresentMon console executable is configured (set EXO_VERIFY_PRESENTMON); the runner needs it to capture this run's attributed process",
    )
}

struct PresentMonSummary {
    total_presents: usize,
    distribution: std::collections::BTreeMap<&'static str, usize>,
    last_mode: &'static str,
}

/// The product exposes `present.mode` as the mode of the single most recently
/// decoded present at snapshot time (`PresentMonEtwSession::latest_`, overwritten
/// per drained event), not an aggregate over the capture window. DWM legitimately
/// interleaves `Composed: Flip` and `Hardware Composed: Independent Flip` for the
/// same process across an 8-second window (observed: 1127 vs. 157 presents,
/// scattered throughout), so requiring one homogeneous mode for the whole window
/// is a false assumption about desktop composition, not a product defect. Compare
/// against the same "last observed" semantics PresentMon carries: chronologically
/// last matching row, not the mode set as a whole.
fn compare_presentmon(csv: &str, process_id: u32, product_mode: &str) -> Step<PresentMonSummary> {
    let mut lines = csv.lines().filter(|line| !line.trim().is_empty());
    let header = lines
        .next()
        .ok_or_else(|| Stop::infra("PresentMon capture has no header"))?;
    let columns = split_csv(header)?;
    let pid_column = columns
        .iter()
        .position(|column| column.trim().eq_ignore_ascii_case("ProcessID"))
        .ok_or_else(|| Stop::infra("PresentMon capture has no ProcessID column"))?;
    let mode_column = columns
        .iter()
        .position(|column| column.trim().eq_ignore_ascii_case("PresentMode"))
        .ok_or_else(|| Stop::infra("PresentMon capture has no PresentMode column"))?;
    let mut distribution: std::collections::BTreeMap<&'static str, usize> =
        std::collections::BTreeMap::new();
    let mut last_mode: Option<&'static str> = None;
    let mut count = 0;
    for line in lines {
        let fields = split_csv(line)?;
        if fields.len() < columns.len() {
            continue;
        }
        let pid: u32 = fields[pid_column]
            .trim()
            .parse()
            .map_err(|_| Stop::infra("PresentMon capture has a malformed ProcessID"))?;
        if pid != process_id {
            continue;
        }
        count += 1;
        let mode = match fields[mode_column].trim().to_ascii_lowercase().as_str() {
            "hardware: legacy flip" | "hardware: legacy copy to front buffer" => {
                "exclusiveFullscreen"
            }
            "hardware: independent flip" | "hardware composed: independent flip" => {
                "independentFlip"
            }
            "composed: flip"
            | "composed: copy with gpu gdi"
            | "composed: copy with cpu gdi"
            | "composed: composition atlas" => "composed",
            _ => "unknown",
        };
        *distribution.entry(mode).or_insert(0) += 1;
        last_mode = Some(mode);
    }
    infra_ensure!(
        count > 0,
        "PresentMon attributed no present to process {process_id}"
    );
    infra_ensure!(
        !distribution.contains_key("unknown"),
        "PresentMon reported an unclassified presentation mode: {distribution:?}"
    );
    let last_mode = last_mode.expect("count > 0 implies at least one classified mode");
    product_ensure!(
        distribution.contains_key(product_mode),
        "product mode {product_mode} was never observed by PresentMon; it saw {distribution:?}"
    );
    product_ensure!(
        last_mode == product_mode,
        "product reports mode {product_mode} but PresentMon's most recently observed present for this process was {last_mode} ({distribution:?})"
    );
    Ok(PresentMonSummary {
        total_presents: count,
        distribution,
        last_mode,
    })
}

fn split_csv(line: &str) -> Step<Vec<String>> {
    let mut fields = Vec::new();
    let mut current = String::new();
    let mut chars = line.trim_end_matches('\r').chars().peekable();
    let mut quoted = false;
    while let Some(ch) = chars.next() {
        match ch {
            '"' if quoted && chars.peek() == Some(&'"') => {
                current.push('"');
                chars.next();
            }
            '"' => quoted = !quoted,
            ',' if !quoted => {
                fields.push(std::mem::take(&mut current));
            }
            _ => current.push(ch),
        }
    }
    infra_ensure!(
        !quoted,
        "PresentMon capture contains an unterminated quoted field"
    );
    fields.push(current);
    Ok(fields)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn present_release_gates_are_required_and_missing_tool_is_unavailable() {
        for scenario in scenarios() {
            assert_eq!(scenario.tier, Tier::Required);
            assert_eq!(scenario.lane, Lane::Hardware);
        }
        assert!(matches!(
            independent_crosscheck_unavailable_reason(),
            Stop::Unavailable(_)
        ));
    }

    #[test]
    fn elevated_present_requires_decoded_mode() {
        assert!(matches!(
            judge_elevated_present(
                &serde_json::json!({"elevated":true,"available":true,"presentCount":0,"mode":"composed"})
            ),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            judge_elevated_present(
                &serde_json::json!({"elevated":true,"available":true,"presentCount":8,"mode":"unavailable"})
            ),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            judge_elevated_present(
                &serde_json::json!({"elevated":true,"available":true,"presentCount":8,"mode":"unknown"})
            ),
            Err(Stop::Fail(_))
        ));
        judge_elevated_present(&serde_json::json!({"elevated":true,"available":true,"presentCount":8,"mode":"composed"})).unwrap();
    }

    #[test]
    fn presentmon_comparison_requires_same_process_and_mode() {
        let csv = "Application,ProcessID,PresentMode\nother.exe,9,Hardware: Independent Flip\nexosnap.exe,42,Composed: Flip\n";
        compare_presentmon(csv, 42, "composed").unwrap();
        assert!(matches!(
            compare_presentmon(csv, 42, "independentFlip"),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            compare_presentmon(csv, 10, "composed"),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn homogeneous_window_requires_exact_match() {
        let composed = "ProcessID,PresentMode\n42,Composed: Flip\n42,Composed: Flip\n";
        compare_presentmon(composed, 42, "composed").unwrap();
        assert!(matches!(
            compare_presentmon(composed, 42, "independentFlip"),
            Err(Stop::Fail(_))
        ));

        let flip = "ProcessID,PresentMode\n42,Hardware Composed: Independent Flip\n";
        compare_presentmon(flip, 42, "independentFlip").unwrap();
        assert!(matches!(
            compare_presentmon(flip, 42, "composed"),
            Err(Stop::Fail(_))
        ));
    }

    #[test]
    fn real_mixed_window_is_judged_by_the_most_recently_observed_present() {
        // Reflects an actual 1127/157 split observed on hardware, scattered
        // across the whole capture, ending on a run of "Composed: Flip".
        let mixed = "ProcessID,PresentMode\n\
             42,Hardware Composed: Independent Flip\n\
             42,Composed: Flip\n\
             42,Composed: Flip\n\
             42,Hardware Composed: Independent Flip\n\
             42,Composed: Flip\n";
        let summary = compare_presentmon(mixed, 42, "composed").unwrap();
        assert_eq!(summary.total_presents, 5);
        assert_eq!(summary.distribution.get("composed"), Some(&3));
        assert_eq!(summary.distribution.get("independentFlip"), Some(&2));
        assert_eq!(summary.last_mode, "composed");

        // Mixing alone is not an infrastructure error: a mismatch against the
        // real last-observed mode is a genuine product disagreement (FAIL).
        assert!(matches!(
            compare_presentmon(mixed, 42, "independentFlip"),
            Err(Stop::Fail(_))
        ));
    }

    #[test]
    fn product_mode_never_observed_by_presentmon_fails() {
        let csv = "ProcessID,PresentMode\n42,Composed: Flip\n42,Composed: Flip\n";
        assert!(matches!(
            compare_presentmon(csv, 42, "exclusiveFullscreen"),
            Err(Stop::Fail(_))
        ));
    }

    #[test]
    fn unknown_presentmon_mode_never_passes() {
        let unknown = "ProcessID,PresentMode\n42,New Mode\n";
        assert!(matches!(
            compare_presentmon(unknown, 42, "composed"),
            Err(Stop::Infra(_))
        ));

        let mixed_with_unknown =
            "ProcessID,PresentMode\n42,Composed: Flip\n42,New Mode\n42,Composed: Flip\n";
        assert!(matches!(
            compare_presentmon(mixed_with_unknown, 42, "composed"),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn no_presents_for_pid_is_infra_not_fail() {
        let csv = "ProcessID,PresentMode\n7,Composed: Flip\n";
        assert!(matches!(
            compare_presentmon(csv, 42, "composed"),
            Err(Stop::Infra(_))
        ));
    }
}
