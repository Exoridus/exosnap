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
    #[allow(dead_code, reason = "Reserved for process timing evidence")]
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
    let status = wait(&mut child, timeout).with_context(|| description.clone())?;
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

// External diagnostics are human-readable only. Preserve BOM-declared Unicode,
// but replace undecodable bytes instead of hiding the tool's original failure.
fn diagnostic_text(bytes: &[u8]) -> String {
    let utf16 = bytes
        .strip_prefix(&[0xff, 0xfe])
        .map(|body| (body, true))
        .or_else(|| bytes.strip_prefix(&[0xfe, 0xff]).map(|body| (body, false)));
    if let Some((body, little_endian)) = utf16 {
        let mut pairs = body.chunks_exact(2);
        let units: Vec<_> = pairs
            .by_ref()
            .map(|pair| {
                if little_endian {
                    u16::from_le_bytes([pair[0], pair[1]])
                } else {
                    u16::from_be_bytes([pair[0], pair[1]])
                }
            })
            .collect();
        let mut text = String::from_utf16_lossy(&units);
        if !pairs.remainder().is_empty() {
            text.push('\u{fffd}');
        }
        text
    } else {
        String::from_utf8_lossy(bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(bytes))
            .into_owned()
    }
}

pub fn read_diagnostic_log(path: &Path, tool: &str) -> Result<String> {
    let bytes = std::fs::read(path).with_context(|| {
        format!(
            "read {tool} stdout/stderr log {} for BOM-aware UTF-16LE/BE or UTF-8 replacement decoding",
            path.display()
        )
    })?;
    Ok(diagnostic_text(&bytes))
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_presentmon_redirected_stderr_utf16le_fixture() {
        let bytes = include_bytes!("../tests/fixtures/presentmon-stderr-utf16le.bin");
        assert_eq!(&bytes[..6], &[0xff, 0xfe, b'w', 0, b'a', 0]);
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("presentmon.log");
        std::fs::write(&path, bytes).unwrap();
        let decoded = read_diagnostic_log(&path, "PresentMon").unwrap();
        assert!(decoded.starts_with("warning: PresentMon requires elevated privilege"));
        assert!(decoded.contains("error: failed to start trace session: access denied.\r\n"));
        assert!(!decoded.contains(['\0', '\u{fffd}']));
        assert_eq!(
            decoded
                .encode_utf16()
                .flat_map(u16::to_le_bytes)
                .collect::<Vec<_>>(),
            bytes[2..]
        );
        assert_eq!(std::fs::read(&path).unwrap(), bytes);
    }

    #[test]
    fn reads_utf8_diagnostics_without_changing_text() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("tool.log");
        let text = "warning: Display München 東京\r\n";
        for prefix in ["", "\u{feff}"] {
            std::fs::write(&path, format!("{prefix}{text}")).unwrap();
            assert_eq!(read_diagnostic_log(&path, "tool").unwrap(), text);
        }
    }

    #[test]
    fn utf16be_diagnostics_preserve_unicode() {
        assert_eq!(
            diagnostic_text(&[0xfe, 0xff, 0, b'A', 0, 0xe4, 0x67, 0x71]),
            "Aä東"
        );
    }

    #[test]
    fn malformed_diagnostics_use_replacement_without_panicking() {
        assert_eq!(diagnostic_text(b"warning: \x80"), "warning: \u{fffd}");
        assert_eq!(diagnostic_text(&[0xff, 0xfe, b'A']), "\u{fffd}");
        assert_eq!(diagnostic_text(&[0xff, 0xfe, 0, 0xd8]), "\u{fffd}");
    }
}
