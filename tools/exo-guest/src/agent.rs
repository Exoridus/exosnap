//! Request handling inside the guest. Transport-independent, so it is tested
//! against in-memory streams.

use anyhow::{Context, Result, bail};
use base64::Engine as _;
use serde_json::{Value, json};
use std::collections::{BTreeMap, HashMap};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::process::{Child, Command, Stdio};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::frame;
use crate::protocol::{
    DirEntry, Envelope, ExecResult, MAX_CAPTURE_BYTES, MAX_FRAME_BYTES, PROTOCOL_VERSION, Request,
    Response,
};

/// A reboot or shutdown the host asked for. The connection loop returns it
/// so the caller can act after the response has been delivered.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PowerAction {
    Reboot,
    Shutdown,
}

#[derive(Default)]
pub struct Agent {
    children: HashMap<u64, Child>,
    next_handle: u64,
    greeted: bool,
}

impl Agent {
    /// Serves one connection until the peer closes it or requests a power
    /// action. Spawned processes outlive the connection so a reconnecting
    /// host can still wait for them.
    pub fn serve(&mut self, stream: &mut (impl Read + Write)) -> Result<Option<PowerAction>> {
        self.greeted = false;
        while let Some(envelope) = frame::read::<Envelope>(stream)? {
            let power = match envelope.request {
                Request::Reboot => Some(PowerAction::Reboot),
                Request::Shutdown => Some(PowerAction::Shutdown),
                _ => None,
            };
            let response = match self.handle(envelope.request) {
                Ok(result) => Response::ok(envelope.id, result),
                Err(e) => Response::err(envelope.id, format!("{e:#}")),
            };
            let failed = !response.ok;
            frame::write(stream, &response)?;
            if power.is_some() && !failed {
                return Ok(power);
            }
        }
        Ok(None)
    }

    pub fn handle(&mut self, request: Request) -> Result<Value> {
        if !self.greeted && !matches!(request, Request::Hello { .. }) {
            bail!("the first request of a connection must be hello");
        }
        match request {
            Request::Hello { protocol, nonce } => {
                if protocol != PROTOCOL_VERSION {
                    bail!("host speaks protocol {protocol}, agent speaks {PROTOCOL_VERSION}");
                }
                self.greeted = true;
                Ok(json!({
                    "protocol": PROTOCOL_VERSION,
                    "nonce": nonce,
                    "agentVersion": env!("CARGO_PKG_VERSION"),
                }))
            }
            Request::Capabilities => Ok(json!({
                "os": std::env::consts::OS,
                "arch": std::env::consts::ARCH,
                "computerName": std::env::var("COMPUTERNAME").ok(),
                "session": session_info(),
            })),
            Request::Exec {
                program,
                args,
                cwd,
                env,
                timeout_ms,
            } => Ok(serde_json::to_value(exec(
                &program,
                &args,
                cwd.as_deref(),
                &env,
                Duration::from_millis(timeout_ms),
            )?)?),
            Request::Spawn {
                program,
                args,
                cwd,
                env,
            } => {
                let child = command(&program, &args, cwd.as_deref(), &env)
                    .stdin(Stdio::null())
                    .stdout(Stdio::null())
                    .stderr(Stdio::null())
                    .spawn()
                    .with_context(|| format!("spawn {program}"))?;
                self.next_handle += 1;
                let pid = child.id();
                self.children.insert(self.next_handle, child);
                Ok(json!({"handle": self.next_handle, "pid": pid}))
            }
            Request::Wait { handle, timeout_ms } => {
                let child = self
                    .children
                    .get_mut(&handle)
                    .with_context(|| format!("no process with handle {handle}"))?;
                let deadline = Instant::now() + Duration::from_millis(timeout_ms);
                loop {
                    if let Some(status) = child.try_wait()? {
                        self.children.remove(&handle);
                        return Ok(json!({"exited": true, "exitCode": status.code()}));
                    }
                    if Instant::now() >= deadline {
                        return Ok(json!({"exited": false}));
                    }
                    std::thread::sleep(Duration::from_millis(50));
                }
            }
            Request::Kill { handle } => {
                let mut child = self
                    .children
                    .remove(&handle)
                    .with_context(|| format!("no process with handle {handle}"))?;
                let _ = child.kill();
                let status = child.wait()?;
                Ok(json!({"exitCode": status.code()}))
            }
            Request::ReadFile {
                path,
                offset,
                max_len,
            } => {
                let limit = max_len.min((MAX_FRAME_BYTES / 2) as u64);
                let mut file =
                    std::fs::File::open(&path).with_context(|| format!("open {path}"))?;
                let total = file.metadata()?.len();
                file.seek(SeekFrom::Start(offset))?;
                let mut data = Vec::new();
                file.take(limit).read_to_end(&mut data)?;
                let end = offset + data.len() as u64;
                Ok(json!({
                    "data": base64::engine::general_purpose::STANDARD.encode(&data),
                    "totalLen": total,
                    "eof": end >= total,
                }))
            }
            Request::WriteFile { path, data, append } => {
                let bytes = base64::engine::general_purpose::STANDARD
                    .decode(data)
                    .context("data is not base64")?;
                if let Some(parent) = Path::new(&path).parent() {
                    std::fs::create_dir_all(parent)?;
                }
                let mut file = std::fs::OpenOptions::new()
                    .create(true)
                    .write(true)
                    .append(append)
                    .truncate(!append)
                    .open(&path)
                    .with_context(|| format!("open {path} for writing"))?;
                file.write_all(&bytes)?;
                Ok(json!({"written": bytes.len()}))
            }
            Request::Exists { path } => Ok(match std::fs::metadata(&path) {
                Ok(m) => json!({"exists": true, "isDir": m.is_dir(), "len": m.len()}),
                Err(_) => json!({"exists": false}),
            }),
            Request::ListDir { path } => {
                let mut entries = Vec::new();
                for entry in std::fs::read_dir(&path).with_context(|| format!("list {path}"))? {
                    let entry = entry?;
                    let meta = entry.metadata()?;
                    entries.push(DirEntry {
                        name: entry.file_name().to_string_lossy().into_owned(),
                        is_dir: meta.is_dir(),
                        len: meta.len(),
                    });
                }
                entries.sort_by(|a, b| a.name.cmp(&b.name));
                Ok(serde_json::to_value(entries)?)
            }
            Request::SessionInfo => Ok(session_info()),
            Request::GateReady => {
                let info = session_info();
                let ready = info["interactive"].as_bool().unwrap_or(false);
                Ok(json!({"ready": ready, "session": info}))
            }
            Request::Reboot | Request::Shutdown => Ok(json!({"accepted": true})),
        }
    }
}

fn command(
    program: &str,
    args: &[String],
    cwd: Option<&str>,
    env: &BTreeMap<String, String>,
) -> Command {
    let mut command = Command::new(program);
    command.args(args).envs(env);
    if let Some(dir) = cwd {
        command.current_dir(dir);
    }
    command
}

fn capture(mut source: impl Read + Send + 'static) -> JoinHandle<(Vec<u8>, bool)> {
    std::thread::spawn(move || {
        let mut kept = Vec::new();
        let mut truncated = false;
        let mut chunk = [0u8; 8192];
        while let Ok(n) = source.read(&mut chunk) {
            if n == 0 {
                break;
            }
            let room = MAX_CAPTURE_BYTES.saturating_sub(kept.len());
            kept.extend_from_slice(&chunk[..n.min(room)]);
            truncated |= n > room;
        }
        (kept, truncated)
    })
}

pub fn exec(
    program: &str,
    args: &[String],
    cwd: Option<&str>,
    env: &BTreeMap<String, String>,
    timeout: Duration,
) -> Result<ExecResult> {
    let started = Instant::now();
    let mut child = command(program, args, cwd, env)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| format!("spawn {program}"))?;
    let out = capture(child.stdout.take().unwrap());
    let err = capture(child.stderr.take().unwrap());
    let status = loop {
        if let Some(status) = child.try_wait()? {
            break status;
        }
        if started.elapsed() >= timeout {
            let _ = child.kill();
            let _ = child.wait();
            bail!("{program} did not finish within {} ms", timeout.as_millis());
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    let (stdout, out_truncated) = out.join().unwrap_or_default();
    let (stderr, err_truncated) = err.join().unwrap_or_default();
    Ok(ExecResult {
        exit_code: status.code(),
        stdout: String::from_utf8_lossy(&stdout).into_owned(),
        stderr: String::from_utf8_lossy(&stderr).into_owned(),
        truncated: out_truncated || err_truncated,
        elapsed_ms: started.elapsed().as_millis() as u64,
    })
}

#[cfg(windows)]
pub fn session_info() -> Value {
    use windows::Win32::System::RemoteDesktop::{
        ProcessIdToSessionId, WTSGetActiveConsoleSessionId,
    };
    use windows::Win32::System::StationsAndDesktops::{
        CloseDesktop, DESKTOP_ACCESS_FLAGS, DESKTOP_CONTROL_FLAGS, OpenInputDesktop,
    };
    use windows::Win32::System::Threading::GetCurrentProcessId;
    let mut session = 0u32;
    let has_session = unsafe { ProcessIdToSessionId(GetCurrentProcessId(), &mut session) }.is_ok();
    let console = unsafe { WTSGetActiveConsoleSessionId() };
    let input_desktop = unsafe {
        OpenInputDesktop(
            DESKTOP_CONTROL_FLAGS(0),
            false,
            DESKTOP_ACCESS_FLAGS(0x0100),
        )
        .map(|desktop| {
            let _ = CloseDesktop(desktop);
        })
        .is_ok()
    };
    json!({
        "sessionId": has_session.then_some(session),
        "consoleSessionId": console,
        "user": std::env::var("USERNAME").ok(),
        "interactive": has_session && session != 0 && input_desktop,
    })
}

#[cfg(not(windows))]
pub fn session_info() -> Value {
    json!({"sessionId": null, "interactive": false})
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Duplex {
        input: std::io::Cursor<Vec<u8>>,
        output: Vec<u8>,
    }
    impl Read for Duplex {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            self.input.read(buf)
        }
    }
    impl Write for Duplex {
        fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
            self.output.write(buf)
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    fn converse(requests: Vec<Request>) -> (Vec<Response>, Option<PowerAction>) {
        let mut input = Vec::new();
        for (i, request) in requests.into_iter().enumerate() {
            frame::write(
                &mut input,
                &Envelope {
                    id: i as u64,
                    request,
                },
            )
            .unwrap();
        }
        let mut duplex = Duplex {
            input: std::io::Cursor::new(input),
            output: Vec::new(),
        };
        let power = Agent::default().serve(&mut duplex).unwrap();
        let mut cursor = std::io::Cursor::new(duplex.output);
        let mut responses = Vec::new();
        while let Some(r) = frame::read::<Response>(&mut cursor).unwrap() {
            responses.push(r);
        }
        (responses, power)
    }

    fn hello() -> Request {
        Request::Hello {
            protocol: PROTOCOL_VERSION,
            nonce: "n1".into(),
        }
    }

    #[test]
    fn requests_before_hello_are_refused() {
        let (responses, _) = converse(vec![Request::SessionInfo, hello(), Request::SessionInfo]);
        assert!(!responses[0].ok);
        assert!(responses[1].ok);
        assert_eq!(responses[1].result["nonce"], "n1");
        assert!(responses[2].ok);
    }

    #[test]
    fn a_protocol_mismatch_is_refused() {
        let (responses, _) = converse(vec![Request::Hello {
            protocol: PROTOCOL_VERSION + 1,
            nonce: String::new(),
        }]);
        assert!(!responses[0].ok);
    }

    #[test]
    fn files_round_trip_and_list() {
        let dir = std::env::temp_dir().join(format!("exo-guest-test-{}", std::process::id()));
        let file = dir.join("sub").join("a.txt");
        let path = file.to_string_lossy().into_owned();
        let data = base64::engine::general_purpose::STANDARD.encode(b"hello");
        let (responses, _) = converse(vec![
            hello(),
            Request::WriteFile {
                path: path.clone(),
                data,
                append: false,
            },
            Request::ReadFile {
                path: path.clone(),
                offset: 1,
                max_len: 3,
            },
            Request::Exists { path },
            Request::ListDir {
                path: dir.join("sub").to_string_lossy().into_owned(),
            },
        ]);
        let _ = std::fs::remove_dir_all(&dir);
        assert!(responses.iter().all(|r| r.ok), "{responses:?}");
        let read = base64::engine::general_purpose::STANDARD
            .decode(responses[2].result["data"].as_str().unwrap())
            .unwrap();
        assert_eq!(read, b"ell");
        assert_eq!(responses[2].result["eof"], false);
        assert_eq!(responses[3].result["len"], 5);
        assert_eq!(responses[4].result[0]["name"], "a.txt");
    }

    #[test]
    fn exec_reports_exit_code_and_output() {
        let (program, args) = if cfg!(windows) {
            ("cmd", vec!["/c".to_string(), "echo hi& exit 3".to_string()])
        } else {
            ("sh", vec!["-c".to_string(), "echo hi; exit 3".to_string()])
        };
        let result = exec(
            program,
            &args,
            None,
            &BTreeMap::new(),
            Duration::from_secs(30),
        )
        .unwrap();
        assert_eq!(result.exit_code, Some(3));
        assert!(result.stdout.contains("hi"));
    }

    #[test]
    fn a_power_request_ends_the_connection_after_its_response() {
        let (responses, power) = converse(vec![hello(), Request::Reboot, Request::SessionInfo]);
        assert_eq!(responses.len(), 2);
        assert_eq!(power, Some(PowerAction::Reboot));
    }
}
