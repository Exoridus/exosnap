//! Windows Sandbox through its `wsb` command line.
//!
//! `wsb exec` returns only an exit code, never the command's output, so the
//! run directory is shared into the sandbox and everything the guest produces
//! (transcript, lane result, evidence) is written there.

use anyhow::{Context as _, Result, bail};
use std::process::Command;
use std::time::{Duration, Instant};

use super::{BackendKind, DisposableWindows, GUEST_ROOT, GuestCommand, RunDir};
use crate::tools;

pub fn available() -> bool {
    tools::resolve("wsb").is_some()
}

pub struct Sandbox {
    id: String,
    started: bool,
}

impl Sandbox {
    pub fn new() -> Self {
        Sandbox {
            id: guid(),
            started: false,
        }
    }

    fn wsb(&self, args: &[&str], timeout: Duration) -> Result<tools::Output> {
        let mut command = Command::new(tools::require("wsb")?);
        command.args(args);
        tools::run(&mut command, timeout)
    }
}

impl Default for Sandbox {
    fn default() -> Self {
        Self::new()
    }
}

/// A fresh identifier for the sandbox, so the host addresses exactly the
/// instance it started and never one the developer has open.
fn guid() -> String {
    let hex = crate::bundle::sha256_bytes(crate::control::new_run_id("wsb").as_bytes());
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

/// The sandbox configuration. vGPU stays on so D3D11 capture paths can be
/// probed; whether they work is the guest probe's answer, not an assumption.
pub fn config(host_dir: &str) -> String {
    format!(
        "<Configuration>\
<VGpu>Enable</VGpu>\
<Networking>Default</Networking>\
<ClipboardRedirection>Disable</ClipboardRedirection>\
<PrinterRedirection>Disable</PrinterRedirection>\
<MappedFolders><MappedFolder>\
<HostFolder>{}</HostFolder>\
<SandboxFolder>{GUEST_ROOT}</SandboxFolder>\
<ReadOnly>false</ReadOnly>\
</MappedFolder></MappedFolders>\
</Configuration>",
        xml_escape(host_dir)
    )
}

fn xml_escape(text: &str) -> String {
    text.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

/// Quotes one argument for the command line `wsb exec` hands to `cmd /c`.
fn quote(arg: &str) -> String {
    if !arg.is_empty() && !arg.contains([' ', '\t', '"', '&', '|', '<', '>', '^']) {
        return arg.to_string();
    }
    format!("\"{}\"", arg.replace('"', "\\\""))
}

/// The exit code `wsb exec --raw` reports for the guest command.
fn exit_code(output: &tools::Output) -> Option<i32> {
    serde_json::from_str::<serde_json::Value>(output.stdout.trim())
        .ok()
        .and_then(|v| {
            ["ExitCode", "exitCode", "exit_code"]
                .iter()
                .find_map(|k| v.get(k).and_then(serde_json::Value::as_i64))
        })
        .map(|c| c as i32)
        .or_else(|| output.code())
}

impl DisposableWindows for Sandbox {
    fn kind(&self) -> BackendKind {
        BackendKind::Sandbox
    }

    fn prepare(&mut self, run: &RunDir) -> Result<()> {
        let host = run.host.canonicalize()?;
        std::fs::write(
            run.host.join("sandbox.wsb"),
            config(&host.to_string_lossy().replace(r"\\?\", "")),
        )?;
        Ok(())
    }

    fn start(&mut self, run: &RunDir) -> Result<()> {
        let config = std::fs::read_to_string(run.host.join("sandbox.wsb"))?;
        let out = self.wsb(
            &["start", "--id", &self.id, "--raw", "--config", &config],
            Duration::from_secs(300),
        )?;
        if !out.success() {
            bail!(
                "wsb start failed ({:?}): {}{}",
                out.code(),
                out.stdout.trim(),
                out.stderr.trim()
            );
        }
        self.started = true;
        // The instance accepts commands only once its user session exists.
        let deadline = Instant::now() + Duration::from_secs(300);
        loop {
            let probe = self.wsb(
                &[
                    "exec",
                    "--id",
                    &self.id,
                    "--raw",
                    "-r",
                    "ExistingLogin",
                    "-c",
                    "cmd.exe /c exit 0",
                ],
                Duration::from_secs(60),
            );
            if matches!(&probe, Ok(out) if exit_code(out) == Some(0)) {
                return Ok(());
            }
            if Instant::now() >= deadline {
                bail!("the sandbox session did not become ready within 300 s");
            }
            std::thread::sleep(Duration::from_secs(3));
        }
    }

    fn execute(&mut self, command: &GuestCommand) -> Result<i32> {
        let mut line = quote(&command.program);
        for arg in &command.args {
            line.push(' ');
            line.push_str(&quote(arg));
        }
        let wrapped = format!(r#"cmd.exe /c "{line} > {GUEST_ROOT}\transcript.log 2>&1""#);
        let out = self
            .wsb(
                &[
                    "exec",
                    "--id",
                    &self.id,
                    "--raw",
                    "-r",
                    "ExistingLogin",
                    "-c",
                    &wrapped,
                ],
                command.timeout,
            )
            .context("run the lane inside the sandbox")?;
        exit_code(&out).context("wsb exec reported no exit code")
    }

    fn collect(&mut self, _run: &RunDir) -> Result<()> {
        // The run directory is the mapped folder; the guest wrote in place.
        Ok(())
    }

    fn stop(&mut self) -> Result<()> {
        if self.started {
            let out = self.wsb(
                &["stop", "--id", &self.id, "--raw"],
                Duration::from_secs(120),
            )?;
            if !out.success() {
                bail!(
                    "wsb stop failed: {}{}",
                    out.stdout.trim(),
                    out.stderr.trim()
                );
            }
            self.started = false;
        }
        Ok(())
    }

    fn destroy(&mut self) -> Result<()> {
        // A stopped sandbox leaves nothing behind; stopping is destroying.
        self.stop()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_run_directory_is_mapped_writable_at_the_guest_root() {
        let xml = config(r"C:\runs\a&b");
        assert!(xml.contains(r"<HostFolder>C:\runs\a&amp;b</HostFolder>"));
        assert!(xml.contains(r"<SandboxFolder>C:\ExoVerify</SandboxFolder>"));
        assert!(xml.contains("<ReadOnly>false</ReadOnly>"));
    }

    #[test]
    fn arguments_with_spaces_are_quoted() {
        assert_eq!(quote("run"), "run");
        assert_eq!(quote(r"C:\Program Files\x"), r#""C:\Program Files\x""#);
    }

    #[test]
    fn each_sandbox_gets_its_own_guid() {
        let a = guid();
        assert_ne!(a, guid());
        assert_eq!(
            a.split('-').map(str::len).collect::<Vec<_>>(),
            [8, 4, 4, 4, 12]
        );
    }
}
