//! exo-guest: the verification agent baked into a sealed Hyper-V base image.

use anyhow::{Context, Result, bail};
use clap::{Parser, Subcommand};
use std::process::ExitCode;

#[derive(Parser)]
#[command(
    name = "exo-guest",
    version,
    about = "ExoSnap verification guest agent"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Listen on the Hyper-V socket service and serve the host, forever.
    Serve,
    /// Register the agent to start in the interactive session at every logon.
    /// Run once while provisioning the base image.
    Install,
    /// Remove the logon registration.
    Uninstall,
}

const TASK_NAME: &str = "ExoSnap Verification Guest Agent";

fn schtasks(args: &[&str]) -> Result<()> {
    let status = std::process::Command::new("schtasks.exe")
        .args(args)
        .status()
        .context("run schtasks.exe")?;
    if !status.success() {
        bail!("schtasks {args:?} exited with {status}");
    }
    Ok(())
}

#[cfg(windows)]
fn serve() -> Result<()> {
    use exo_guest::agent::{Agent, PowerAction};
    use exo_guest::hvsock::{HvListener, parse_guid};
    let listener = HvListener::bind(parse_guid(exo_guest::SERVICE_ID)?)?;
    let mut agent = Agent::default();
    loop {
        let mut stream = match listener.accept() {
            Ok(stream) => stream,
            Err(e) => {
                eprintln!("exo-guest: accept failed: {e:#}");
                continue;
            }
        };
        match agent.serve(&mut stream) {
            Ok(Some(action)) => {
                drop(stream);
                let flag = match action {
                    PowerAction::Reboot => "/r",
                    PowerAction::Shutdown => "/s",
                };
                let _ = std::process::Command::new("shutdown.exe")
                    .args([flag, "/t", "0", "/f"])
                    .status();
            }
            Ok(None) => {}
            Err(e) => eprintln!("exo-guest: connection ended: {e:#}"),
        }
    }
}

#[cfg(not(windows))]
fn serve() -> Result<()> {
    bail!("the guest agent runs only on Windows")
}

fn main() -> ExitCode {
    let result = match Cli::parse().command {
        Command::Serve => serve(),
        Command::Install => std::env::current_exe()
            .context("locate exo-guest.exe")
            .and_then(|exe| {
                let action = format!("\"{}\" serve", exe.display());
                schtasks(&[
                    "/Create", "/F", "/TN", TASK_NAME, "/SC", "ONLOGON", "/RL", "HIGHEST", "/TR",
                    &action,
                ])
            }),
        Command::Uninstall => schtasks(&["/Delete", "/F", "/TN", TASK_NAME]),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("exo-guest: {e:#}");
            ExitCode::FAILURE
        }
    }
}
