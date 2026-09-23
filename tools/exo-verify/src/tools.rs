//! External specialist executables and bounded process execution.
//!
//! A tool is found through exactly one override variable (`EXO_VERIFY_<NAME>`)
//! or `PATH`. Nothing is guessed from similarly named binaries elsewhere.

use anyhow::{Context, Result, bail};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::time::{Duration, Instant};

pub fn resolve(name: &str) -> Option<PathBuf> {
    let var = format!("EXO_VERIFY_{}", name.to_ascii_uppercase().replace('-', "_"));
    if let Ok(path) = std::env::var(&var) {
        let path = PathBuf::from(path);
        return path.is_file().then_some(path);
    }
    let exe = if cfg!(windows) {
        format!("{name}.exe")
    } else {
        name.to_string()
    };
    std::env::var_os("PATH").and_then(|paths| {
        std::env::split_paths(&paths)
            .map(|dir| dir.join(&exe))
            .find(|candidate| candidate.is_file())
    })
}

pub fn require(name: &str) -> Result<PathBuf> {
    resolve(name).with_context(|| {
        format!(
            "{name} is not available (set EXO_VERIFY_{} or put it on PATH)",
            name.to_ascii_uppercase().replace('-', "_")
        )
    })
}

#[derive(Debug)]
pub struct Output {
    pub status: ExitStatus,
    pub stdout: String,
    pub stderr: String,
    pub elapsed: Duration,
}

impl Output {
    pub fn success(&self) -> bool {
        self.status.success()
    }
    pub fn code(&self) -> Option<i32> {
        self.status.code()
    }
}

/// Runs `command` to completion with a hard wall-clock limit. On timeout the
/// process is killed and an error is returned; a hang is never a verdict.
pub fn run(command: &mut Command, timeout: Duration) -> Result<Output> {
    let started = Instant::now();
    let description = format!("{command:?}");
    let mut child = command
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| format!("start {description}"))?;
    let stdout = drain(child.stdout.take());
    let stderr = drain(child.stderr.take());
    let status = wait(&mut child, timeout).with_context(|| format!("{description}"))?;
    Ok(Output {
        status,
        stdout: stdout.join().unwrap_or_default(),
        stderr: stderr.join().unwrap_or_default(),
        elapsed: started.elapsed(),
    })
}

fn drain<R: Read + Send + 'static>(stream: Option<R>) -> std::thread::JoinHandle<String> {
    std::thread::spawn(move || {
        let mut text = Vec::new();
        if let Some(mut s) = stream {
            let _ = s.read_to_end(&mut text);
        }
        String::from_utf8_lossy(&text).into_owned()
    })
}

pub fn wait(child: &mut Child, timeout: Duration) -> Result<ExitStatus> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(status) = child.try_wait()? {
            return Ok(status);
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            bail!("timed out after {} s and was terminated", timeout.as_secs());
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// First line of `<tool> -version`, for the lane's tool record.
pub fn version_line(tool: &Path) -> Option<String> {
    let out = run(Command::new(tool).arg("-version"), Duration::from_secs(20)).ok()?;
    out.stdout.lines().next().map(|l| l.trim().to_string())
}
