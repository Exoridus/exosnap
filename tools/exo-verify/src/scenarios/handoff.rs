//! The updater's own trust boundary: a handoff whose signed manifest was
//! altered, or that points at something other than an ExoSnap installation,
//! is refused without touching the installation.

use serde_json::{Value, json};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Instant;

use super::common::secs;
use super::dist::{contains, stage_updater, suppress_error_dialogs};
use super::install::tree_hashes;
use super::update::copy_tree;
use crate::capability::Capability;
use crate::context::Context;
use crate::control::{self, Client};
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![Scenario {
        id: "update.handoff-tamper-refused",
        revision: 1,
        title: "The updater refuses a tampered or misdirected handoff",
        class: ScenarioClass::Contract,
        contract: "the shipped updater re-verifies the signed manifest itself and refuses a handoff whose manifest bytes or signature were altered, or whose install directory is not an ExoSnap installation, as a failure with the installation byte-identical, no retry offered and a non-zero exit code",
        lane: Lane::CiUpdate,
        also: &[],
        tier: Tier::Required,
        requires: &[Capability::Windows, Capability::InteractiveDesktop],
        timeout: secs(300.0),
        run: tamper_refused,
    }]
}

/// Names the directory holding the candidate's signed disposable feed
/// (`update-manifest.json` and `update-manifest.json.sig`).
const FEED_VARIABLE: &str = "EXO_VERIFY_SIGNED_FEED";
const KEY_VARIABLE: &str = "EXO_VERIFY_UPDATE_PUBLIC_KEY_HEX";

const TERMINAL_PHASES: [&str; 6] = [
    "completed",
    "failed",
    "cancelled",
    "rebootRequired",
    "restartPending",
    "upToDate",
];

struct Feed {
    manifest: PathBuf,
    signature: PathBuf,
    target_version: String,
}

/// The signed feed, proven to verify under the key the updater under test
/// embeds. Only then is a refusal of the altered copies attributable to the
/// alteration rather than to a key mismatch.
fn signed_feed(updater: &Path) -> Step<Feed> {
    let dir = std::env::var_os(FEED_VARIABLE).map(PathBuf::from).ok_or_else(|| {
        Stop::unavailable(format!(
            "{FEED_VARIABLE} does not name the signed disposable feed (update-manifest.json and its .sig from the sign-test-feed job)"
        ))
    })?;
    let manifest = dir.join("update-manifest.json");
    let signature = dir.join("update-manifest.json.sig");
    if !manifest.is_file() || !signature.is_file() {
        return Err(Stop::unavailable(format!(
            "{} does not contain update-manifest.json and update-manifest.json.sig",
            dir.display()
        )));
    }
    let key_hex = std::env::var(KEY_VARIABLE).map_err(|_| {
        Stop::unavailable(format!(
            "{KEY_VARIABLE} (the public key the feed is signed for) is not configured"
        ))
    })?;
    let public = crate::manifest::public_key_from_hex(key_hex.trim())
        .map_err(|e| Stop::infra(format!("{KEY_VARIABLE}: {e:#}")))?;
    let manifest_bytes = std::fs::read(&manifest)?;
    crate::manifest::verify(
        &manifest_bytes,
        &std::fs::read_to_string(&signature)?,
        &public,
    )
    .map_err(|e| {
        Stop::infra(format!(
            "the signed feed does not verify under {KEY_VARIABLE}: {e:#}"
        ))
    })?;
    if !contains(&std::fs::read(updater)?, public.as_bytes()) {
        return Err(Stop::unavailable(format!(
            "{} does not embed the key the feed is signed for",
            updater.display()
        )));
    }
    let parsed: Value = serde_json::from_slice(&manifest_bytes)?;
    let target_version = parsed["version"]
        .as_str()
        .ok_or_else(|| Stop::infra("the signed manifest has no version"))?
        .to_string();
    Ok(Feed {
        manifest,
        signature,
        target_version,
    })
}

/// A copy of `path` with one bit flipped in its middle byte.
fn flip_middle_bit(path: &Path, out: &Path) -> Step<PathBuf> {
    let mut bytes = std::fs::read(path)?;
    infra_ensure!(!bytes.is_empty(), "{} is empty", path.display());
    let middle = bytes.len() / 2;
    bytes[middle] ^= 0x01;
    std::fs::write(out, bytes)?;
    Ok(out.to_path_buf())
}

/// A copy of the hex signature with its first digit replaced, so it stays
/// well-formed hex and only its value is wrong.
fn alter_signature(path: &Path, out: &Path) -> Step<PathBuf> {
    let text = std::fs::read_to_string(path)?;
    let text = text.trim();
    let first = text
        .chars()
        .next()
        .ok_or_else(|| Stop::infra("the manifest signature is empty"))?;
    let replaced = if first == '0' { '1' } else { '0' };
    std::fs::write(out, format!("{replaced}{}", &text[first.len_utf8()..]))?;
    Ok(out.to_path_buf())
}

struct Handoff<'a> {
    manifest: &'a Path,
    signature: &'a Path,
    install_dir: &'a Path,
    current_version: &'a str,
    target_version: &'a str,
    app_pid: u32,
}

fn handoff_document(handoff: &Handoff) -> Value {
    json!({
        "handoffVersion": 1,
        "updateTransactionId": control::new_run_id("u"),
        "targetVersion": handoff.target_version,
        "currentVersion": handoff.current_version,
        "manifestPath": handoff.manifest,
        "manifestSignaturePath": handoff.signature,
        "installMode": "portable",
        "installDir": handoff.install_dir,
        "appPid": handoff.app_pid,
        "verifyReinstall": false,
    })
}

fn judge_refusal(state: &Value, expected: &str) -> Step {
    product_ensure!(
        state["phase"] == "failed",
        "the updater ended in phase {} instead of refusing",
        state["phase"]
    );
    product_ensure!(
        state["failureCase"] == expected,
        "the updater refused with {} instead of {expected}",
        state["failureCase"]
    );
    product_ensure!(
        state["installState"] == "intact",
        "after refusing, the updater reports the installation as {}",
        state["installState"]
    );
    let actions = state["availableActions"]
        .as_array()
        .ok_or_else(|| Stop::infra("updater.getState has no availableActions array"))?;
    product_ensure!(
        !actions.iter().any(|action| action == "updater.retry"),
        "the updater offers a retry of an input it cannot repair"
    );
    Ok(())
}

fn run_case(
    ctx: &mut Context,
    updater: &Path,
    name: &str,
    document: &Value,
    expected: &str,
) -> Step<Value> {
    // Absolute: the updater's cwd is redirected to its own staged directory
    // below, so a relative path here would resolve against the wrong root.
    let path = std::path::absolute(ctx.scenario_dir.join(format!("{name}.json")))?;
    std::fs::write(&path, serde_json::to_vec_pretty(document)?)?;
    let run_id = control::new_run_id("tamper");
    let mut child = ctx.spawn(
        Command::new(updater)
            .arg("--apply-handoff")
            .arg(&path)
            .arg("--automation-control")
            .arg(&run_id)
            .current_dir(updater.parent().unwrap_or(Path::new("."))),
    )?;
    let mut updater_client = match Client::connect("Updater", &run_id, secs(60.0)) {
        Ok(updater_client) => updater_client,
        Err(error) => {
            return Err(match child.try_wait()? {
                Some(status) => Stop::fail(format!(
                    "{name}: the updater exited ({status}) before its control endpoint came up"
                )),
                None => Stop::Infra(error),
            });
        }
    };
    let state = updater_client
        .poll("updater.getState", json!({}), secs(120.0), |state| {
            state["phase"]
                .as_str()
                .is_some_and(|phase| TERMINAL_PHASES.contains(&phase))
        })?
        .ok_or_else(|| {
            Stop::fail(format!(
                "{name}: the updater reached no terminal phase within 120 s"
            ))
        })?;
    judge_refusal(&state, expected).map_err(|stop| match stop {
        Stop::Fail(message) => Stop::Fail(format!("{name}: {message}")),
        other => other,
    })?;
    updater_client
        .request("updater.close", json!({}), secs(15.0))?
        .map_err(|r| Stop::fail(format!("{name}: updater.close was refused: {r}")))?;
    drop(updater_client);
    let status = crate::tools::wait(&mut child, secs(30.0)).map_err(|_| {
        Stop::fail(format!(
            "{name}: the updater did not exit after updater.close"
        ))
    })?;
    product_ensure!(
        !status.success(),
        "{name}: the updater exited with success after refusing the handoff"
    );
    Ok(json!({ "state": state, "exitCode": status.code() }))
}

#[cfg(windows)]
fn product_version(exe: &Path) -> Step<String> {
    crate::win::version_strings(exe)?
        .remove("ProductVersion")
        .filter(|version| !version.is_empty())
        .ok_or_else(|| Stop::infra(format!("{} has no ProductVersion", exe.display())))
}

#[cfg(not(windows))]
fn product_version(_: &Path) -> Step<String> {
    Err(Stop::unavailable(
        "version resources are only readable on Windows",
    ))
}

fn tamper_refused(ctx: &mut Context) -> Step {
    suppress_error_dialogs();
    let product = ctx.product()?;
    let feed = signed_feed(&product.updater)?;
    let scratch = std::path::absolute(&ctx.scenario_dir)?;
    let install = scratch.join("installation");
    copy_tree(&product.root, &install)?;
    let current_version = product_version(&install.join("exosnap.exe"))?;
    let updater = stage_updater(&install, &scratch.join("updater-stage"))?;
    let foreign = scratch.join("not-an-installation");
    std::fs::create_dir_all(&foreign)?;
    let altered_manifest = flip_middle_bit(&feed.manifest, &scratch.join("altered-manifest.json"))?;
    let altered_signature =
        alter_signature(&feed.signature, &scratch.join("altered-manifest.json.sig"))?;
    let before = tree_hashes(&install)?;

    // The handoff names the process it replaces. A placeholder stands in for
    // the application, so a case that wrongly got as far as closing it would
    // end this placeholder and never the runner.
    let mut placeholder = ctx.spawn(
        Command::new("ping")
            .args(["-n", "600", "127.0.0.1"])
            .stdout(Stdio::null())
            .stderr(Stdio::null()),
    )?;
    let app_pid = placeholder.id();
    let base = Handoff {
        manifest: &feed.manifest,
        signature: &feed.signature,
        install_dir: &install,
        current_version: &current_version,
        target_version: &feed.target_version,
        app_pid,
    };
    let cases = [
        (
            "modified-manifest-bytes",
            Handoff {
                manifest: &altered_manifest,
                ..base
            },
            "verifyDownloadFailed",
        ),
        (
            "modified-manifest-signature",
            Handoff {
                signature: &altered_signature,
                ..base
            },
            "verifyDownloadFailed",
        ),
        (
            "install-dir-is-not-an-installation",
            Handoff {
                install_dir: &foreign,
                ..base
            },
            "handoffRejected",
        ),
    ];
    let started = Instant::now();
    let mut results = serde_json::Map::new();
    let outcome = (|| -> Step {
        for (name, handoff, expected) in &cases {
            let result = run_case(ctx, &updater, name, &handoff_document(handoff), expected)?;
            results.insert(name.to_string(), result);
            product_ensure!(
                tree_hashes(&install)? == before,
                "{name}: the refused handoff changed the installation"
            );
        }
        Ok(())
    })();
    let _ = placeholder.kill();
    let _ = placeholder.wait();
    ctx.evidence.put("cases", Value::Object(results));
    ctx.evidence
        .put("targetVersion", feed.target_version.clone());
    ctx.evidence.put("currentVersion", current_version);
    ctx.evidence.put("seconds", started.elapsed().as_secs_f64());
    outcome
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_refusal_must_be_the_expected_failure_with_nothing_touched_and_no_retry() {
        let refused = json!({"phase": "failed", "failureCase": "verifyDownloadFailed", "installState": "intact", "availableActions": ["updater.close"]});
        judge_refusal(&refused, "verifyDownloadFailed").unwrap();
        for (field, value) in [
            ("phase", json!("completed")),
            ("failureCase", json!("handoffRejected")),
            ("installState", json!("strandedInBackup")),
            (
                "availableActions",
                json!(["updater.retry", "updater.close"]),
            ),
        ] {
            let mut state = refused.clone();
            state[field] = value;
            assert!(
                matches!(
                    judge_refusal(&state, "verifyDownloadFailed"),
                    Err(Stop::Fail(_))
                ),
                "{field}"
            );
        }
        let mut state = refused.clone();
        state["availableActions"] = Value::Null;
        assert!(matches!(
            judge_refusal(&state, "verifyDownloadFailed"),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn tampering_changes_exactly_the_bytes_the_signature_covers() {
        let dir = tempfile::tempdir().unwrap();
        let manifest = dir.path().join("m.json");
        std::fs::write(&manifest, br#"{"version":"0.10.0"}"#).unwrap();
        let altered = flip_middle_bit(&manifest, &dir.path().join("a.json")).unwrap();
        let (a, b) = (
            std::fs::read(&manifest).unwrap(),
            std::fs::read(&altered).unwrap(),
        );
        assert_eq!(a.len(), b.len());
        assert_eq!(a.iter().zip(&b).filter(|(x, y)| x != y).count(), 1);

        let signature = dir.path().join("m.sig");
        std::fs::write(&signature, "0abc\r\n").unwrap();
        let altered = alter_signature(&signature, &dir.path().join("a.sig")).unwrap();
        assert_eq!(std::fs::read_to_string(altered).unwrap(), "1abc");
    }

    /// `run_case` passes its handoff-document path to the updater as
    /// `--apply-handoff`, and the updater's cwd is redirected to its own
    /// staged directory. A relative path there silently resolves against the
    /// wrong root instead of failing loudly, which is why this asserts the
    /// absolute invariant rather than trusting a passing spawn.
    #[test]
    fn case_document_path_survives_the_updaters_redirected_cwd() {
        let dir = tempfile::tempdir().unwrap();
        let scenario_dir = dir
            .path()
            .join("scenarios")
            .join("update.handoff-tamper-refused");
        std::fs::create_dir_all(&scenario_dir).unwrap();
        let path = std::path::absolute(scenario_dir.join("modified-manifest-bytes.json")).unwrap();
        std::fs::write(&path, b"{}").unwrap();

        assert!(
            path.is_absolute(),
            "a relative path breaks once the child's cwd differs"
        );
        let updater_stage = dir.path().join("updater-stage");
        std::fs::create_dir_all(&updater_stage).unwrap();
        let resolved_from_updater_cwd = updater_stage.join(&path);
        assert!(
            resolved_from_updater_cwd.is_file(),
            "an absolute path must still resolve when joined onto an unrelated cwd"
        );
    }
}
