//! Client for the product control channel: one JSON object per line over a
//! local named pipe (`\\.\pipe\ExoSnap.<Role>.<run-id>`), protocol 2.
//!
//! The client never reconnects on its own. A vanished endpoint can be the very
//! fact a scenario observes (an update replacing the process), so it surfaces
//! as an error and the scenario decides what it means.

use anyhow::{Context, Result, anyhow, bail};
use serde_json::{Map, Value, json};
use std::collections::VecDeque;
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::time::{Duration, Instant};

pub const PROTOCOL: u64 = 2;

/// The oldest protocol the product still answers.
pub const PROTOCOL_V1: u64 = 1;

pub fn pipe_name(role: &str, run_id: &str) -> String {
    format!(r"\\.\pipe\ExoSnap.{role}.{run_id}")
}

/// A fresh unpredictable run id within the product's accepted alphabet.
pub fn new_run_id(prefix: &str) -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let entropy = crate::bundle::sha256_bytes(
        format!("{nanos}-{}-{:p}", std::process::id(), &nanos).as_bytes(),
    );
    format!("{prefix}-{}", &entropy[..16])
}

/// A refused or failed command, with the protocol's structured reason.
#[derive(Debug, Clone)]
pub struct Refusal {
    pub code: String,
    pub message: String,
    pub requires: Value,
    pub actual: Value,
}

impl std::fmt::Display for Refusal {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.code, self.message)
    }
}

pub struct Client {
    pipe: pipe::Pipe,
    lines: Receiver<Result<String, String>>,
    events: VecDeque<Value>,
    next_id: u64,
    protocol: u64,
    pub state_revision: u64,
    pub transcript: Vec<Value>,
    pub identity: Value,
    /// The whole envelope of the last response, for protocol-shape checks.
    pub last_response: Value,
}

impl Client {
    /// Connects and performs the handshake, retrying the connection (not the
    /// handshake) until `timeout`: the endpoint appears once the process is up.
    pub fn connect(role: &str, run_id: &str, timeout: Duration) -> Result<Client> {
        Self::connect_protocol(role, run_id, timeout, PROTOCOL)
    }

    pub fn connect_protocol(
        role: &str,
        run_id: &str,
        timeout: Duration,
        protocol: u64,
    ) -> Result<Client> {
        let name = pipe_name(role, run_id);
        let deadline = Instant::now() + timeout;
        let pipe = loop {
            match pipe::Pipe::open(&name) {
                Ok(p) => break p,
                Err(e) if Instant::now() < deadline => {
                    let _ = e;
                    std::thread::sleep(Duration::from_millis(200));
                }
                Err(e) => return Err(e).with_context(|| format!("connect {name}")),
            }
        };
        let (tx, rx) = mpsc::channel();
        pipe.spawn_reader(tx);
        let mut client = Client {
            pipe,
            lines: rx,
            events: VecDeque::new(),
            next_id: 1,
            protocol,
            state_revision: 0,
            transcript: Vec::new(),
            identity: Value::Null,
            last_response: Value::Null,
        };
        let hello = client
            .request(
                "system.hello",
                json!({ "runId": run_id }),
                Duration::from_secs(15),
            )?
            .map_err(|r| anyhow!("handshake refused: {r}"))?;
        if hello.get("protocol").and_then(Value::as_u64) != Some(protocol) {
            bail!(
                "endpoint answered protocol {:?}, client speaks {protocol}",
                hello.get("protocol")
            );
        }
        client.identity = hello;
        Ok(client)
    }

    fn read_object(&mut self, until: Instant) -> Result<Option<Value>> {
        loop {
            let remaining = until.saturating_duration_since(Instant::now());
            match self.lines.recv_timeout(remaining) {
                Ok(Ok(line)) => {
                    let Ok(value) = serde_json::from_str::<Value>(&line) else {
                        continue;
                    };
                    if let Some(rev) = value.get("stateRevision").and_then(Value::as_u64) {
                        self.state_revision = self.state_revision.max(rev);
                    }
                    self.transcript.push(json!({ "in": value.clone() }));
                    return Ok(Some(value));
                }
                Ok(Err(error)) => bail!("control pipe closed: {error}"),
                Err(RecvTimeoutError::Timeout) => return Ok(None),
                Err(RecvTimeoutError::Disconnected) => bail!("control pipe closed"),
            }
        }
    }

    /// Sends a command and waits for its response. Events that arrive in the
    /// meantime are kept for `wait_event`.
    pub fn request(
        &mut self,
        command: &str,
        params: Value,
        timeout: Duration,
    ) -> Result<Result<Value, Refusal>> {
        let id = self.next_id.to_string();
        self.next_id += 1;
        let request =
            json!({ "protocol": self.protocol, "id": id, "command": command, "params": params });
        self.transcript.push(json!({ "out": request.clone() }));
        let mut line = serde_json::to_vec(&request)?;
        line.push(b'\n');
        self.pipe
            .write_all(&line)
            .with_context(|| format!("send {command}"))?;
        let deadline = Instant::now() + timeout;
        loop {
            let Some(object) = self.read_object(deadline)? else {
                bail!("no response to {command} within {} s", timeout.as_secs());
            };
            if object.get("event").is_some() {
                self.events.push_back(object);
                continue;
            }
            if object.get("id").and_then(Value::as_str) != Some(id.as_str()) {
                continue;
            }
            self.last_response = object.clone();
            if object.get("ok").and_then(Value::as_bool) == Some(true) {
                return Ok(Ok(object
                    .get("result")
                    .cloned()
                    .unwrap_or(Value::Object(Map::new()))));
            }
            let error = object.get("error").cloned().unwrap_or_default();
            return Ok(Err(Refusal {
                code: error
                    .get("code")
                    .and_then(Value::as_str)
                    .unwrap_or("unknown")
                    .to_string(),
                message: error
                    .get("message")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string(),
                requires: error.get("requires").cloned().unwrap_or(Value::Null),
                actual: error.get("actual").cloned().unwrap_or(Value::Null),
            }));
        }
    }

    /// A command the scenario expects to be accepted. A refusal is returned as
    /// an error carrying the protocol's reason; the caller decides whether that
    /// is a product failure or a harness mistake.
    pub fn call(&mut self, command: &str, params: Value) -> Result<Value> {
        self.request(command, params, Duration::from_secs(30))?
            .map_err(|r| {
                anyhow!(
                    "{command} refused: {r} (requires {}, actual {})",
                    r.requires,
                    r.actual
                )
            })
    }

    /// Waits for an event whose `data` matches every field of `filter`.
    #[allow(dead_code, reason = "Reserved for event-driven scenarios")]
    pub fn wait_event(
        &mut self,
        name: &str,
        filter: &Value,
        timeout: Duration,
    ) -> Result<Option<Value>> {
        let matches = |event: &Value| {
            event.get("event").and_then(Value::as_str) == Some(name)
                && filter.as_object().is_none_or(|f| {
                    f.iter()
                        .all(|(k, v)| event.get("data").and_then(|d| d.get(k)) == Some(v))
                })
        };
        if let Some(pos) = self.events.iter().position(matches) {
            return Ok(self.events.remove(pos));
        }
        let deadline = Instant::now() + timeout;
        while let Some(object) = self.read_object(deadline)? {
            if object.get("event").is_none() {
                continue;
            }
            if matches(&object) {
                return Ok(Some(object));
            }
            self.events.push_back(object);
        }
        Ok(None)
    }

    /// Stops reading the pipe while keeping the connection open: the server
    /// then holds unread output for a client that never drains it.
    pub fn stop_reading(&self) {
        self.pipe.cancel();
    }

    /// Polls `command` until `predicate` holds on its result. For fields that
    /// have no event; the interval is a poll cadence, not a settle sleep.
    pub fn poll(
        &mut self,
        command: &str,
        params: Value,
        timeout: Duration,
        mut predicate: impl FnMut(&Value) -> bool,
    ) -> Result<Option<Value>> {
        let deadline = Instant::now() + timeout;
        loop {
            let value = self.call(command, params.clone())?;
            if predicate(&value) {
                return Ok(Some(value));
            }
            if Instant::now() >= deadline {
                return Ok(None);
            }
            std::thread::sleep(Duration::from_millis(100));
        }
    }
}

#[cfg(windows)]
mod pipe {
    use anyhow::{Result, bail};
    use std::sync::Arc;
    use std::sync::mpsc::Sender;
    use windows::Win32::Foundation::{
        CloseHandle, ERROR_IO_PENDING, GENERIC_READ, GENERIC_WRITE, HANDLE,
    };
    use windows::Win32::Storage::FileSystem::{
        CreateFileW, FILE_FLAG_OVERLAPPED, FILE_SHARE_NONE, OPEN_EXISTING, ReadFile, WriteFile,
    };
    use windows::Win32::System::IO::{GetOverlappedResult, OVERLAPPED};
    use windows::Win32::System::Threading::CreateEventW;
    use windows::core::HSTRING;

    struct Handle(HANDLE);
    unsafe impl Send for Handle {}
    unsafe impl Sync for Handle {}
    impl Drop for Handle {
        fn drop(&mut self) {
            unsafe {
                let _ = CloseHandle(self.0);
            }
        }
    }

    /// An overlapped pipe handle: a blocked read on the reader thread must not
    /// serialize writes, which is what a synchronous handle would do.
    pub struct Pipe {
        handle: Arc<Handle>,
    }

    fn overlapped_io(
        handle: HANDLE,
        op: impl FnOnce(*mut OVERLAPPED) -> windows::core::Result<()>,
    ) -> windows::core::Result<u32> {
        unsafe {
            let event = CreateEventW(None, true, false, None)?;
            let mut overlapped = OVERLAPPED {
                hEvent: event,
                ..Default::default()
            };
            let started = op(&mut overlapped);
            let result = match started {
                Ok(()) => Ok(()),
                Err(e) if e.code() == ERROR_IO_PENDING.to_hresult() => Ok(()),
                Err(e) => Err(e),
            };
            let outcome = result.and_then(|_| {
                let mut transferred = 0u32;
                GetOverlappedResult(handle, &overlapped, &mut transferred, true)
                    .map(|_| transferred)
            });
            let _ = CloseHandle(event);
            outcome
        }
    }

    impl Pipe {
        pub fn open(name: &str) -> Result<Pipe> {
            let handle = unsafe {
                CreateFileW(
                    &HSTRING::from(name),
                    (GENERIC_READ | GENERIC_WRITE).0,
                    FILE_SHARE_NONE,
                    None,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    None,
                )?
            };
            Ok(Pipe {
                handle: Arc::new(Handle(handle)),
            })
        }

        pub fn write_all(&self, bytes: &[u8]) -> Result<()> {
            let mut offset = 0;
            while offset < bytes.len() {
                let chunk = &bytes[offset..];
                let written = overlapped_io(self.handle.0, |o| unsafe {
                    WriteFile(self.handle.0, Some(chunk), None, Some(o))
                })?;
                if written == 0 {
                    bail!("pipe write made no progress");
                }
                offset += written as usize;
            }
            Ok(())
        }

        pub fn cancel(&self) {
            unsafe {
                let _ = windows::Win32::System::IO::CancelIoEx(self.handle.0, None);
            }
        }

        pub fn spawn_reader(&self, tx: Sender<Result<String, String>>) {
            let handle = Arc::clone(&self.handle);
            std::thread::spawn(move || {
                let mut buffer = vec![0u8; 64 * 1024];
                let mut partial: Vec<u8> = Vec::new();
                loop {
                    let read = overlapped_io(handle.0, |o| unsafe {
                        ReadFile(handle.0, Some(&mut buffer), None, Some(o))
                    });
                    match read {
                        Ok(0) => {
                            let _ = tx.send(Err("end of stream".into()));
                            return;
                        }
                        Ok(n) => {
                            partial.extend_from_slice(&buffer[..n as usize]);
                            while let Some(pos) = partial.iter().position(|b| *b == b'\n') {
                                let line: Vec<u8> = partial.drain(..=pos).collect();
                                let text = String::from_utf8_lossy(&line).trim().to_string();
                                if !text.is_empty() && tx.send(Ok(text)).is_err() {
                                    return;
                                }
                            }
                        }
                        Err(e) => {
                            let _ = tx.send(Err(e.to_string()));
                            return;
                        }
                    }
                }
            });
        }
    }
}

#[cfg(not(windows))]
mod pipe {
    use anyhow::{Result, bail};
    use std::sync::mpsc::Sender;
    pub struct Pipe;
    impl Pipe {
        pub fn open(_: &str) -> Result<Pipe> {
            bail!("the control channel is a Windows named pipe")
        }
        pub fn write_all(&self, _: &[u8]) -> Result<()> {
            bail!("unsupported")
        }
        pub fn spawn_reader(&self, _: Sender<Result<String, String>>) {}
        pub fn cancel(&self) {}
    }
}

impl Drop for Client {
    /// Unblocks the reader thread so the pipe handle closes with the client.
    fn drop(&mut self) {
        self.pipe.cancel();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn run_ids_fit_the_product_alphabet() {
        let id = new_run_id("exo");
        assert!((8..=64).contains(&id.len()));
        assert!(
            id.bytes()
                .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'))
        );
        assert_ne!(new_run_id("exo"), new_run_id("exo"));
    }

    #[test]
    fn pipe_names_follow_the_role_scheme() {
        assert_eq!(
            pipe_name("LiveVerify", "abc12345"),
            r"\\.\pipe\ExoSnap.LiveVerify.abc12345"
        );
    }
}
